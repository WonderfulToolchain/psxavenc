/*
psxavenc: MDEC video + SPU/XA-ADPCM audio encoder frontend

Copyright (c) 2019, 2020 Adrian "asie" Siekierka
Copyright (c) 2019 Ben "GreaseMonkey" Russell
Copyright (c) 2023, 2025 spicyjpeg

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <libpsxav.h>
#include "args.h"
#include "decoding.h"
#include "mdec.h"

static time_t start_time = 0;
static time_t last_progress_update = 0;

static time_t get_elapsed_time(void) {
	time_t t;

	if (start_time > 0) {
		t = time(NULL) - start_time;
	} else {
		t = 0;
		start_time = time(NULL);
	}

	if (t <= last_progress_update)
		return 0;

	last_progress_update = t;
	return t;
}

static psx_audio_xa_settings_t args_to_libpsxav_xa_audio(const args_t *args) {
	psx_audio_xa_settings_t settings;

	settings.bits_per_sample = args->audio_bit_depth;
	settings.frequency = args->audio_frequency;
	settings.stereo = (args->audio_channels == 2);
	settings.file_number = args->audio_xa_file;
	settings.channel_number = args->audio_xa_channel;

	if (args->format == FORMAT_XACD || args->format == FORMAT_STRCD)
		settings.format = PSX_AUDIO_XA_FORMAT_XACD;
	else
		settings.format = PSX_AUDIO_XA_FORMAT_XA;

	return settings;
};

static void init_sector_buffer_video(const args_t *args, uint8_t *sector, int lba) {
	psx_cdrom_sector_xa_subheader_t *subheader = NULL;

	if (args->format == FORMAT_STRCD) {
		psx_cdrom_init_sector((psx_cdrom_sector_t *)sector, lba, PSX_CDROM_SECTOR_TYPE_MODE2_FORM1);
		subheader = ((psx_cdrom_sector_t *)sector)->mode2.subheader;
	} else if (args->format == FORMAT_STR) {
		subheader = (psx_cdrom_sector_xa_subheader_t *)sector;
	}

	if (subheader != NULL) {
		subheader->file = args->audio_xa_file;
		subheader->channel = args->audio_xa_channel & PSX_CDROM_SECTOR_XA_CHANNEL_MASK;
		subheader->submode = PSX_CDROM_SECTOR_XA_SUBMODE_DATA | PSX_CDROM_SECTOR_XA_SUBMODE_RT;
		subheader->coding = 0;

		memcpy(subheader + 1, subheader, sizeof(psx_cdrom_sector_xa_subheader_t));
	}
}

#define VAG_HEADER_SIZE 0x30

static void write_vag_header(const args_t *args, int size_per_channel, uint8_t *header) {
	memset(header, 0, VAG_HEADER_SIZE);

	// Magic
	header[0x00] = 'V';
	header[0x01] = 'A';
	header[0x02] = 'G';

	if (args->format == FORMAT_VAGI)
		header[0x03] = 'i';
	else
	 	header[0x03] = 'p';

	// Version (big endian)
	header[0x04] = 0x00;
	header[0x05] = 0x00;
	header[0x06] = 0x00;
	header[0x07] = 0x20;

	// Interleave (little endian)
	if (args->format == FORMAT_VAGI) {
		header[0x08] = (uint8_t)args->audio_interleave;
		header[0x09] = (uint8_t)(args->audio_interleave >> 8);
		header[0x0A] = (uint8_t)(args->audio_interleave >> 16);
		header[0x0B] = (uint8_t)(args->audio_interleave >> 24);
	}

	// Length of data for each channel (big endian)
	header[0x0C] = (uint8_t)(size_per_channel >> 24);
	header[0x0D] = (uint8_t)(size_per_channel >> 16);
	header[0x0E] = (uint8_t)(size_per_channel >> 8);
	header[0x0F] = (uint8_t)size_per_channel;

	// Sample rate (big endian)
	header[0x10] = (uint8_t)(args->audio_frequency >> 24);
	header[0x11] = (uint8_t)(args->audio_frequency >> 16);
	header[0x12] = (uint8_t)(args->audio_frequency >> 8);
	header[0x13] = (uint8_t)args->audio_frequency;

	// Loop point in bytes (big endian, non-standard)
	if (args->format == FORMAT_VAGI && args->audio_loop_point >= 0) {
		int loop_start_block = (args->audio_loop_point * args->audio_frequency) / (PSX_AUDIO_SPU_SAMPLES_PER_BLOCK * 1000);

		if (!(args->flags & FLAG_SPU_NO_LEADING_DUMMY))
			loop_start_block++;

		int loop_point = loop_start_block * PSX_AUDIO_SPU_BLOCK_SIZE;
		header[0x14] = (uint8_t)(loop_point >> 24);
		header[0x15] = (uint8_t)(loop_point >> 16);
		header[0x16] = (uint8_t)(loop_point >> 8);
		header[0x17] = (uint8_t)loop_point;
	}

	// Number of channels (non-standard)
	header[0x1E] = (uint8_t)args->audio_channels;

	// Filename
	int name_offset = strlen(args->output_file);

	while (
		name_offset > 0 &&
		args->output_file[name_offset - 1] != '/' &&
		args->output_file[name_offset - 1] != '\\'
	)
		name_offset--;

	strncpy((char*)(header + 0x20), &args->output_file[name_offset], 16);
}

// The functions below are some peak spaghetti code I would rewrite if that
// didn't also require scrapping the rest of the codebase. -- spicyjpeg

void encode_file_xa(const args_t *args, decoder_t *decoder, FILE *output) {
	psx_audio_xa_settings_t xa_settings = args_to_libpsxav_xa_audio(args);

	int audio_samples_per_sector = psx_audio_xa_get_samples_per_sector(xa_settings);

	psx_audio_encoder_state_t audio_state;
	memset(&audio_state, 0, sizeof(psx_audio_encoder_state_t));

	int sector_count = 0;

	for (; ensure_av_data(decoder, audio_samples_per_sector * args->audio_channels, 0); sector_count++) {
		int samples_length = decoder->audio_sample_count / args->audio_channels;

		if (samples_length > audio_samples_per_sector)
			samples_length = audio_samples_per_sector;

		uint8_t sector[PSX_CDROM_SECTOR_SIZE];
		int length = psx_audio_xa_encode(
			xa_settings,
			&audio_state,
			decoder->audio_samples,
			samples_length,
			sector_count,
			sector
		);

		if (decoder->end_of_input)
			psx_audio_xa_encode_finalize(xa_settings, sector, length);

		retire_av_data(decoder, samples_length * args->audio_channels, 0);
		fwrite(sector, length, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rLBA: %6d | Encoding speed: %5.2fx",
				sector_count,
				(double)(sector_count * audio_samples_per_sector) / (double)(args->audio_frequency * t)
			);
		}
	}
}

void encode_file_spu(const args_t *args, decoder_t *decoder, FILE *output) {
	psx_audio_encoder_channel_state_t audio_state;
	memset(&audio_state, 0, sizeof(psx_audio_encoder_channel_state_t));

	// The header must be written after the data as we don't yet know the
	// number of audio samples.
	if (args->format == FORMAT_VAG)
		fseek(output, VAG_HEADER_SIZE, SEEK_SET);

	uint8_t block[PSX_AUDIO_SPU_BLOCK_SIZE];
	int block_count = 0;

	if (!(args->flags & FLAG_SPU_NO_LEADING_DUMMY)) {
		// Insert leading silent block
		memset(block, 0, PSX_AUDIO_SPU_BLOCK_SIZE);

		fwrite(block, PSX_AUDIO_SPU_BLOCK_SIZE, 1, output);
		block_count++;
	}

	int loop_start_block = -1;

	if (args->audio_loop_point >= 0)
		loop_start_block = block_count + (args->audio_loop_point * args->audio_frequency) / (PSX_AUDIO_SPU_SAMPLES_PER_BLOCK * 1000);

	for (; ensure_av_data(decoder, PSX_AUDIO_SPU_SAMPLES_PER_BLOCK, 0); block_count++) {
		int samples_length = decoder->audio_sample_count;

		if (samples_length > PSX_AUDIO_SPU_SAMPLES_PER_BLOCK)
			samples_length = PSX_AUDIO_SPU_SAMPLES_PER_BLOCK;

		int length = psx_audio_spu_encode(
			&audio_state,
			decoder->audio_samples,
			samples_length,
			1,
			block
		);

		if (block_count == loop_start_block)
			block[1] |= PSX_AUDIO_SPU_LOOP_START;
		if ((args->flags & FLAG_SPU_ENABLE_LOOP) && decoder->end_of_input)
			block[1] |= PSX_AUDIO_SPU_LOOP_REPEAT;

		retire_av_data(decoder, samples_length, 0);
		fwrite(block, length, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rBlock: %6d | Encoding speed: %5.2fx",
				block_count,
				(double)(block_count * PSX_AUDIO_SPU_SAMPLES_PER_BLOCK) / (double)(args->audio_frequency * t)
			);
		}
	}

	if (!(args->flags & FLAG_SPU_ENABLE_LOOP)) {
		// Insert trailing looping block
		memset(block, 0, PSX_AUDIO_SPU_BLOCK_SIZE);
		block[1] = PSX_AUDIO_SPU_LOOP_TRAP;

		fwrite(block, PSX_AUDIO_SPU_BLOCK_SIZE, 1, output);
		block_count++;
	}

	int overflow = (block_count * PSX_AUDIO_SPU_BLOCK_SIZE) % args->alignment;

	if (overflow) {
		for (int i = 0; i < (args->alignment - overflow); i++)
			fputc(0, output);
	}
	if (args->format == FORMAT_VAG) {
		uint8_t header[VAG_HEADER_SIZE];
		write_vag_header(args, block_count * PSX_AUDIO_SPU_BLOCK_SIZE, header);

		fseek(output, 0, SEEK_SET);
		fwrite(header, VAG_HEADER_SIZE, 1, output);
	}
}

void encode_file_spui(const args_t *args, decoder_t *decoder, FILE *output) {
	int audio_samples_per_chunk = args->audio_interleave / PSX_AUDIO_SPU_BLOCK_SIZE * PSX_AUDIO_SPU_SAMPLES_PER_BLOCK;

	// NOTE: since the interleaved .vag format is not standardized, some tools
	// (such as vgmstream) will not properly play files with interleave < 2048,
	// alignment != 2048 or channels != 2.
	int chunk_size = args->audio_interleave * args->audio_channels + args->alignment - 1;
	chunk_size -= chunk_size % args->alignment;

	int header_size = VAG_HEADER_SIZE + args->alignment - 1;
	header_size -= header_size % args->alignment;

	if (args->format == FORMAT_VAGI)
		fseek(output, header_size, SEEK_SET);
	else if (args->audio_loop_point >= 0 && !(args->flags & FLAG_QUIET))
		fprintf(stderr, "Warning: ignoring loop point as there is no header to store it in\n");

	int audio_state_size = sizeof(psx_audio_encoder_channel_state_t) * args->audio_channels;
	psx_audio_encoder_channel_state_t *audio_state = malloc(audio_state_size);
	memset(audio_state, 0, audio_state_size);

	uint8_t *chunk = malloc(chunk_size);
	int chunk_count = 0;

	for (; ensure_av_data(decoder, audio_samples_per_chunk * args->audio_channels, 0); chunk_count++) {
		int samples_length = decoder->audio_sample_count / args->audio_channels;

		if (samples_length > audio_samples_per_chunk)
			samples_length = audio_samples_per_chunk;

		memset(chunk, 0, chunk_size);
		uint8_t *chunk_ptr = chunk;

		// Insert leading silent block
		if (chunk_count == 0 && !(args->flags & FLAG_SPU_NO_LEADING_DUMMY)) {
			chunk_ptr += PSX_AUDIO_SPU_BLOCK_SIZE;
			samples_length -= PSX_AUDIO_SPU_SAMPLES_PER_BLOCK;
		}

		for (int ch = 0; ch < args->audio_channels; ch++, chunk_ptr += args->audio_interleave) {
			int length = psx_audio_spu_encode(
				audio_state + ch,
				decoder->audio_samples + ch,
				samples_length,
				args->audio_channels,
				chunk_ptr
			);

			if (length > 0) {
				uint8_t *last_block = chunk_ptr + length - PSX_AUDIO_SPU_BLOCK_SIZE;

				if (
					(args->flags & FLAG_SPU_ENABLE_LOOP) ||
					(decoder->end_of_input && args->audio_loop_point >= 0)
				) {
					last_block[1] = PSX_AUDIO_SPU_LOOP_REPEAT;
				} else if (decoder->end_of_input) {
					// HACK: the trailing block should in theory be appended to
					// the existing data, but it's easier to just zerofill and
					// repurpose the last encoded block.
					memset(last_block, 0, PSX_AUDIO_SPU_BLOCK_SIZE);
					last_block[1] = PSX_AUDIO_SPU_LOOP_TRAP;
				}
			}
		}

		retire_av_data(decoder, samples_length * args->audio_channels, 0);
		fwrite(chunk, chunk_size, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rChunk: %6d | Encoding speed: %5.2fx",
				chunk_count,
				(double)(chunk_count * audio_samples_per_chunk) / (double)(args->audio_frequency * t)
			);
		}

	}

	free(audio_state);
	free(chunk);

	if (args->format == FORMAT_VAGI) {
		uint8_t *header = malloc(header_size);
		memset(header, 0, header_size);
		write_vag_header(args, chunk_count * args->audio_interleave, header);

		fseek(output, 0, SEEK_SET);
		fwrite(header, header_size, 1, output);
		free(header);
	}
}

void encode_file_str(const args_t *args, decoder_t *decoder, FILE *output) {
	psx_audio_xa_settings_t xa_settings = args_to_libpsxav_xa_audio(args);
	int sector_size = psx_audio_xa_get_buffer_size_per_sector(xa_settings);

	int interleave;
	int audio_samples_per_sector;
	int video_sectors_per_block;

	if (decoder->state.audio_stream != NULL) {
		// 1/N audio, (N-1)/N video
		interleave = psx_audio_xa_get_sector_interleave(xa_settings) * args->str_cd_speed;
		audio_samples_per_sector = psx_audio_xa_get_samples_per_sector(xa_settings);
		video_sectors_per_block = interleave - 1;

		if (!(args->flags & FLAG_QUIET))
			fprintf(
				stderr,
				"Interleave: %d/%d audio, %d/%d video\n",
				interleave - video_sectors_per_block,
				interleave,
				video_sectors_per_block,
				interleave
			);
	} else {
		// 0/1 audio, 1/1 video
		interleave = 1;
		audio_samples_per_sector = 0;
		video_sectors_per_block = 1;
	}

	psx_audio_encoder_state_t audio_state;
	memset(&audio_state, 0, sizeof(psx_audio_encoder_state_t));

	mdec_encoder_t encoder;
	init_mdec_encoder(&encoder, args->video_codec, args->video_width, args->video_height);

	// e.g. 15fps = (150*7/8/15) = 8.75 blocks per frame
	encoder.state.frame_block_base_overflow = (75 * args->str_cd_speed) * video_sectors_per_block * args->str_fps_den;
	encoder.state.frame_block_overflow_den = interleave * args->str_fps_num;
	double frame_size = (double)encoder.state.frame_block_base_overflow / (double)encoder.state.frame_block_overflow_den;

	if (!(args->flags & FLAG_QUIET))
		fprintf(stderr, "Frame size: %.2f sectors\n", frame_size);

	encoder.state.frame_output = malloc(2016 * (int)ceil(frame_size));
	encoder.state.frame_index = 0;
	encoder.state.frame_data_offset = 0;
	encoder.state.frame_max_size = 0;
	encoder.state.frame_block_overflow_num = 0;
	encoder.state.quant_scale_sum = 0;

	// FIXME: this needs an extra frame to prevent A/V desync
	int frames_needed = (int)ceil((double)video_sectors_per_block / frame_size);

	if (frames_needed < 2)
		frames_needed = 2;

	int sector_count = 0;

	for (; !decoder->end_of_input || encoder.state.frame_data_offset < encoder.state.frame_max_size; sector_count++) {
		ensure_av_data(decoder, audio_samples_per_sector * args->audio_channels, frames_needed);

		uint8_t sector[PSX_CDROM_SECTOR_SIZE];
		bool is_video_sector;

		if (audio_samples_per_sector == 0)
			is_video_sector = true;
		else if (args->flags & FLAG_STR_TRAILING_AUDIO)
			is_video_sector = (sector_count % interleave) < video_sectors_per_block;
		else
			is_video_sector = (sector_count % interleave) > 0;

		if (is_video_sector) {
			init_sector_buffer_video(args, sector, sector_count);

			int frames_used = encode_sector_str(
				&encoder,
				args->format,
				args->str_video_id,
				decoder->video_frames,
				sector
			);

			psx_cdrom_calculate_checksums((psx_cdrom_sector_t *)sector, PSX_CDROM_SECTOR_TYPE_MODE2_FORM1);
			retire_av_data(decoder, 0, frames_used);
		} else {
			int samples_length = decoder->audio_sample_count / args->audio_channels;

			if (samples_length > audio_samples_per_sector)
				samples_length = audio_samples_per_sector;

			// FIXME: this is an extremely hacky way to handle audio tracks
			// shorter than the video track
			if (!samples_length)
				video_sectors_per_block++;

			int length = psx_audio_xa_encode(
				xa_settings,
				&audio_state,
				decoder->audio_samples,
				samples_length,
				sector_count,
				sector
			);

			if (decoder->end_of_input)
				psx_audio_xa_encode_finalize(xa_settings, sector, length);

			retire_av_data(decoder, samples_length * args->audio_channels, 0);
		}

		fwrite(sector, sector_size, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rFrame: %4d | LBA: %6d | Avg. q. scale: %5.2f | Encoding speed: %5.2fx",
				encoder.state.frame_index,
				sector_count,
				(double)encoder.state.quant_scale_sum / (double)encoder.state.frame_index,
				(double)(encoder.state.frame_index * args->str_fps_den) / (double)(t * args->str_fps_num)
			);
		}
	}

	free(encoder.state.frame_output);
	destroy_mdec_encoder(&encoder);
}

void encode_file_strspu(const args_t *args, decoder_t *decoder, FILE *output) {
	int interleave;
	int audio_samples_per_sector;
	int video_sectors_per_block;

	if (decoder->state.audio_stream != NULL) {
		assert(false); // TODO: implement

		if (!(args->flags & FLAG_QUIET))
			fprintf(
				stderr,
				"Interleave: %d/%d audio, %d/%d video\n",
				interleave - video_sectors_per_block,
				interleave,
				video_sectors_per_block,
				interleave
			);
	} else {
		// 0/1 audio, 1/1 video
		interleave = 1;
		audio_samples_per_sector = 0;
		video_sectors_per_block = 1;
	}

	mdec_encoder_t encoder;
	init_mdec_encoder(&encoder, args->video_codec, args->video_width, args->video_height);

	// e.g. 15fps = (150*7/8/15) = 8.75 blocks per frame
	encoder.state.frame_block_base_overflow = (75 * args->str_cd_speed) * video_sectors_per_block * args->str_fps_den;
	encoder.state.frame_block_overflow_den = interleave * args->str_fps_num;
	double frame_size = (double)encoder.state.frame_block_base_overflow / (double)encoder.state.frame_block_overflow_den;

	if (!(args->flags & FLAG_QUIET))
		fprintf(stderr, "Frame size: %.2f sectors\n", frame_size);

	encoder.state.frame_output = malloc(2016 * (int)ceil(frame_size));
	encoder.state.frame_index = 0;
	encoder.state.frame_data_offset = 0;
	encoder.state.frame_max_size = 0;
	encoder.state.frame_block_overflow_num = 0;
	encoder.state.quant_scale_sum = 0;

	// FIXME: this needs an extra frame to prevent A/V desync
	int frames_needed = (int)ceil((double)video_sectors_per_block / frame_size);

	if (frames_needed < 2)
		frames_needed = 2;

	int sector_count = 0;

	for (; !decoder->end_of_input || encoder.state.frame_data_offset < encoder.state.frame_max_size; sector_count++) {
		ensure_av_data(decoder, audio_samples_per_sector * args->audio_channels, frames_needed);

		uint8_t sector[2048];
		bool is_video_sector;

		if (audio_samples_per_sector == 0)
			is_video_sector = true;
		else if (args->flags & FLAG_STR_TRAILING_AUDIO)
			is_video_sector = (sector_count % interleave) < video_sectors_per_block;
		else
			is_video_sector = (sector_count % interleave) > 0;

		if (is_video_sector) {
			init_sector_buffer_video(args, sector, sector_count);

			int frames_used = encode_sector_str(
				&encoder,
				args->format,
				args->str_video_id,
				decoder->video_frames,
				sector
			);

			retire_av_data(decoder, 0, frames_used);
		} else {
			int samples_length = decoder->audio_sample_count / args->audio_channels;

			if (samples_length > audio_samples_per_sector)
				samples_length = audio_samples_per_sector;

			// FIXME: this is an extremely hacky way to handle audio tracks
			// shorter than the video track
			if (!samples_length)
				video_sectors_per_block++;

			assert(false); // TODO: implement

			retire_av_data(decoder, samples_length * args->audio_channels, 0);
		}

		fwrite(sector, 2048, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rFrame: %4d | LBA: %6d | Avg. q. scale: %5.2f | Encoding speed: %5.2fx",
				encoder.state.frame_index,
				sector_count,
				(double)encoder.state.quant_scale_sum / (double)encoder.state.frame_index,
				(double)(encoder.state.frame_index * args->str_fps_den) / (double)(t * args->str_fps_num)
			);
		}
	}

	free(encoder.state.frame_output);
	destroy_mdec_encoder(&encoder);
}

void encode_file_sbs(const args_t *args, decoder_t *decoder, FILE *output) {
	mdec_encoder_t encoder;
	init_mdec_encoder(&encoder, args->video_codec, args->video_width, args->video_height);

	encoder.state.frame_output = malloc(args->alignment);
	encoder.state.frame_data_offset = 0;
	encoder.state.frame_max_size = args->alignment;
	encoder.state.quant_scale_sum = 0;

	for (int j = 0; ensure_av_data(decoder, 0, 1); j++) {
		encode_frame_bs(&encoder, decoder->video_frames);

		retire_av_data(decoder, 0, 1);
		fwrite(encoder.state.frame_output, args->alignment, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rFrame: %4d | Avg. q. scale: %5.2f | Encoding speed: %5.2fx",
				j,
				(double)encoder.state.quant_scale_sum / (double)j,
				(double)(j * args->str_fps_den) / (double)(t * args->str_fps_num)
			);
		}
	}

	free(encoder.state.frame_output);
	destroy_mdec_encoder(&encoder);
}
