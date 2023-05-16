/*
psxavenc: MDEC video + SPU/XA-ADPCM audio encoder frontend

Copyright (c) 2019, 2020 Adrian "asie" Siekierka
Copyright (c) 2019 Ben "GreaseMonkey" Russell
Copyright (c) 2023 spicyjpeg

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

#include "common.h"
#include "libpsxav.h"

static time_t get_elapsed_time(settings_t *settings) {
	if (!settings->show_progress) {
		return 0;
	}
	time_t t = time(NULL) - settings->start_time;
	if (t <= settings->last_progress_update) {
		return 0;
	}
	settings->last_progress_update = t;
	return t;
}

static psx_audio_xa_settings_t settings_to_libpsxav_xa_audio(settings_t *settings) {
	psx_audio_xa_settings_t new_settings;
	new_settings.bits_per_sample = settings->bits_per_sample;
	new_settings.frequency = settings->frequency;
	new_settings.stereo = settings->channels == 2;
	new_settings.file_number = settings->file_number;
	new_settings.channel_number = settings->channel_number;

	switch (settings->format) {
		case FORMAT_XA:
		case FORMAT_STR2:
			new_settings.format = PSX_AUDIO_XA_FORMAT_XA;
			break;
		default:
			new_settings.format = PSX_AUDIO_XA_FORMAT_XACD;
			break;
	}

	return new_settings;
};

void write_vag_header(int size_per_channel, uint8_t *header, settings_t *settings) {
	// Magic
	header[0x00] = 'V';
	header[0x01] = 'A';
	header[0x02] = 'G';
	header[0x03] = settings->interleave ? 'i' : 'p';

	// Version (big-endian)
	header[0x04] = 0x00;
	header[0x05] = 0x00;
	header[0x06] = 0x00;
	header[0x07] = 0x20;

	// Interleave (little-endian)
	header[0x08] = (uint8_t)settings->interleave;
	header[0x09] = (uint8_t)(settings->interleave>>8);
	header[0x0a] = (uint8_t)(settings->interleave>>16);
	header[0x0b] = (uint8_t)(settings->interleave>>24);

	// Length of data for each channel (big-endian)
	header[0x0c] = (uint8_t)(size_per_channel>>24);
	header[0x0d] = (uint8_t)(size_per_channel>>16);
	header[0x0e] = (uint8_t)(size_per_channel>>8);
	header[0x0f] = (uint8_t)size_per_channel;

	// Sample rate (big-endian)
	header[0x10] = (uint8_t)(settings->frequency>>24);
	header[0x11] = (uint8_t)(settings->frequency>>16);
	header[0x12] = (uint8_t)(settings->frequency>>8);
	header[0x13] = (uint8_t)settings->frequency;

	// Number of channels (little-endian)
	header[0x1e] = (uint8_t)settings->channels;
	header[0x1f] = 0x00;

	// Filename
	//strncpy(header + 0x20, "psxavenc", 16);
	memset(header + 0x20, 0, 16);
}

void encode_file_spu(settings_t *settings, FILE *output) {
	psx_audio_encoder_channel_state_t audio_state;	
	int audio_samples_per_block = psx_audio_spu_get_samples_per_block();
	int block_size = psx_audio_spu_get_buffer_size_per_block();
	uint8_t buffer[16];
	int block_count;

	memset(&audio_state, 0, sizeof(psx_audio_encoder_channel_state_t));

	// The header must be written after the data as we don't yet know the
	// number of audio samples.
	if (settings->format == FORMAT_VAG) {
		fseek(output, 48, SEEK_SET);
	}

	for (block_count = 0; ensure_av_data(settings, audio_samples_per_block, 0); block_count++) {
		int samples_length = settings->audio_sample_count;
		if (samples_length > audio_samples_per_block) samples_length = audio_samples_per_block;

		int length = psx_audio_spu_encode(&audio_state, settings->audio_samples, samples_length, 1, buffer);
		if (!block_count) {
			// This flag is not required as the SPU already resets the loop
			// address when starting playback of a sample.
			//buffer[1] |= PSX_AUDIO_SPU_LOOP_START;
		}
		if (settings->end_of_input) {
			buffer[1] |= settings->loop ? PSX_AUDIO_SPU_LOOP_REPEAT : PSX_AUDIO_SPU_LOOP_END;
		}

		retire_av_data(settings, samples_length, 0);
		fwrite(buffer, length, 1, output);

		time_t t = get_elapsed_time(settings);
		if (t) {
			fprintf(stderr, "\rBlock: %6d | Encoding speed: %5.2fx",
				block_count,
				(double)(block_count*audio_samples_per_block) / (double)(settings->frequency*t)
			);
		}
	}

	if (settings->format == FORMAT_VAG) {
		uint8_t header[48];
		memset(header, 0, 48);
		write_vag_header(block_count*block_size, header, settings);
		fseek(output, 0, SEEK_SET);
		fwrite(header, 48, 1, output);
	}
}

void encode_file_spu_interleaved(settings_t *settings, FILE *output) {
	int audio_state_size = sizeof(psx_audio_encoder_channel_state_t) * settings->channels;

	// NOTE: since the interleaved .vag format is not standardized, some tools
	// (such as vgmstream) will not properly play files with interleave < 2048,
	// alignment != 2048 or channels != 2.
	int buffer_size = settings->interleave + settings->alignment - 1;
	buffer_size -= buffer_size % settings->alignment;
	int header_size = 48 + settings->alignment - 1;
	header_size -= header_size % settings->alignment;

	psx_audio_encoder_channel_state_t *audio_state = malloc(audio_state_size);
	uint8_t *buffer = malloc(buffer_size);
	int audio_samples_per_block = psx_audio_spu_get_samples_per_block();
	int block_size = psx_audio_spu_get_buffer_size_per_block();
	int audio_samples_per_chunk = settings->interleave / block_size * audio_samples_per_block;
	int chunk_count;

	memset(audio_state, 0, audio_state_size);

	if (settings->format == FORMAT_VAGI) {
		fseek(output, header_size, SEEK_SET);
	}

	for (chunk_count = 0; ensure_av_data(settings, audio_samples_per_chunk*settings->channels, 0); chunk_count++) {
		int samples_length = settings->audio_sample_count / settings->channels;
		if (samples_length > audio_samples_per_chunk) samples_length = audio_samples_per_chunk;

		for (int ch = 0; ch < settings->channels; ch++) {
			memset(buffer, 0, buffer_size);
			int length = psx_audio_spu_encode(audio_state + ch, settings->audio_samples + ch, samples_length, settings->channels, buffer);
			if (length) {
				//buffer[1] |= PSX_AUDIO_SPU_LOOP_START;
				if (settings->loop) {
					buffer[length - block_size + 1] |= PSX_AUDIO_SPU_LOOP_REPEAT;
				}
				if (settings->end_of_input) {
					buffer[length - block_size + 1] |= PSX_AUDIO_SPU_LOOP_END;
				}
			}

			fwrite(buffer, buffer_size, 1, output);

			time_t t = get_elapsed_time(settings);
			if (t) {
				fprintf(stderr, "\rChunk: %6d | Encoding speed: %5.2fx",
					chunk_count,
					(double)(chunk_count*audio_samples_per_chunk) / (double)(settings->frequency*t)
				);
			}
		}

		retire_av_data(settings, samples_length*settings->channels, 0);
	}

	if (settings->format == FORMAT_VAGI) {
		uint8_t *header = malloc(header_size);
		memset(header, 0, header_size);
		write_vag_header(chunk_count*settings->interleave, header, settings);
		fseek(output, 0, SEEK_SET);
		fwrite(header, header_size, 1, output);
		free(header);
	}

	free(audio_state);
	free(buffer);
}

void encode_file_xa(settings_t *settings, FILE *output) {
	psx_audio_xa_settings_t xa_settings = settings_to_libpsxav_xa_audio(settings);
	psx_audio_encoder_state_t audio_state;	
	int audio_samples_per_sector = psx_audio_xa_get_samples_per_sector(xa_settings);
	uint8_t buffer[2352];

	memset(&audio_state, 0, sizeof(psx_audio_encoder_state_t));

	for (int j = 0; ensure_av_data(settings, audio_samples_per_sector*settings->channels, 0); j++) {
		int samples_length = settings->audio_sample_count / settings->channels;
		if (samples_length > audio_samples_per_sector) samples_length = audio_samples_per_sector;
		int length = psx_audio_xa_encode(xa_settings, &audio_state, settings->audio_samples, samples_length, buffer);
		if (settings->end_of_input) {
			psx_audio_xa_encode_finalize(xa_settings, buffer, length);
		}

		if (settings->format == FORMAT_XACD) {
			int t = j + 75*2;

			// Put the time in
			buffer[0x00C] = ((t/75/60)%10)|(((t/75/60)/10)<<4);
			buffer[0x00D] = (((t/75)%60)%10)|((((t/75)%60)/10)<<4);
			buffer[0x00E] = ((t%75)%10)|(((t%75)/10)<<4);
		}

		retire_av_data(settings, samples_length*settings->channels, 0);
		fwrite(buffer, length, 1, output);

		time_t t = get_elapsed_time(settings);
		if (t) {
			fprintf(stderr, "\rLBA: %6d | Encoding speed: %5.2fx",
				j,
				(double)(j*audio_samples_per_sector) / (double)(settings->frequency*t)
			);
		}
	}
}

void encode_file_str(settings_t *settings, FILE *output) {
	psx_audio_xa_settings_t xa_settings = settings_to_libpsxav_xa_audio(settings);
	psx_audio_encoder_state_t audio_state;
	int audio_samples_per_sector;
	uint8_t buffer[2352];

	int interleave;
	int video_sectors_per_block;
	if (settings->decoder_state_av.audio_stream) {
		// 1/N audio, (N-1)/N video
		audio_samples_per_sector = psx_audio_xa_get_samples_per_sector(xa_settings);
		interleave = psx_audio_xa_get_sector_interleave(xa_settings) * settings->cd_speed;
		video_sectors_per_block = interleave - 1;
	} else {
		// 0/1 audio, 1/1 video
		audio_samples_per_sector = 0;
		interleave = 1;
		video_sectors_per_block = 1;
	}

	if (!settings->quiet) {
		fprintf(stderr, "Interleave: %d/%d audio, %d/%d video\n",
			interleave - video_sectors_per_block, interleave, video_sectors_per_block, interleave);
	}

	memset(&audio_state, 0, sizeof(psx_audio_encoder_state_t));

	// e.g. 15fps = (150*7/8/15) = 8.75 blocks per frame
	settings->state_vid.frame_block_base_overflow = (75*settings->cd_speed) * video_sectors_per_block * settings->video_fps_den;
	settings->state_vid.frame_block_overflow_den = interleave * settings->video_fps_num;
	double frame_size = (double)settings->state_vid.frame_block_base_overflow / (double)settings->state_vid.frame_block_overflow_den;
	if (!settings->quiet) {
		fprintf(stderr, "Frame size: %.2f sectors\n", frame_size);
	}

	settings->state_vid.frame_output = malloc(2016 * (int)ceil(frame_size));
	settings->state_vid.frame_index = 0;
	settings->state_vid.frame_data_offset = 0;
	settings->state_vid.frame_max_size = 0;
	settings->state_vid.frame_block_overflow_num = 0;
	settings->state_vid.quant_scale_sum = 0;

	// FIXME: this needs an extra frame to prevent A/V desync
	int frames_needed = (int) ceil((double)video_sectors_per_block / frame_size);
	if (frames_needed < 2) frames_needed = 2;

	for (int j = 0; !settings->end_of_input || settings->state_vid.frame_data_offset < settings->state_vid.frame_max_size; j++) {
		ensure_av_data(settings, audio_samples_per_sector*settings->channels, frames_needed);

		if ((j%interleave) < video_sectors_per_block) {
			// Video sector
			init_sector_buffer_video(buffer, settings);
			encode_sector_str(settings->video_frames, buffer, settings);
		} else {
			// Audio sector
			int samples_length = settings->audio_sample_count / settings->channels;
			if (samples_length > audio_samples_per_sector) samples_length = audio_samples_per_sector;

			// FIXME: this is an extremely hacky way to handle audio tracks
			// shorter than the video track
			if (!samples_length) {
				video_sectors_per_block++;
			}

			int length = psx_audio_xa_encode(xa_settings, &audio_state, settings->audio_samples, samples_length, buffer);
			if (settings->end_of_input) {
				psx_audio_xa_encode_finalize(xa_settings, buffer, length);
			}
			retire_av_data(settings, samples_length*settings->channels, 0);
		}

		if (settings->format == FORMAT_STR2CD) {
			int t = j + 75*2;

			// Put the time in
			buffer[0x00C] = ((t/75/60)%10)|(((t/75/60)/10)<<4);
			buffer[0x00D] = (((t/75)%60)%10)|((((t/75)%60)/10)<<4);
			buffer[0x00E] = ((t%75)%10)|(((t%75)/10)<<4);

			// FIXME: EDC is not calculated in 2336-byte sector mode (shouldn't
			// matter anyway, any CD image builder will have to recalculate it
			// due to the sector's MSF changing)
			if((j%interleave) < video_sectors_per_block) {
				calculate_edc_data(buffer);
			}
		}

		fwrite(buffer, 2352, 1, output);

		time_t t = get_elapsed_time(settings);
		if (t) {
			fprintf(stderr, "\rFrame: %4d | LBA: %6d | Avg. q. scale: %5.2f | Encoding speed: %5.2fx",
				settings->state_vid.frame_index,
				j,
				(double)settings->state_vid.quant_scale_sum / (double)settings->state_vid.frame_index,
				(double)(settings->state_vid.frame_index*settings->video_fps_den) / (double)(t*settings->video_fps_num)
			);
		}
	}

	free(settings->state_vid.frame_output);
}

void encode_file_sbs(settings_t *settings, FILE *output) {
	settings->state_vid.frame_output = malloc(settings->alignment);
	settings->state_vid.frame_data_offset = 0;
	settings->state_vid.frame_max_size = settings->alignment;
	settings->state_vid.quant_scale_sum = 0;

	for (int j = 0; ensure_av_data(settings, 0, 2); j++) {
		encode_frame_bs(settings->video_frames, settings);
		fwrite(settings->state_vid.frame_output, settings->alignment, 1, output);

		time_t t = get_elapsed_time(settings);
		if (t) {
			fprintf(stderr, "\rFrame: %4d | Avg. q. scale: %5.2f | Encoding speed: %5.2fx",
				j,
				(double)settings->state_vid.quant_scale_sum / (double)j,
				(double)(j*settings->video_fps_den) / (double)(t*settings->video_fps_num)
			);
		}
	}

	free(settings->state_vid.frame_output);
}
