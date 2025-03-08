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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avdct.h>
#include "args.h"

typedef struct {
	int frame_index;
	int frame_data_offset;
	int frame_max_size;
	int frame_block_base_overflow;
	int frame_block_overflow_num;
	int frame_block_overflow_den;
	int block_type;
	int16_t last_dc_values[3];
	uint16_t bits_value;
	int bits_left;
	uint8_t *frame_output;
	int bytes_used;
	int blocks_used;
	int uncomp_hwords_used;
	int quant_scale;
	int quant_scale_sum;

	AVDCT *dct_context;
	uint32_t *ac_huffman_map;
	uint32_t *dc_huffman_map;
	int16_t *coeff_clamp_map;
	int16_t *dct_block_lists[6];
} mdec_encoder_state_t;

typedef struct {
	bs_codec_t video_codec;
	int video_width;
	int video_height;

	mdec_encoder_state_t state;
} mdec_encoder_t;

bool init_mdec_encoder(mdec_encoder_t *encoder, bs_codec_t video_codec, int video_width, int video_height);
void destroy_mdec_encoder(mdec_encoder_t *encoder);
void encode_frame_bs(mdec_encoder_t *encoder, const uint8_t *video_frame);
int encode_sector_str(
	mdec_encoder_t *encoder,
	format_t format,
	uint16_t str_video_id,
	const uint8_t *video_frames,
	uint8_t *output
);
