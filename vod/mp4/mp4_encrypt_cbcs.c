#include "mp4_encrypt_cbcs.h"

#if (VOD_HAVE_OPENSSL_EVP)

#include "mp4_write_stream.h"
#include "mp4_defs.h"
#include "../write_buffer.h"
#include "../read_stream.h"
#include "../avc_parser.h"
#include "../avc_defs.h"
#include "../udrm.h"

// constants
#define MIN_ENCRYPTED_PACKET_SIZE (1 + AES_BLOCK_SIZE)		// minimum 1 byte for slice header
#define ENCRYPTED_BLOCK_PERIOD (10)							// 1 out of 10 blocks is encrypted
#define MAX_SLICE_HEADER_SIZE (80)
#define MP4_ENCRYPT_KEY_SIZE (16)

// typedefs
typedef struct {
	// fixed
	request_context_t* request_context;
	u_char iv[AES_BLOCK_SIZE];
	u_char key[MP4_ENCRYPT_KEY_SIZE];

	write_buffer_state_t write_buffer;
	EVP_CIPHER_CTX* cipher;
	uint32_t flush_left;
} mp4_encrypt_cbcs_state_t;

typedef struct {
	// fixed
	mp4_encrypt_cbcs_state_t* state;

	// frame state
	media_track_t* cur_track;
	media_track_t* last_track;
	uint32_t total_track_count;

	frame_list_part_t* cur_frame_part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t frame_size_left;
	uint32_t clear_trailer_size;		// only used in audio
} mp4_encrypt_cbcs_stream_state_t;

typedef struct {
	mp4_encrypt_cbcs_stream_state_t base;

	avc_parse_ctx_t avc_parse;
	uint32_t nal_packet_size_length;

	// nal packet state
	int cur_state;
	uint32_t length_bytes_left;
	uint32_t packet_size_left;
	uint32_t next_block_size_left;

	// slice header read state
	u_char slice_header[MAX_SLICE_HEADER_SIZE];
	u_char* slice_header_pos;
	u_char* slice_header_end;
} mp4_encrypt_cbcs_video_stream_state_t;

// enums
enum {
	// regular states
	STATE_PACKET_SIZE,
	STATE_NAL_TYPE,
	STATE_SLICE_HEADER,
	STATE_PACKET_ENCRYPT,
	STATE_PACKET_COPY,
};

////// base functions

static void
mp4_encrypt_cbcs_free_cipher(mp4_encrypt_cbcs_state_t* state)
{
	EVP_CIPHER_CTX_free(state->cipher);
}

static vod_status_t
mp4_encrypt_cbcs_init_cipher(mp4_encrypt_cbcs_state_t* state)
{
	vod_pool_cleanup_t *cln;
	request_context_t* request_context = state->request_context;

	// allocate cleanup item
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_cbcs_init_cipher: vod_pool_cleanup_add failed");
		return VOD_ALLOC_FAILED;
	}

	state->cipher = EVP_CIPHER_CTX_new();
	if (state->cipher == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_encrypt_cbcs_init_cipher: EVP_CIPHER_CTX_new failed");
		return VOD_ALLOC_FAILED;
	}

	cln->handler = (vod_pool_cleanup_pt)mp4_encrypt_cbcs_free_cipher;
	cln->data = state;

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_cbcs_reset_cipher(mp4_encrypt_cbcs_state_t* state)
{
	if (1 != EVP_EncryptInit_ex(state->cipher, EVP_aes_128_cbc(), NULL, state->key, state->iv))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_encrypt_cbcs_reset_cipher: EVP_EncryptInit_ex failed");
		return VOD_ALLOC_FAILED;
	}

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_cbcs_write_encrypted(
	mp4_encrypt_cbcs_state_t* state,
	u_char* buffer,
	uint32_t size)
{
	vod_status_t rc;
	u_char* output;
	size_t output_size;
	int written;

	rc = write_buffer_get_bytes(
		&state->write_buffer, 
		aes_round_up_to_block_exact(size), 
		&output_size, 
		&output);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (1 != EVP_EncryptUpdate(
		state->cipher,
		output,
		&written,
		buffer,
		size))
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_encrypt_cbcs_write_encrypted: EVP_EncryptUpdate failed");
		return VOD_UNEXPECTED;
	}

	state->write_buffer.cur_pos += written;

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_cbcs_flush(mp4_encrypt_cbcs_state_t* state)
{
	state->flush_left--;

	if (state->flush_left > 0)
	{
		return VOD_OK;
	}

	return write_buffer_flush(&state->write_buffer, FALSE);
}

