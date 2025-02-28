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

static void init_sector_buffer_video(const args_t *args, psx_cdrom_sector_mode2_t *sector, int lba) {
	psx_cdrom_init_sector((psx_cdrom_sector_t *)sector, lba, PSX_CDROM_SECTOR_TYPE_MODE2_FORM1);

	sector->subheader[0].file = args->audio_xa_file;
	sector->subheader[0].channel = args->audio_xa_channel & PSX_CDROM_SECTOR_XA_CHANNEL_MASK;
	sector->subheader[0].submode = PSX_CDROM_SECTOR_XA_SUBMODE_DATA | PSX_CDROM_SECTOR_XA_SUBMODE_RT;
	sector->subheader[0].coding = 0;

	memcpy(sector->subheader + 1, sector->subheader, sizeof(psx_cdrom_sector_xa_subheader_t));
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

	// Version (big-endian)
	header[0x04] = 0x00;
	header[0x05] = 0x00;
	header[0x06] = 0x00;
	header[0x07] = 0x20;

	// Interleave (little-endian)
	if (args->format == FORMAT_VAGI) {
		header[0x08] = (uint8_t)args->audio_interleave;
		header[0x09] = (uint8_t)(args->audio_interleave >> 8);
		header[0x0a] = (uint8_t)(args->audio_interleave >> 16);
		header[0x0b] = (uint8_t)(args->audio_interleave >> 24);
	}

	// Length of data for each channel (big-endian)
	header[0x0c] = (uint8_t)(size_per_channel >> 24);
	header[0x0d] = (uint8_t)(size_per_channel >> 16);
	header[0x0e] = (uint8_t)(size_per_channel >> 8);
	header[0x0f] = (uint8_t)size_per_channel;

	// Sample rate (big-endian)
	header[0x10] = (uint8_t)(args->audio_frequency >> 24);
	header[0x11] = (uint8_t)(args->audio_frequency >> 16);
	header[0x12] = (uint8_t)(args->audio_frequency >> 8);
	header[0x13] = (uint8_t)args->audio_frequency;

	// Number of channels (little-endian)
	header[0x1e] = (uint8_t)args->audio_channels;
	header[0x1f] = 0x00;

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

void encode_file_xa(args_t *args, decoder_t *decoder, FILE *output) {
	psx_audio_xa_settings_t xa_settings = args_to_libpsxav_xa_audio(args);

	int audio_samples_per_sector = psx_audio_xa_get_samples_per_sector(xa_settings);

	psx_audio_encoder_state_t audio_state;
	memset(&audio_state, 0, sizeof(psx_audio_encoder_state_t));

	for (int j = 0; ensure_av_data(decoder, audio_samples_per_sector * args->audio_channels, 0); j++) {
		int samples_length = decoder->audio_sample_count / args->audio_channels;

		if (samples_length > audio_samples_per_sector)
			samples_length = audio_samples_per_sector;

		uint8_t buffer[PSX_CDROM_SECTOR_SIZE];
		int length = psx_audio_xa_encode(
			xa_settings,
			&audio_state,
			decoder->audio_samples,
			samples_length,
			buffer
		);

		if (decoder->end_of_input)
			psx_audio_xa_encode_finalize(xa_settings, buffer, length);

		if (args->format == FORMAT_XACD) {
			int t = j + 75*2;

			// Put the time in
			buffer[0x00C] = ((t/75/60)%10)|(((t/75/60)/10)<<4);
			buffer[0x00D] = (((t/75)%60)%10)|((((t/75)%60)/10)<<4);
			buffer[0x00E] = ((t%75)%10)|(((t%75)/10)<<4);
		}

		retire_av_data(decoder, samples_length * args->audio_channels, 0);
		fwrite(buffer, length, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rLBA: %6d | Encoding speed: %5.2fx",
				j,
				(double)(j * audio_samples_per_sector) / (double)(args->audio_frequency * t)
			);
		}
	}
}

