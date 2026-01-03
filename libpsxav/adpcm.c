/*
libpsxav: MDEC video + SPU/XA-ADPCM audio library

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

#include <assert.h>
#include <string.h>
#include "libpsxav.h"

#define SHIFT_RANGE_4BPS 12
#define SHIFT_RANGE_8BPS 8

#define ADPCM_FILTER_COUNT     5
#define XA_ADPCM_FILTER_COUNT  4
#define SPU_ADPCM_FILTER_COUNT 5

static const int16_t filter_k1[ADPCM_FILTER_COUNT] = {0, 60, 115, 98, 122};
static const int16_t filter_k2[ADPCM_FILTER_COUNT] = {0, 0, -52, -55, -60};

static int find_min_shift(
	const psx_audio_encoder_channel_state_t *state,
	const int16_t *samples,
	int sample_limit,
	int pitch,
	int filter,
	int shift_range
) {
	// Assumption made:
	//
	// There is value in shifting right one step further to allow the nibbles to clip.
	// However, given a possible shift value, there is no value in shifting one step less.
	//
	// Having said that, this is not a completely accurate model of the encoder,
	// so maybe we will need to shift one step less.
	//
	int prev1 = state->prev1;
	int prev2 = state->prev2;
	int k1 = filter_k1[filter];
	int k2 = filter_k2[filter];

	int right_shift = 0;

	int32_t s_min = 0;
	int32_t s_max = 0;
	for (int i = 0; i < PSX_AUDIO_SPU_SAMPLES_PER_BLOCK; i++) {
		int32_t raw_sample = (i >= sample_limit) ? 0 : samples[i * pitch];
		int32_t previous_values = (k1*prev1 + k2*prev2 + (1<<5))>>6;
		int32_t sample = raw_sample - previous_values;
		if (sample < s_min) { s_min = sample; }
		if (sample > s_max) { s_max = sample; }
		prev2 = prev1;
		prev1 = raw_sample;
	}
	while(right_shift < shift_range && (s_max>>right_shift) > (+0x7FFF >> shift_range)) { right_shift += 1; };
	while(right_shift < shift_range && (s_min>>right_shift) < (-0x8000 >> shift_range)) { right_shift += 1; };

	int min_shift = shift_range - right_shift;
	assert(0 <= min_shift && min_shift <= shift_range);
	return min_shift;
}

static uint8_t attempt_to_encode(
	psx_audio_encoder_channel_state_t *outstate,
	const psx_audio_encoder_channel_state_t *instate,
	const int16_t *samples,
	int sample_limit,
	int pitch,
	uint8_t *data,
	int data_shift,
	int data_pitch,
	int filter,
	int sample_shift,
	int shift_range
) {
	uint8_t sample_mask = 0xFFFF >> shift_range;
	uint8_t nondata_mask = ~(sample_mask << data_shift);

	int min_shift = sample_shift;
	int k1 = filter_k1[filter];
	int k2 = filter_k2[filter];

	uint8_t hdr = (min_shift & 0x0F) | (filter << 4);

	if (outstate != instate) {
		memcpy(outstate, instate, sizeof(psx_audio_encoder_channel_state_t));
	}

	outstate->mse = 0;

	for (int i = 0; i < PSX_AUDIO_SPU_SAMPLES_PER_BLOCK; i++) {
		int32_t sample = ((i >= sample_limit) ? 0 : samples[i * pitch]) + outstate->qerr;
		int32_t previous_values = (k1*outstate->prev1 + k2*outstate->prev2 + (1<<5))>>6;
		int32_t sample_enc = sample - previous_values;
		sample_enc <<= min_shift;
		sample_enc += (1<<(shift_range-1));
		sample_enc >>= shift_range;
		if(sample_enc < (-0x8000 >> shift_range)) { sample_enc = -0x8000 >> shift_range; }
		if(sample_enc > (+0x7FFF >> shift_range)) { sample_enc = +0x7FFF >> shift_range; }
		sample_enc &= sample_mask;

		int32_t sample_dec = (int16_t) ((sample_enc & sample_mask) << shift_range);
		sample_dec >>= min_shift;
		sample_dec += previous_values;
		if (sample_dec > +0x7FFF) { sample_dec = +0x7FFF; }
		if (sample_dec < -0x8000) { sample_dec = -0x8000; }
		int64_t sample_error = sample_dec - sample;

		assert(sample_error < (1<<30));
		assert(sample_error > -(1<<30));

		data[i * data_pitch] = (data[i * data_pitch] & nondata_mask) | (sample_enc << data_shift);
		// FIXME: dithering is hard to predict
		//outstate->qerr += sample_error;
		outstate->mse += ((uint64_t)sample_error) * (uint64_t)sample_error;

		outstate->prev2 = outstate->prev1;
		outstate->prev1 = sample_dec;
	}

	return hdr;
}

static uint8_t encode(
	psx_audio_encoder_channel_state_t *state,
	const int16_t *samples,
	int sample_limit,
	int pitch,
	uint8_t *data,
	int data_shift,
	int data_pitch,
	int filter_count,
	int shift_range
) {
	psx_audio_encoder_channel_state_t proposed;
	int64_t best_mse = ((int64_t)1<<(int64_t)50);
	int best_filter = 0;
	int best_sample_shift = 0;

	for (int filter = 0; filter < filter_count; filter++) {
		int true_min_shift = find_min_shift(state, samples, sample_limit, pitch, filter, shift_range);

		// Testing has shown that the optimal shift can be off the true minimum shift
		// by 1 in *either* direction.
		// This is NOT the case when dither is used.
		int min_shift = true_min_shift - 1;
		int max_shift = true_min_shift + 1;
		if (min_shift < 0) { min_shift = 0; }
		if (max_shift > shift_range) { max_shift = shift_range; }

		for (int sample_shift = min_shift; sample_shift <= max_shift; sample_shift++) {
			// ignore header here
			attempt_to_encode(
				&proposed, state,
				samples, sample_limit, pitch,
				data, data_shift, data_pitch,
				filter, sample_shift, shift_range);

			if (best_mse > proposed.mse) {
				best_mse = proposed.mse;
				best_filter = filter;
				best_sample_shift = sample_shift;
			}
		}
	}

	// now go with the encoder
	return attempt_to_encode(
		state, state,
		samples, sample_limit, pitch,
		data, data_shift, data_pitch,
		best_filter, best_sample_shift, shift_range);
}

static void encode_block_xa(
	const int16_t *audio_samples,
	int audio_samples_limit,
	uint8_t *data,
	psx_audio_xa_settings_t settings,
	psx_audio_encoder_state_t *state
) {
	if (settings.bits_per_sample == 4) {
		if (settings.stereo) {
			data[0]  = encode(&(state->left),  audio_samples,            audio_samples_limit,        2, data + 0x10, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[1]  = encode(&(state->right), audio_samples + 1,        audio_samples_limit,        2, data + 0x10, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[2]  = encode(&(state->left),  audio_samples + 56,       audio_samples_limit - 28,   2, data + 0x11, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[3]  = encode(&(state->right), audio_samples + 56 + 1,   audio_samples_limit - 28,   2, data + 0x11, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[8]  = encode(&(state->left),  audio_samples + 56*2,     audio_samples_limit - 28*2, 2, data + 0x12, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[9]  = encode(&(state->right), audio_samples + 56*2 + 1, audio_samples_limit - 28*2, 2, data + 0x12, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[10] = encode(&(state->left),  audio_samples + 56*3,     audio_samples_limit - 28*3, 2, data + 0x13, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[11] = encode(&(state->right), audio_samples + 56*3 + 1, audio_samples_limit - 28*3, 2, data + 0x13, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
		} else {
			data[0]  = encode(&(state->left), audio_samples,        audio_samples_limit,        1, data + 0x10, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[1]  = encode(&(state->left), audio_samples + 28,   audio_samples_limit - 28,   1, data + 0x10, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[2]  = encode(&(state->left), audio_samples + 28*2, audio_samples_limit - 28*2, 1, data + 0x11, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[3]  = encode(&(state->left), audio_samples + 28*3, audio_samples_limit - 28*3, 1, data + 0x11, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[8]  = encode(&(state->left), audio_samples + 28*4, audio_samples_limit - 28*4, 1, data + 0x12, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[9]  = encode(&(state->left), audio_samples + 28*5, audio_samples_limit - 28*5, 1, data + 0x12, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[10] = encode(&(state->left), audio_samples + 28*6, audio_samples_limit - 28*6, 1, data + 0x13, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
			data[11] = encode(&(state->left), audio_samples + 28*7, audio_samples_limit - 28*7, 1, data + 0x13, 4, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
		}
	} else {
		if (settings.stereo) {
			data[0] = encode(&(state->left),  audio_samples,          audio_samples_limit,      2, data + 0x10, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
			data[1] = encode(&(state->right), audio_samples + 1,      audio_samples_limit,      2, data + 0x11, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
			data[2] = encode(&(state->left),  audio_samples + 56,     audio_samples_limit - 28, 2, data + 0x12, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
			data[3] = encode(&(state->right), audio_samples + 56 + 1, audio_samples_limit - 28, 2, data + 0x13, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
		} else {
			data[0] = encode(&(state->left), audio_samples,        audio_samples_limit,        1, data + 0x10, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
			data[1] = encode(&(state->left), audio_samples + 28,   audio_samples_limit - 28,   1, data + 0x11, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
			data[2] = encode(&(state->left), audio_samples + 28*2, audio_samples_limit - 28*2, 1, data + 0x12, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
			data[3] = encode(&(state->left), audio_samples + 28*3, audio_samples_limit - 28*3, 1, data + 0x13, 0, 4, XA_ADPCM_FILTER_COUNT, SHIFT_RANGE_8BPS);
		}
	}
}

uint32_t psx_audio_xa_get_buffer_size(psx_audio_xa_settings_t settings, int sample_count) {
	int sample_pitch = psx_audio_xa_get_samples_per_sector(settings);
	int xa_sectors = ((sample_count + sample_pitch - 1) / sample_pitch);
	int xa_sector_size = psx_audio_xa_get_buffer_size_per_sector(settings);
	return xa_sectors * xa_sector_size;
}

uint32_t psx_audio_spu_get_buffer_size(int sample_count) {
	return ((sample_count + PSX_AUDIO_SPU_SAMPLES_PER_BLOCK - 1) / PSX_AUDIO_SPU_SAMPLES_PER_BLOCK) << 4;
}

uint32_t psx_audio_xa_get_buffer_size_per_sector(psx_audio_xa_settings_t settings) {
	return settings.format == PSX_AUDIO_XA_FORMAT_XA ? 2336 : 2352;
}

uint32_t psx_audio_xa_get_samples_per_sector(psx_audio_xa_settings_t settings) {
	return (((settings.bits_per_sample == 8) ? 112 : 224) >> (settings.stereo ? 1 : 0)) * 18;
}

uint32_t psx_audio_xa_get_sector_interleave(psx_audio_xa_settings_t settings) {
	// 1/2 interleave for 37800 Hz 8-bit stereo at 1x speed
	int interleave = settings.stereo ? 2 : 4;
	if (settings.frequency == PSX_AUDIO_XA_FREQ_SINGLE) { interleave <<= 1; }
	if (settings.bits_per_sample == 4) { interleave <<= 1; }
	return interleave;
}

static inline void psx_audio_xa_sync_subheader_copy(psx_cdrom_sector_mode2_t *buffer) {
	memcpy(buffer->subheader + 1, buffer->subheader, sizeof(psx_cdrom_sector_xa_subheader_t));
}

static void psx_audio_xa_encode_init_sector(psx_cdrom_sector_mode2_t *buffer, int lba, psx_audio_xa_settings_t settings) {
	if (settings.format == PSX_AUDIO_XA_FORMAT_XACD)
		psx_cdrom_init_sector((psx_cdrom_sector_t *)buffer, lba, PSX_CDROM_SECTOR_TYPE_MODE2_FORM2);
	else if (settings.format == PSX_AUDIO_XA_FORMAT_XA)
		psx_cdrom_init_xa_subheader(buffer->subheader, PSX_CDROM_SECTOR_TYPE_MODE2_FORM2);

	buffer->subheader[0].file = settings.file_number;
	buffer->subheader[0].channel = settings.channel_number & PSX_CDROM_SECTOR_XA_CHANNEL_MASK;
	buffer->subheader[0].submode =
		PSX_CDROM_SECTOR_XA_SUBMODE_AUDIO
		| PSX_CDROM_SECTOR_XA_SUBMODE_FORM2
		| PSX_CDROM_SECTOR_XA_SUBMODE_RT;

	if (settings.stereo)
		buffer->subheader[0].coding |= PSX_CDROM_SECTOR_XA_CODING_STEREO;
	else
		buffer->subheader[0].coding |= PSX_CDROM_SECTOR_XA_CODING_MONO;
	if (settings.frequency == PSX_AUDIO_XA_FREQ_DOUBLE)
		buffer->subheader[0].coding |= PSX_CDROM_SECTOR_XA_CODING_FREQ_DOUBLE;
	else
		buffer->subheader[0].coding |= PSX_CDROM_SECTOR_XA_CODING_FREQ_SINGLE;
	if (settings.bits_per_sample == 8)
		buffer->subheader[0].coding |= PSX_CDROM_SECTOR_XA_CODING_BITS_8;
	else
		buffer->subheader[0].coding |= PSX_CDROM_SECTOR_XA_CODING_BITS_4;

	psx_audio_xa_sync_subheader_copy(buffer);
}

int psx_audio_xa_encode(
	psx_audio_xa_settings_t settings,
	psx_audio_encoder_state_t *state,
	const int16_t *samples,
	int sample_count,
	int lba,
	uint8_t *output
) {
	int sample_jump = (settings.bits_per_sample == 8) ? 112 : 224;
	int i, j;
	int xa_sector_size = psx_audio_xa_get_buffer_size_per_sector(settings);
	int xa_offset = PSX_CDROM_SECTOR_SIZE - xa_sector_size;
	uint8_t init_sector = 1;

	if (settings.stereo)
		sample_count *= 2;

	for (i = 0, j = 0; i < sample_count || ((j % 18) != 0); i += sample_jump, j++) {
		psx_cdrom_sector_mode2_t *sector_data = (psx_cdrom_sector_mode2_t*) (output + ((j/18) * xa_sector_size) - xa_offset);
		uint8_t *block_data = sector_data->data + ((j%18) * 0x80);

		if (init_sector) {
			psx_audio_xa_encode_init_sector(sector_data, lba, settings);
			init_sector = 0;
		}

		encode_block_xa(samples + i, sample_count - i, block_data, settings, state);

		memcpy(block_data + 4, block_data, 4);
		memcpy(block_data + 12, block_data + 8, 4);

		if ((j+1)%18 == 0) {
			psx_cdrom_calculate_checksums((psx_cdrom_sector_t *)sector_data, PSX_CDROM_SECTOR_TYPE_MODE2_FORM2);
			init_sector = 1;
			lba++;
		}
	}

	return (((j + 17) / 18) * xa_sector_size);
}

void psx_audio_xa_encode_finalize(psx_audio_xa_settings_t settings, uint8_t *output, int output_length) {
	if (output_length >= 2336) {
		psx_cdrom_sector_mode2_t *sector = (psx_cdrom_sector_mode2_t*) &output[output_length - PSX_CDROM_SECTOR_SIZE];
		sector->subheader[0].submode |= PSX_CDROM_SECTOR_XA_SUBMODE_EOF;
		psx_audio_xa_sync_subheader_copy(sector);
	}
}

int psx_audio_xa_encode_simple(
	psx_audio_xa_settings_t settings,
	const int16_t *samples,
	int sample_count,
	int lba,
	uint8_t *output
) {
	psx_audio_encoder_state_t state;
	memset(&state, 0, sizeof(psx_audio_encoder_state_t));
	int length = psx_audio_xa_encode(settings, &state, samples, sample_count, lba, output);
	psx_audio_xa_encode_finalize(settings, output, length);
	return length;
}

int psx_audio_spu_encode(
	psx_audio_encoder_channel_state_t *state,
	const int16_t *samples,
	int sample_count,
	int pitch,
	uint8_t *output
) {
	uint8_t prebuf[PSX_AUDIO_SPU_SAMPLES_PER_BLOCK];
	uint8_t *buffer = output;

	for (int i = 0; i < sample_count; i += PSX_AUDIO_SPU_SAMPLES_PER_BLOCK, buffer += PSX_AUDIO_SPU_BLOCK_SIZE) {
		buffer[0] = encode(state, samples + i * pitch, sample_count - i, pitch, prebuf, 0, 1, SPU_ADPCM_FILTER_COUNT, SHIFT_RANGE_4BPS);
		buffer[1] = 0;

		for (int j = 0; j < PSX_AUDIO_SPU_SAMPLES_PER_BLOCK; j+=2) {
			buffer[2 + (j>>1)] = (prebuf[j] & 0x0F) | (prebuf[j+1] << 4);
		}
	}

	return buffer - output;
}

int psx_audio_spu_encode_simple(const int16_t *samples, int sample_count, uint8_t *output, int loop_start) {
	psx_audio_encoder_channel_state_t state;
	memset(&state, 0, sizeof(psx_audio_encoder_channel_state_t));
	int length = psx_audio_spu_encode(&state, samples, sample_count, 1, output);

	if (length >= PSX_AUDIO_SPU_BLOCK_SIZE) {
		uint8_t *last_block = output + length - PSX_AUDIO_SPU_BLOCK_SIZE;

		if (loop_start < 0) {
			// Insert trailing looping block
			memset(output + length, 0, PSX_AUDIO_SPU_BLOCK_SIZE);
			output[length + 1] = PSX_AUDIO_SPU_LOOP_TRAP;

			length += PSX_AUDIO_SPU_BLOCK_SIZE;
		} else {
			int loop_start_offset = loop_start / PSX_AUDIO_SPU_SAMPLES_PER_BLOCK * PSX_AUDIO_SPU_BLOCK_SIZE;

			last_block[1] |= PSX_AUDIO_SPU_LOOP_REPEAT;
			output[loop_start_offset + 1] |= PSX_AUDIO_SPU_LOOP_START;
		}
	}

	return length;
}