////// common stream functions

static void
mp4_encrypt_cbcs_init_track(mp4_encrypt_cbcs_stream_state_t* stream_state)
{
	media_track_t* track = stream_state->cur_track;

	stream_state->cur_frame_part = &track->frames;
	stream_state->cur_frame = track->frames.first_frame;
	stream_state->last_frame = track->frames.last_frame;
	stream_state->frame_size_left = 0;
}

static void
mp4_encrypt_cbcs_init_stream_state(
	mp4_encrypt_cbcs_stream_state_t* stream_state,
	mp4_encrypt_cbcs_state_t* state,
	media_set_t* media_set,
	media_track_t* track)
{
	stream_state->state = state;

	// init the first clip
	stream_state->cur_track = track;
	stream_state->last_track = media_set->filtered_tracks + (media_set->total_track_count * media_set->clip_count);
	stream_state->total_track_count = media_set->total_track_count;
	mp4_encrypt_cbcs_init_track(stream_state);
}

static bool_t
mp4_encrypt_cbcs_move_to_next_frame(mp4_encrypt_cbcs_stream_state_t* stream_state, bool_t* init_track)
{
	for (;;)
	{
		if (stream_state->cur_frame < stream_state->last_frame)
		{
			if (stream_state->cur_frame->size > 0)
			{
				return TRUE;
			}

			stream_state->cur_frame++;
			continue;
		}

		if (stream_state->cur_frame_part->next != NULL)
		{
			stream_state->cur_frame_part = stream_state->cur_frame_part->next;
			stream_state->cur_frame = stream_state->cur_frame_part->first_frame;
			stream_state->last_frame = stream_state->cur_frame_part->last_frame;
			continue;
		}

		stream_state->cur_track += stream_state->total_track_count;
		if (stream_state->cur_track >= stream_state->last_track)
		{
			return FALSE;
		}

		mp4_encrypt_cbcs_init_track(stream_state);

		if (init_track != NULL)
		{
			*init_track = TRUE;
		}
	}
}

static vod_status_t
mp4_encrypt_cbcs_start_frame(mp4_encrypt_cbcs_stream_state_t* stream_state)
{
	// make sure we have a frame
	if (stream_state->cur_frame >= stream_state->last_frame)
	{
		vod_log_error(VOD_LOG_ERR, stream_state->state->request_context->log, 0,
			"mp4_encrypt_cbcs_start_frame: no more frames");
		return VOD_BAD_DATA;
	}

	// get the frame size
	stream_state->frame_size_left = stream_state->cur_frame->size;
	stream_state->cur_frame++;

	return VOD_OK;
}

////// video stream functions