void encode_file_spu(args_t *args, decoder_t *decoder, FILE *output) {
	psx_audio_encoder_channel_state_t audio_state;
	memset(&audio_state, 0, sizeof(psx_audio_encoder_channel_state_t));

	int audio_samples_per_block = psx_audio_spu_get_samples_per_block();
	int block_size = psx_audio_spu_get_buffer_size_per_block();
	int block_count;

	// The header must be written after the data as we don't yet know the
	// number of audio samples.
	if (args->format == FORMAT_VAG)
		fseek(output, VAG_HEADER_SIZE, SEEK_SET);

	for (block_count = 0; ensure_av_data(decoder, audio_samples_per_block, 0); block_count++) {
		int samples_length = decoder->audio_sample_count;

		if (samples_length > audio_samples_per_block)
			samples_length = audio_samples_per_block;

		uint8_t buffer[16];
		int length = psx_audio_spu_encode(
			&audio_state,
			decoder->audio_samples,
			samples_length,
			1,
			buffer
		);

		// TODO: implement proper loop flag support
		if (false)
			buffer[1] |= PSX_AUDIO_SPU_LOOP_START;
		if (decoder->end_of_input) {
			if (args->flags & FLAG_SPU_LOOP_END)
				buffer[1] |= PSX_AUDIO_SPU_LOOP_REPEAT;
			else
			 	buffer[1] |= PSX_AUDIO_SPU_LOOP_END;
		}

		retire_av_data(decoder, samples_length, 0);
		fwrite(buffer, length, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rBlock: %6d | Encoding speed: %5.2fx",
				block_count,
				(double)(block_count * audio_samples_per_block) / (double)(args->audio_frequency * t)
			);
		}
	}

	int overflow = (block_count * block_size) % args->alignment;

	if (overflow) {
		for (int i = 0; i < (args->alignment - overflow); i++)
			fputc(0, output);
	}
	if (args->format == FORMAT_VAG) {
		uint8_t header[VAG_HEADER_SIZE];
		write_vag_header(args, block_count * block_size, header);

		fseek(output, 0, SEEK_SET);
		fwrite(header, VAG_HEADER_SIZE, 1, output);
	}
}