static vod_status_t
mp4_encrypt_cbcs_video_init_track(mp4_encrypt_cbcs_video_stream_state_t* stream_state)
{
	mp4_encrypt_cbcs_state_t* state = stream_state->base.state;
	media_track_t* track = stream_state->base.cur_track;
	vod_status_t rc;

	rc = codec_config_avcc_get_nal_packet_size_length(
		state->request_context,
		&track->media_info.extra_data,
		&stream_state->nal_packet_size_length);
	if (rc != VOD_OK)
	{
		return rc;
	}

	if (stream_state->nal_packet_size_length < 1 || stream_state->nal_packet_size_length > 4)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"mp4_encrypt_cbcs_video_init_track: invalid nal packet size length %uD", 
			stream_state->nal_packet_size_length);
		return VOD_BAD_DATA;
	}

	rc = avc_parser_parse_extra_data(
		&stream_state->avc_parse,
		&track->media_info.extra_data);
	if (rc != VOD_OK)
	{
		return rc;
	}

	stream_state->cur_state = STATE_PACKET_SIZE;
	stream_state->length_bytes_left = stream_state->nal_packet_size_length;
	stream_state->packet_size_left = 0;
	stream_state->next_block_size_left = 0;

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_cbcs_video_write_buffer(void* context, u_char* buffer, uint32_t size)
{
	mp4_encrypt_cbcs_video_stream_state_t* stream_state = (mp4_encrypt_cbcs_video_stream_state_t*)context;
	mp4_encrypt_cbcs_state_t* state = stream_state->base.state;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	u_char* output;
	uint32_t slice_header_buf_size;
	uint32_t slice_header_size;
	uint32_t full_packet_size;
	uint32_t write_size;
	uint32_t size_left;
	int32_t cur_shift;
	uint8_t nal_type;
	bool_t init_track;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		switch (stream_state->cur_state)
		{
		case STATE_PACKET_SIZE:
			if (stream_state->base.frame_size_left <= 0)
			{
				// start a frame
				rc = mp4_encrypt_cbcs_start_frame(&stream_state->base);
				if (rc != VOD_OK)
				{
					return rc;
				}

				if (stream_state->base.frame_size_left <= stream_state->nal_packet_size_length)
				{
					vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
						"mp4_encrypt_cbcs_video_write_buffer: frame size %uD too small, nalu size %uD",
						stream_state->base.frame_size_left, stream_state->nal_packet_size_length);
					return VOD_BAD_DATA;
				}
			}

			// get the packet size
			for (; stream_state->length_bytes_left && cur_pos < buffer_end; stream_state->length_bytes_left--)
			{
				stream_state->packet_size_left = (stream_state->packet_size_left << 8) | *cur_pos++;
			}

			if (stream_state->length_bytes_left > 0)
			{
				break;
			}

			// validate the packet size
			if (stream_state->packet_size_left <= 0)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_encrypt_cbcs_video_write_buffer: zero size packet");
				return VOD_BAD_DATA;
			}

			if (stream_state->packet_size_left > UINT_MAX - stream_state->nal_packet_size_length)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_encrypt_cbcs_video_write_buffer: packet size %uD too big, nalu size %uD",
					stream_state->packet_size_left, stream_state->nal_packet_size_length);
				return VOD_BAD_DATA;
			}

			// update the frame size left
			full_packet_size = stream_state->nal_packet_size_length + stream_state->packet_size_left;
			if (stream_state->base.frame_size_left < full_packet_size)
			{
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_encrypt_cbcs_video_write_buffer: frame size %uD too small, nalu size %uD packet size %uD",
					stream_state->base.frame_size_left, stream_state->nal_packet_size_length, stream_state->packet_size_left);
				return VOD_BAD_DATA;
			}

			stream_state->base.frame_size_left -= full_packet_size;

			stream_state->cur_state = STATE_NAL_TYPE;

			if (cur_pos >= buffer_end)
			{
				break;
			}
			// fall through

		case STATE_NAL_TYPE:

			// write the packet size and nal type
			rc = write_buffer_get_bytes(&state->write_buffer, stream_state->nal_packet_size_length + 1, NULL, &output);
			if (rc != VOD_OK)
			{
				return rc;
			}

			for (cur_shift = (stream_state->nal_packet_size_length - 1) * 8; cur_shift >= 0; cur_shift -= 8)
			{
				*output++ = (stream_state->packet_size_left >> cur_shift) & 0xff;
			}

			nal_type = *cur_pos++;
			*output++ = nal_type;		// nal type

			// update the packet size
			stream_state->packet_size_left--;

			switch (nal_type & 0x1f)
			{
			case AVC_NAL_SLICE:
			case AVC_NAL_IDR_SLICE:
				if (stream_state->packet_size_left < MIN_ENCRYPTED_PACKET_SIZE)
				{
					stream_state->cur_state = STATE_PACKET_COPY;
					continue;
				}
				break;

			case AVC_NAL_DPA:
			case AVC_NAL_DPB:
			case AVC_NAL_DPC:
				vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
					"mp4_encrypt_cbcs_video_write_buffer: nal types 2-4 are not supported");
				return VOD_BAD_DATA;

				// TODO: parse in-stream SPS/PPS

			default:
				stream_state->cur_state = STATE_PACKET_COPY;
				continue;
			}

			// initialize the slice header state
			stream_state->slice_header[0] = nal_type;
			stream_state->slice_header_pos = stream_state->slice_header + 1;
			stream_state->slice_header_end = stream_state->slice_header + 
				vod_min(sizeof(stream_state->slice_header), stream_state->packet_size_left + 1);
			stream_state->cur_state = STATE_SLICE_HEADER;
			// fall through

		case STATE_SLICE_HEADER:
			// copy to the slice header buffer
			size_left = buffer_end - cur_pos;
			write_size = stream_state->slice_header_end - stream_state->slice_header_pos;
			write_size = vod_min(write_size, size_left);
			stream_state->slice_header_pos = vod_copy(stream_state->slice_header_pos, cur_pos, write_size);
			cur_pos += write_size;
			if (stream_state->slice_header_pos < stream_state->slice_header_end)
			{
				break;
			}

			// get the slice header size
			slice_header_buf_size = stream_state->slice_header_end - stream_state->slice_header;

			rc = avc_parser_get_slice_header_size(
				&stream_state->avc_parse,
				stream_state->slice_header,
				slice_header_buf_size,
				&slice_header_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			slice_header_size--;	// remove the nal type byte

			if (stream_state->packet_size_left - slice_header_size < AES_BLOCK_SIZE)
			{
				// packet data smaller than block
				rc = write_buffer_write(&state->write_buffer, stream_state->slice_header + 1, slice_header_buf_size - 1);
				if (rc != VOD_OK)
				{
					return rc;
				}

				stream_state->packet_size_left -= slice_header_buf_size - 1;
				stream_state->cur_state = STATE_PACKET_COPY;
				break;
			}

			// write the slice header
			stream_state->next_block_size_left = stream_state->packet_size_left - slice_header_size;

			rc = write_buffer_write(&state->write_buffer, stream_state->slice_header + 1, slice_header_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			// write the encrypted bytes
			rc = mp4_encrypt_cbcs_reset_cipher(state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			write_size = slice_header_buf_size - slice_header_size - 1;
			write_size = vod_min(AES_BLOCK_SIZE, write_size);
			rc = mp4_encrypt_cbcs_write_encrypted(
				state,
				stream_state->slice_header + 1 + slice_header_size,
				write_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			if (write_size >= AES_BLOCK_SIZE)
			{
				// write any remaining clear bytes in the slice header buffer
				rc = write_buffer_write(
					&state->write_buffer, 
					stream_state->slice_header + 1 + slice_header_size + AES_BLOCK_SIZE, 
					slice_header_buf_size - (1 + slice_header_size + AES_BLOCK_SIZE));
				if (rc != VOD_OK)
				{
					return rc;
				}

				// update the next block position
				if (stream_state->next_block_size_left >= (ENCRYPTED_BLOCK_PERIOD + 1) * AES_BLOCK_SIZE)
				{
					stream_state->next_block_size_left -= ENCRYPTED_BLOCK_PERIOD * AES_BLOCK_SIZE;
				}
				else
				{
					stream_state->next_block_size_left = 0;
				}
			}

			stream_state->packet_size_left -= slice_header_buf_size - 1;
			stream_state->cur_state = STATE_PACKET_ENCRYPT;
			// fallthrough

		case STATE_PACKET_ENCRYPT:
			if (stream_state->packet_size_left > 0 && 
				stream_state->packet_size_left <= stream_state->next_block_size_left)
			{
				// write encrypted bytes
				size_left = buffer_end - cur_pos;
				write_size = stream_state->packet_size_left - (stream_state->next_block_size_left - AES_BLOCK_SIZE);
				write_size = vod_min(write_size, size_left);

				rc = mp4_encrypt_cbcs_write_encrypted(
					state,
					cur_pos,
					write_size);
				if (rc != VOD_OK)
				{
					return rc;
				}

				cur_pos += write_size;
				stream_state->packet_size_left -= write_size;

				if (stream_state->packet_size_left > stream_state->next_block_size_left - AES_BLOCK_SIZE)
				{
					break;
				}

				// update the next block position
				if (stream_state->next_block_size_left >= (ENCRYPTED_BLOCK_PERIOD + 1) * AES_BLOCK_SIZE)
				{
					stream_state->next_block_size_left -= ENCRYPTED_BLOCK_PERIOD * AES_BLOCK_SIZE;
				}
				else
				{
					stream_state->next_block_size_left = 0;
				}
			}
			// fallthrough

		case STATE_PACKET_COPY:
			// write clear bytes
			size_left = buffer_end - cur_pos;
			write_size = stream_state->packet_size_left - stream_state->next_block_size_left;
			write_size = vod_min(write_size, size_left);

			rc = write_buffer_write(&state->write_buffer, cur_pos, write_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_pos += write_size;
			stream_state->packet_size_left -= write_size;
			if (stream_state->packet_size_left > 0)
			{
				break;
			}

			// finished a packet
			stream_state->cur_state = STATE_PACKET_SIZE;
			stream_state->length_bytes_left = stream_state->nal_packet_size_length;
			stream_state->packet_size_left = 0;

			if (stream_state->base.frame_size_left > 0)
			{
				break;
			}

			// move to the next frame
			init_track = FALSE;
			if (!mp4_encrypt_cbcs_move_to_next_frame(&stream_state->base, &init_track))
			{
				// finished all frames
				return mp4_encrypt_cbcs_flush(state);
			}

			if (init_track)
			{
				rc = mp4_encrypt_cbcs_video_init_track(stream_state);
				if (rc != VOD_OK)
				{
					return rc;
				}
			}

			break;
		}
	}

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_cbcs_video_get_fragment_writer(
	mp4_encrypt_cbcs_state_t* state,
	media_set_t* media_set,
	media_track_t* track,
	segment_writer_t* segment_writer)
{
	mp4_encrypt_cbcs_video_stream_state_t* stream_state;
	request_context_t* request_context = state->request_context;
	vod_status_t rc;

	// TODO: support hevc

	if (track->media_info.codec_id != VOD_CODEC_ID_AVC)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"mp4_encrypt_cbcs_video_get_fragment_writer: codec id %uD is not supported", 
			track->media_info.codec_id);
		return VOD_BAD_REQUEST;
	}

	// allocate the state
	stream_state = vod_alloc(request_context->pool, sizeof(*stream_state));
	if (stream_state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_cbcs_video_get_fragment_writer: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	rc = avc_parser_init_ctx(&stream_state->avc_parse, request_context);
	if (rc != VOD_OK)
	{
		return rc;
	}

	mp4_encrypt_cbcs_init_stream_state(
		&stream_state->base,
		state,
		media_set, 
		track);

	segment_writer->write_tail = mp4_encrypt_cbcs_video_write_buffer;
	segment_writer->write_head = NULL;
	segment_writer->context = stream_state;

	// init writing for the first track
	if (!mp4_encrypt_cbcs_move_to_next_frame(&stream_state->base, NULL))
	{
		return VOD_OK;
	}

	rc = mp4_encrypt_cbcs_video_init_track(stream_state);
	if (rc != VOD_OK)
	{
		return rc;
	}

	return VOD_OK;
}

////// audio fragment functions

static vod_status_t
mp4_encrypt_cbcs_audio_write_buffer(void* context, u_char* buffer, uint32_t size)
{
	mp4_encrypt_cbcs_stream_state_t* stream_state = (mp4_encrypt_cbcs_stream_state_t*)context;
	mp4_encrypt_cbcs_state_t* state = stream_state->state;
	u_char* buffer_end = buffer + size;
	u_char* cur_pos = buffer;
	uint32_t write_size;
	uint32_t size_left;
	vod_status_t rc;

	while (cur_pos < buffer_end)
	{
		if (stream_state->frame_size_left <= 0)
		{
			// start a frame
			rc = mp4_encrypt_cbcs_start_frame(stream_state);
			if (rc != VOD_OK)
			{
				return rc;
			}

			stream_state->clear_trailer_size = stream_state->frame_size_left & 0xf;

			rc = mp4_encrypt_cbcs_reset_cipher(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}

		if (stream_state->frame_size_left > stream_state->clear_trailer_size)
		{
			// encrypted part
			size_left = buffer_end - cur_pos;
			write_size = stream_state->frame_size_left - stream_state->clear_trailer_size;
			write_size = vod_min(write_size, size_left);

			rc = mp4_encrypt_cbcs_write_encrypted(
				state,
				cur_pos,
				write_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_pos += write_size;
			stream_state->frame_size_left -= write_size;
		}

		if (stream_state->frame_size_left <= stream_state->clear_trailer_size)
		{
			// clear trailer
			size_left = buffer_end - cur_pos;
			write_size = vod_min(size_left, stream_state->frame_size_left);

			rc = write_buffer_write(&state->write_buffer, cur_pos, write_size);
			if (rc != VOD_OK)
			{
				return rc;
			}

			cur_pos += write_size;
			stream_state->frame_size_left -= write_size;
		}

		if (stream_state->frame_size_left > 0)
		{
			break;
		}

		// finished a frame
		if (!mp4_encrypt_cbcs_move_to_next_frame(stream_state, NULL))
		{
			return mp4_encrypt_cbcs_flush(state);
		}
	}

	return VOD_OK;
}

static vod_status_t
mp4_encrypt_cbcs_audio_get_fragment_writer(
	mp4_encrypt_cbcs_state_t* state,
	media_set_t* media_set,
	media_track_t* track,
	segment_writer_t* segment_writer)
{
	mp4_encrypt_cbcs_stream_state_t* stream_state;
	request_context_t* request_context = state->request_context;

	// allocate the state
	stream_state = vod_alloc(request_context->pool, sizeof(*stream_state));
	if (stream_state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_cbcs_audio_get_fragment_writer: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	mp4_encrypt_cbcs_init_stream_state(
		stream_state, 
		state,
		media_set, 
		track);

	segment_writer->write_tail = mp4_encrypt_cbcs_audio_write_buffer;
	segment_writer->write_head = NULL;
	segment_writer->context = stream_state;

	mp4_encrypt_cbcs_move_to_next_frame(stream_state, NULL);	// ignore the result

	return VOD_OK;
}

vod_status_t
mp4_encrypt_cbcs_get_writers(
	request_context_t* request_context,
	media_set_t* media_set,
	segment_writer_t* segment_writer,
	const u_char* key,
	const u_char* iv,
	segment_writer_t** result)
{
	mp4_encrypt_cbcs_state_t* state;
	segment_writer_t* cur_segment_writer;
	segment_writer_t* segment_writers;
	media_track_t* cur_track;
	vod_status_t rc;
	uint32_t i;

	// allocate the state and writers
	state = vod_alloc(request_context->pool,
		sizeof(*state) + 
		sizeof(segment_writers[0]) * media_set->total_track_count);
	if (state == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"mp4_encrypt_cbcs_get_writers: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	segment_writers = (void*)(state + 1);

	// initialize the state
	state->request_context = request_context;

	rc = mp4_encrypt_cbcs_init_cipher(state);
	if (rc != VOD_OK)
	{
		return rc;
	}

	write_buffer_init(
		&state->write_buffer,
		request_context,
		segment_writer->write_tail,
		segment_writer->context,
		FALSE);

	vod_memcpy(state->iv, iv, sizeof(state->iv));
	vod_memcpy(state->key, key, sizeof(state->key));
	state->flush_left = media_set->total_track_count;

	for (i = 0; i < media_set->total_track_count; i++)
	{
		cur_segment_writer = &segment_writers[i];

		// get a writer for the current track
		cur_track = &media_set->filtered_tracks[i];
		switch (cur_track->media_info.media_type)
		{
		case MEDIA_TYPE_VIDEO:
			rc = mp4_encrypt_cbcs_video_get_fragment_writer(
				state,
				media_set,
				cur_track,
				cur_segment_writer);
			break;

		case MEDIA_TYPE_AUDIO:
			rc = mp4_encrypt_cbcs_audio_get_fragment_writer(
				state,
				media_set,
				cur_track,
				cur_segment_writer);
			break;
		}

		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	*result = segment_writers;
	return VOD_OK;
}

#else

// empty stubs
vod_status_t
mp4_encrypt_cbcs_get_writers(
	request_context_t* request_context,
	media_set_t* media_set,
	segment_writer_t* segment_writer,
	const u_char* key,
	const u_char* iv,
	segment_writer_t** result)
{
	return VOD_UNEXPECTED;
}

#endif //(VOD_HAVE_OPENSSL_EVP)