void encode_file_spui(args_t *args, decoder_t *decoder, FILE *output) {
	int audio_state_size = sizeof(psx_audio_encoder_channel_state_t) * args->audio_channels;

	// NOTE: since the interleaved .vag format is not standardized, some tools
	// (such as vgmstream) will not properly play files with interleave < 2048,
	// alignment != 2048 or channels != 2.
	int buffer_size = args->audio_interleave + args->alignment - 1;
	buffer_size -= buffer_size % args->alignment;

	int header_size = VAG_HEADER_SIZE + args->alignment - 1;
	header_size -= header_size % args->alignment;

	int audio_samples_per_block = psx_audio_spu_get_samples_per_block();
	int block_size = psx_audio_spu_get_buffer_size_per_block();
	int audio_samples_per_chunk = args->audio_interleave / block_size * audio_samples_per_block;
	int chunk_count;

	if (args->format == FORMAT_VAGI)
		fseek(output, header_size, SEEK_SET);

	psx_audio_encoder_channel_state_t *audio_state = malloc(audio_state_size);
	uint8_t *buffer = malloc(buffer_size);
	memset(audio_state, 0, audio_state_size);

	for (chunk_count = 0; ensure_av_data(decoder, audio_samples_per_chunk * args->audio_channels, 0); chunk_count++) {
		int samples_length = decoder->audio_sample_count / args->audio_channels;
		if (samples_length > audio_samples_per_chunk) samples_length = audio_samples_per_chunk;

		for (int ch = 0; ch < args->audio_channels; ch++) {
			memset(buffer, 0, buffer_size);
			int length = psx_audio_spu_encode(
				audio_state + ch,
				decoder->audio_samples + ch,
				samples_length,
				args->audio_channels,
				buffer
			);

			if (length) {
				// TODO: implement proper loop flag support
				if (args->flags & FLAG_SPU_LOOP_END)
					buffer[length - block_size + 1] |= PSX_AUDIO_SPU_LOOP_REPEAT;
				else if (decoder->end_of_input)
					buffer[length - block_size + 1] |= PSX_AUDIO_SPU_LOOP_END;
			}

			fwrite(buffer, buffer_size, 1, output);

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

		retire_av_data(decoder, samples_length * args->audio_channels, 0);
	}

	if (args->format == FORMAT_VAGI) {
		uint8_t *header = malloc(header_size);
		memset(header, 0, header_size);
		write_vag_header(args, chunk_count * args->audio_interleave, header);

		fseek(output, 0, SEEK_SET);
		fwrite(header, header_size, 1, output);
		free(header);
	}

	free(audio_state);
	free(buffer);
}

void encode_file_str(args_t *args, decoder_t *decoder, FILE *output) {
	psx_audio_xa_settings_t xa_settings = args_to_libpsxav_xa_audio(args);
	int audio_samples_per_sector;
	uint8_t buffer[PSX_CDROM_SECTOR_SIZE];

	int offset, sector_size;

	if (args->format == FORMAT_STRV) {
		sector_size = 2048;
		offset = 0x18;
	} else {
		sector_size = psx_audio_xa_get_buffer_size_per_sector(xa_settings);
		offset = PSX_CDROM_SECTOR_SIZE - sector_size;
	}

	int interleave;
	int video_sectors_per_block;
	if (decoder->state.audio_stream) {
		// 1/N audio, (N-1)/N video
		audio_samples_per_sector = psx_audio_xa_get_samples_per_sector(xa_settings);
		interleave = psx_audio_xa_get_sector_interleave(xa_settings) * args->str_cd_speed;
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
		audio_samples_per_sector = 0;
		interleave = 1;
		video_sectors_per_block = 1;
	}

	psx_audio_encoder_state_t audio_state;
	memset(&audio_state, 0, sizeof(psx_audio_encoder_state_t));

	mdec_encoder_t encoder;
	init_mdec_encoder(&encoder, args->video_width, args->video_height);

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
	int frames_needed = (int) ceil((double)video_sectors_per_block / frame_size);
	if (frames_needed < 2) frames_needed = 2;

	for (int j = 0; !decoder->end_of_input || encoder.state.frame_data_offset < encoder.state.frame_max_size; j++) {
		ensure_av_data(decoder, audio_samples_per_sector * args->audio_channels, frames_needed);

		if ((j%interleave) < video_sectors_per_block) {
			// Video sector
			init_sector_buffer_video(args, (psx_cdrom_sector_mode2_t*) buffer, j);

			int frames_used = encode_sector_str(&encoder, decoder->video_frames, buffer);
			retire_av_data(decoder, 0, frames_used);
		} else {
			// Audio sector
			int samples_length = decoder->audio_sample_count / args->audio_channels;
			if (samples_length > audio_samples_per_sector) samples_length = audio_samples_per_sector;

			// FIXME: this is an extremely hacky way to handle audio tracks
			// shorter than the video track
			if (!samples_length)
				video_sectors_per_block++;

			int length = psx_audio_xa_encode(
				xa_settings,
				&audio_state,
				decoder->audio_samples,
				samples_length,
				buffer
			);

			if (decoder->end_of_input)
				psx_audio_xa_encode_finalize(xa_settings, buffer, length);

			retire_av_data(decoder, samples_length * args->audio_channels, 0);
		}

		if (args->format == FORMAT_STRCD) {
			int t = j + 75*2;

			// Put the time in
			buffer[0x00C] = ((t/75/60)%10)|(((t/75/60)/10)<<4);
			buffer[0x00D] = (((t/75)%60)%10)|((((t/75)%60)/10)<<4);
			buffer[0x00E] = ((t%75)%10)|(((t%75)/10)<<4);
		}

		if((j%interleave) < video_sectors_per_block)
			psx_cdrom_calculate_checksums((psx_cdrom_sector_t *)buffer, PSX_CDROM_SECTOR_TYPE_MODE2_FORM1);

		fwrite(buffer + offset, sector_size, 1, output);

		time_t t = get_elapsed_time();

		if (!(args->flags & FLAG_HIDE_PROGRESS) && t) {
			fprintf(
				stderr,
				"\rFrame: %4d | LBA: %6d | Avg. q. scale: %5.2f | Encoding speed: %5.2fx",
				encoder.state.frame_index,
				j,
				(double)encoder.state.quant_scale_sum / (double)encoder.state.frame_index,
				(double)(encoder.state.frame_index * args->str_fps_den) / (double)(t * args->str_fps_num)
			);
		}
	}

	free(encoder.state.frame_output);
	destroy_mdec_encoder(&encoder);
}

void encode_file_sbs(args_t *args, decoder_t *decoder, FILE *output) {
	mdec_encoder_t encoder;
	init_mdec_encoder(&encoder, args->video_width, args->video_height);

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
