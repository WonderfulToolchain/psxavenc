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

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avdct.h>
#include "args.h"
#include "mdec.h"

#define AC_PAIR(zeroes, value) \
	(((zeroes) << 10) | ((+(value)) & 0x3FF)), \
	(((zeroes) << 10) | ((-(value)) & 0x3FF))

static const struct {
	int c_bits;
	uint32_t c_value;
	uint16_t u_hword_pos;
	uint16_t u_hword_neg;
} ac_huffman_tree[] = {
	// Fuck this Huffman tree in particular --GM
	{ 2, 0x3,    AC_PAIR( 0,  1)},
	{ 3, 0x3,    AC_PAIR( 1,  1)},
	{ 4, 0x4,    AC_PAIR( 0,  2)},
	{ 4, 0x5,    AC_PAIR( 2,  1)},
	{ 5, 0x05,   AC_PAIR( 0,  3)},
	{ 5, 0x06,   AC_PAIR( 4,  1)},
	{ 5, 0x07,   AC_PAIR( 3,  1)},
	{ 6, 0x04,   AC_PAIR( 7,  1)},
	{ 6, 0x05,   AC_PAIR( 6,  1)},
	{ 6, 0x06,   AC_PAIR( 1,  2)},
	{ 6, 0x07,   AC_PAIR( 5,  1)},
	{ 7, 0x04,   AC_PAIR( 2,  2)},
	{ 7, 0x05,   AC_PAIR( 9,  1)},
	{ 7, 0x06,   AC_PAIR( 0,  4)},
	{ 7, 0x07,   AC_PAIR( 8,  1)},
	{ 8, 0x20,   AC_PAIR(13,  1)},
	{ 8, 0x21,   AC_PAIR( 0,  6)},
	{ 8, 0x22,   AC_PAIR(12,  1)},
	{ 8, 0x23,   AC_PAIR(11,  1)},
	{ 8, 0x24,   AC_PAIR( 3,  2)},
	{ 8, 0x25,   AC_PAIR( 1,  3)},
	{ 8, 0x26,   AC_PAIR( 0,  5)},
	{ 8, 0x27,   AC_PAIR(10,  1)},
	{10, 0x008,  AC_PAIR(16,  1)},
	{10, 0x009,  AC_PAIR( 5,  2)},
	{10, 0x00A,  AC_PAIR( 0,  7)},
	{10, 0x00B,  AC_PAIR( 2,  3)},
	{10, 0x00C,  AC_PAIR( 1,  4)},
	{10, 0x00D,  AC_PAIR(15,  1)},
	{10, 0x00E,  AC_PAIR(14,  1)},
	{10, 0x00F,  AC_PAIR( 4,  2)},
	{12, 0x010,  AC_PAIR( 0, 11)},
	{12, 0x011,  AC_PAIR( 8,  2)},
	{12, 0x012,  AC_PAIR( 4,  3)},
	{12, 0x013,  AC_PAIR( 0, 10)},
	{12, 0x014,  AC_PAIR( 2,  4)},
	{12, 0x015,  AC_PAIR( 7,  2)},
	{12, 0x016,  AC_PAIR(21,  1)},
	{12, 0x017,  AC_PAIR(20,  1)},
	{12, 0x018,  AC_PAIR( 0,  9)},
	{12, 0x019,  AC_PAIR(19,  1)},
	{12, 0x01A,  AC_PAIR(18,  1)},
	{12, 0x01B,  AC_PAIR( 1,  5)},
	{12, 0x01C,  AC_PAIR( 3,  3)},
	{12, 0x01D,  AC_PAIR( 0,  8)},
	{12, 0x01E,  AC_PAIR( 6,  2)},
	{12, 0x01F,  AC_PAIR(17,  1)},
	{13, 0x0010, AC_PAIR(10,  2)},
	{13, 0x0011, AC_PAIR( 9,  2)},
	{13, 0x0012, AC_PAIR( 5,  3)},
	{13, 0x0013, AC_PAIR( 3,  4)},
	{13, 0x0014, AC_PAIR( 2,  5)},
	{13, 0x0015, AC_PAIR( 1,  7)},
	{13, 0x0016, AC_PAIR( 1,  6)},
	{13, 0x0017, AC_PAIR( 0, 15)},
	{13, 0x0018, AC_PAIR( 0, 14)},
	{13, 0x0019, AC_PAIR( 0, 13)},
	{13, 0x001A, AC_PAIR( 0, 12)},
	{13, 0x001B, AC_PAIR(26,  1)},
	{13, 0x001C, AC_PAIR(25,  1)},
	{13, 0x001D, AC_PAIR(24,  1)},
	{13, 0x001E, AC_PAIR(23,  1)},
	{13, 0x001F, AC_PAIR(22,  1)},
	{14, 0x0010, AC_PAIR( 0, 31)},
	{14, 0x0011, AC_PAIR( 0, 30)},
	{14, 0x0012, AC_PAIR( 0, 29)},
	{14, 0x0013, AC_PAIR( 0, 28)},
	{14, 0x0014, AC_PAIR( 0, 27)},
	{14, 0x0015, AC_PAIR( 0, 26)},
	{14, 0x0016, AC_PAIR( 0, 25)},
	{14, 0x0017, AC_PAIR( 0, 24)},
	{14, 0x0018, AC_PAIR( 0, 23)},
	{14, 0x0019, AC_PAIR( 0, 22)},
	{14, 0x001A, AC_PAIR( 0, 21)},
	{14, 0x001B, AC_PAIR( 0, 20)},
	{14, 0x001C, AC_PAIR( 0, 19)},
	{14, 0x001D, AC_PAIR( 0, 18)},
	{14, 0x001E, AC_PAIR( 0, 17)},
	{14, 0x001F, AC_PAIR( 0, 16)},
	{15, 0x0010, AC_PAIR( 0, 40)},
	{15, 0x0011, AC_PAIR( 0, 39)},
	{15, 0x0012, AC_PAIR( 0, 38)},
	{15, 0x0013, AC_PAIR( 0, 37)},
	{15, 0x0014, AC_PAIR( 0, 36)},
	{15, 0x0015, AC_PAIR( 0, 35)},
	{15, 0x0016, AC_PAIR( 0, 34)},
	{15, 0x0017, AC_PAIR( 0, 33)},
	{15, 0x0018, AC_PAIR( 0, 32)},
	{15, 0x0019, AC_PAIR( 1, 14)},
	{15, 0x001A, AC_PAIR( 1, 13)},
	{15, 0x001B, AC_PAIR( 1, 12)},
	{15, 0x001C, AC_PAIR( 1, 11)},
	{15, 0x001D, AC_PAIR( 1, 10)},
	{15, 0x001E, AC_PAIR( 1,  9)},
	{15, 0x001F, AC_PAIR( 1,  8)},
	{16, 0x0010, AC_PAIR( 1, 18)},
	{16, 0x0011, AC_PAIR( 1, 17)},
	{16, 0x0012, AC_PAIR( 1, 16)},
	{16, 0x0013, AC_PAIR( 1, 15)},
	{16, 0x0014, AC_PAIR( 6,  3)},
	{16, 0x0015, AC_PAIR(16,  2)},
	{16, 0x0016, AC_PAIR(15,  2)},
	{16, 0x0017, AC_PAIR(14,  2)},
	{16, 0x0018, AC_PAIR(13,  2)},
	{16, 0x0019, AC_PAIR(12,  2)},
	{16, 0x001A, AC_PAIR(11,  2)},
	{16, 0x001B, AC_PAIR(31,  1)},
	{16, 0x001C, AC_PAIR(30,  1)},
	{16, 0x001D, AC_PAIR(29,  1)},
	{16, 0x001E, AC_PAIR(28,  1)},
	{16, 0x001F, AC_PAIR(27,  1)}
};

static const struct {
	int c_bits;
	uint32_t c_value;
	int dc_bits;
} dc_c_huffman_tree[] = {
	{2, 0x1,  0},
	{2, 0x2,  1},
	{3, 0x6,  2},
	{4, 0xE,  3},
	{5, 0x1E, 4},
	{6, 0x3E, 5},
	{7, 0x7E, 6},
	{8, 0xFE, 7}
};

static const struct {
	int c_bits;
	uint32_t c_value;
	int dc_bits;
} dc_y_huffman_tree[] = {
	{2, 0x0,  0},
	{2, 0x1,  1},
	{3, 0x5,  2},
	{3, 0x6,  3},
	{4, 0xE,  4},
	{5, 0x1E, 5},
	{6, 0x3E, 6},
	{7, 0x7E, 7}
};

static const uint8_t quant_dec[8*8] = {
	 2, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83
};

#if 0
static const uint8_t dct_zigzag_table[8*8] = {
	 0,  1,  5,  6, 14, 15, 27, 28,
	 2,  4,  7, 13, 16, 26, 29, 42,
	 3,  8, 12, 17, 25, 30, 41, 43,
	 9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63
};
#endif

static const uint8_t dct_zagzig_table[8*8] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

#if 0
enum {
	SF0 = 0x5a82, // cos(0/16 * pi) * sqrt(2)
	SF1 = 0x7d8a, // cos(1/16 * pi) * 2
	SF2 = 0x7641, // cos(2/16 * pi) * 2
	SF3 = 0x6a6d, // cos(3/16 * pi) * 2
	SF4 = 0x5a82, // cos(4/16 * pi) * 2
	SF5 = 0x471c, // cos(5/16 * pi) * 2
	SF6 = 0x30fb, // cos(6/16 * pi) * 2
	SF7 = 0x18f8  // cos(7/16 * pi) * 2
};

static const int16_t dct_scale_table[8*8] = {
	SF0,  SF0,  SF0,  SF0,  SF0,  SF0,  SF0,  SF0,
	SF1,  SF3,  SF5,  SF7, -SF7, -SF5, -SF3, -SF1,
	SF2,  SF6, -SF6, -SF2, -SF2, -SF6,  SF6,  SF2,
	SF3, -SF7, -SF1, -SF5,  SF5,  SF1,  SF7, -SF3,
	SF4, -SF4, -SF4,  SF4,  SF4, -SF4, -SF4,  SF4,
	SF5, -SF1,  SF7,  SF3, -SF3, -SF7,  SF1, -SF5,
	SF6, -SF2,  SF2, -SF6, -SF6,  SF2, -SF2,  SF6,
	SF7, -SF5,  SF3, -SF1,  SF1, -SF3,  SF5, -SF7
};
#endif

enum {
	INDEX_CR,
	INDEX_CB,
	INDEX_Y
};

#define HUFFMAN_CODE(bits, value) (((bits) << 24) | (value))

static void init_dct_data(mdec_encoder_state_t *state, bs_codec_t codec) {
	for(int i = 0; i <= 0xFFFF; i++) {
		state->ac_huffman_map[i] = HUFFMAN_CODE(6 + 16, (0x1 << 16) | i);

		int16_t coeff = (int16_t)i;

		if (coeff < -0x200)
			coeff = -0x200;
		else if (coeff > +0x1FE)
			coeff = +0x1FE; // 0x1FF = v2 end of frame

		state->coeff_clamp_map[i] = coeff;
	}

	state->dc_huffman_map[(INDEX_CR << 9) | 0] = HUFFMAN_CODE(2, 0x0);
	state->dc_huffman_map[(INDEX_CB << 9) | 0] = HUFFMAN_CODE(2, 0x0);
	state->dc_huffman_map[(INDEX_Y  << 9) | 0] = HUFFMAN_CODE(3, 0x4);

	int ac_tree_item_count = sizeof(ac_huffman_tree) / sizeof(ac_huffman_tree[0]);
	int dc_c_tree_item_count = sizeof(dc_c_huffman_tree) / sizeof(dc_c_huffman_tree[0]);
	int dc_y_tree_item_count = sizeof(dc_y_huffman_tree) / sizeof(dc_y_huffman_tree[0]);

	for (int i = 0; i < ac_tree_item_count; i++) {
		int bits = ac_huffman_tree[i].c_bits + 1;
		uint32_t base_value = ac_huffman_tree[i].c_value;

		state->ac_huffman_map[ac_huffman_tree[i].u_hword_pos] = HUFFMAN_CODE(bits, (base_value << 1) | 0);
		state->ac_huffman_map[ac_huffman_tree[i].u_hword_neg] = HUFFMAN_CODE(bits, (base_value << 1) | 1);
	}
	for (int i = 0; i < dc_c_tree_item_count; i++) {
		int dc_bits = dc_c_huffman_tree[i].dc_bits;
		int bits = dc_c_huffman_tree[i].c_bits + 1 + dc_bits;
		uint32_t base_value = dc_c_huffman_tree[i].c_value;

		int pos_offset = 1 << dc_bits;
		int neg_offset = pos_offset * 2 - 1;

		for (int j = 0; j < (1 << dc_bits); j++) {
			int pos = (j + pos_offset) & 0x1FF;
			int neg = (j - neg_offset) & 0x1FF;

			state->dc_huffman_map[(INDEX_CR << 9) | pos] = HUFFMAN_CODE(bits, (base_value << (dc_bits + 1)) | (1 << dc_bits) | j);
			state->dc_huffman_map[(INDEX_CR << 9) | neg] = HUFFMAN_CODE(bits, (base_value << (dc_bits + 1)) | (0 << dc_bits) | j);
			state->dc_huffman_map[(INDEX_CB << 9) | pos] = HUFFMAN_CODE(bits, (base_value << (dc_bits + 1)) | (1 << dc_bits) | j);
			state->dc_huffman_map[(INDEX_CB << 9) | neg] = HUFFMAN_CODE(bits, (base_value << (dc_bits + 1)) | (0 << dc_bits) | j);
		}
	}
	for (int i = 0; i < dc_y_tree_item_count; i++) {
		int dc_bits = dc_y_huffman_tree[i].dc_bits;
		int bits = dc_y_huffman_tree[i].c_bits + 1 + dc_bits;
		uint32_t base_value = dc_y_huffman_tree[i].c_value;

		int pos_offset = 1 << dc_bits;
		int neg_offset = pos_offset * 2 - 1;

		for (int j = 0; j < (1 << dc_bits); j++) {
			int pos = (j + pos_offset) & 0x1FF;
			int neg = (j - neg_offset) & 0x1FF;

			state->dc_huffman_map[(INDEX_Y << 9) | pos] = HUFFMAN_CODE(bits, (base_value << (dc_bits + 1)) | (1 << dc_bits) | j);
			state->dc_huffman_map[(INDEX_Y << 9) | neg] = HUFFMAN_CODE(bits, (base_value << (dc_bits + 1)) | (0 << dc_bits) | j);
		}
	}
}

static bool flush_bits(mdec_encoder_state_t *state) {
	if(state->bits_left < 16) {
		state->frame_output[state->bytes_used++] = (uint8_t)state->bits_value;
		if (state->bytes_used >= state->frame_max_size)
			return false;

		state->frame_output[state->bytes_used++] = (uint8_t)(state->bits_value>>8);
	}

	state->bits_left = 16;
	state->bits_value = 0;
	return true;
}

static bool encode_bits(mdec_encoder_state_t *state, int bits, uint32_t val) {
	assert(val < (1<<bits));

	// FIXME: for some reason the main logic breaks when bits > 16
	// and I have no idea why, so I have to split this up --GM
	if (bits > 16) {
		if (!encode_bits(state, bits-16, val>>16))
			return false;

		bits = 16;
		val &= 0xFFFF;
	}

	if (state->bits_left == 0) {
		if (!flush_bits(state))
			return false;
	}

	while (bits > state->bits_left) {
		// Bits need truncating
		uint32_t outval = val;
		outval >>= bits - state->bits_left;
		assert(outval < (1<<16));
		//uint16_t old_value = state->bits_value;
		assert((state->bits_value & outval) == 0);
		state->bits_value |= (uint16_t)outval;
		//fprintf(stderr, "trunc %2d %2d %08X %04X %04X\n", bits, state->bits_left, val, old_value, state->bits_value);
		bits -= state->bits_left;
		uint32_t mask = (1<<bits)-1;
		val &= mask;
		assert(mask >= 1);
		assert(val < (1<<bits));
		if (!flush_bits(state))
			return false;
	}

	if (bits >= 1) {
		assert(bits <= 16);
		// Bits may need shifting into place
		uint32_t outval = val;
		outval <<= state->bits_left - bits;
		assert(outval < (1<<16));
		//uint16_t old_value = state->bits_value;
		assert((state->bits_value & outval) == 0);
		state->bits_value |= (uint16_t)outval;
		//fprintf(stderr, "plop  %2d %2d %08X %04X %04X\n", bits, state->bits_left, val, state->bits_value);
		state->bits_left -= bits;
	}

	return true;
}

#if 0
static void transform_dct_block(int16_t *block) {
	// Apply DCT to block
	int midblock[8*8];

	for (int i = 0; i < 8; i++) {
	for (int j = 0; j < 8; j++) {
		int v = 0;
		for(int k = 0; k < 8; k++) {
			v += (int)block[8*j+k] * (int)dct_scale_table[8*i+k] / 8;
		}
		midblock[8*i+j] = (v + 0xFFF) >> 13;
	}
	}
	for (int i = 0; i < 8; i++) {
	for (int j = 0; j < 8; j++) {
		int v = 0;
		for(int k = 0; k < 8; k++) {
			v += (int)midblock[8*j+k] * (int)dct_scale_table[8*i+k];
		}
		block[8*i+j] = (int16_t)((v + 0xFFF) >> 13);
	}
	}
}

static int reduce_dct_block(mdec_encoder_state_t *state, int32_t *block, int32_t min_val, int *values_to_shed) {
	// Reduce so it can all fit
	int nonzeroes = 0;

	for (int i = 1; i < 64; i++) {
		//int ri = dct_zigzag_table[i];
		if (block[i] != 0) {
			//if (abs(block[i])+(ri>>3) < min_val+(64>>3)) {
			if ((*values_to_shed) > 0 && abs(block[i]) < min_val*1) {
				block[i] = 0;
				(*values_to_shed)--;
			} else {
				nonzeroes++;
			}
		}
	}

	// Factor in DC + EOF values
	return nonzeroes+2;
}
#endif

// https://stackoverflow.com/a/60011209
#if 0
#define DIVIDE_ROUNDED(n, d) (((n) >= 0) ? (((n) + (d)/2) / (d)) : (((n) - (d)/2) / (d)))
#else
#define DIVIDE_ROUNDED(n, d) ((int)round((double)(n) / (double)(d)))
#endif

static bool encode_dct_block(
	mdec_encoder_state_t *state,
	bs_codec_t codec,
	const int16_t *block,
	const int16_t *quant_table
) {
	int dc = DIVIDE_ROUNDED(block[0], quant_table[0]);

	dc = state->coeff_clamp_map[dc & 0xFFFF];

	if (codec == BS_CODEC_V2) {
		if (!encode_bits(state, 10, dc & 0x3FF))
			return false;
	} else {
		int index = state->block_type;

		if (index > INDEX_Y)
			index = INDEX_Y;

		int delta = DIVIDE_ROUNDED(dc - state->last_dc_values[index], 4);
		state->last_dc_values[index] += delta * 4;

		// Some versions of Sony's BS v3 decoder compute each DC coefficient as
		// ((last + delta * 4) & 0x3FF) instead of just (last + delta * 4). The
		// encoder can leverage this behavior to represent large coefficient
		// differences as smaller deltas that cause the decoder to overflow and
		// wrap around (e.g. -1 to encode -512 -> 511 as opposed to +1023). This
		// saves some space as larger DC values take up more bits.
		if (codec == BS_CODEC_V3DC) {
			if (delta < -0x80)
				delta += 0x100;
			else if (delta > +0x80)
				delta -= 0x100;
		}

		uint32_t outword = state->dc_huffman_map[(index << 9) | (delta & 0x1FF)];

		if (!encode_bits(state, outword >> 24, outword & 0xFFFFFF))
			return false;
	}

	for (int i = 1, zeroes = 0; i < 64; i++) {
		int ri = dct_zagzig_table[i];
		int ac = DIVIDE_ROUNDED(block[ri], quant_table[ri]);

		ac = state->coeff_clamp_map[ac & 0xFFFF];

		if (ac == 0) {
			zeroes++;
		} else {
			uint32_t outword = state->ac_huffman_map[(zeroes << 10) | (ac & 0x3FF)];

			if (!encode_bits(state, outword >> 24, outword & 0xFFFFFF))
				return false;

			zeroes = 0;
			state->uncomp_hwords_used++;
		}
	}

	// Store end of block
	if (!encode_bits(state, 2, 0x2))
		return false;

	state->block_type++;
	state->block_type %= 6;
	state->uncomp_hwords_used += 2;
	//state->uncomp_hwords_used = (state->uncomp_hwords_used+0xF)&~0xF;
	return true;
}

bool init_mdec_encoder(mdec_encoder_t *encoder, bs_codec_t video_codec, int video_width, int video_height) {
	encoder->video_codec = video_codec;
	encoder->video_width = video_width;
	encoder->video_height = video_height;

	mdec_encoder_state_t *state = &(encoder->state);

#if 0
	if (state->dct_context != NULL)
		return true;
#endif

	state->dct_context = avcodec_dct_alloc();
	state->ac_huffman_map = malloc(0x10000 * sizeof(uint32_t));
	state->dc_huffman_map = malloc(0x200 * 3 * sizeof(uint32_t));
	state->coeff_clamp_map = malloc(0x10000 * sizeof(int16_t));

	if (
		state->dct_context == NULL ||
		state->ac_huffman_map == NULL ||
		state->dc_huffman_map == NULL ||
		state->coeff_clamp_map == NULL
	)
		return false;

	int dct_block_count_x = (video_width + 15) / 16;
	int dct_block_count_y = (video_height + 15) / 16;
	int dct_block_size = dct_block_count_x * dct_block_count_y * sizeof(int16_t) * 8*8;

	for (int i = 0; i < 6; i++) {
		state->dct_block_lists[i] = malloc(dct_block_size);

		if (state->dct_block_lists[i] == NULL)
			return false;
	}

	avcodec_dct_init(state->dct_context);
	init_dct_data(state, video_codec);
	return true;
}

void destroy_mdec_encoder(mdec_encoder_t *encoder) {
	mdec_encoder_state_t *state = &(encoder->state);

	if (state->dct_context) {
		av_free(state->dct_context);
		state->dct_context = NULL;
	}
	if (state->ac_huffman_map) {
		free(state->ac_huffman_map);
		state->ac_huffman_map = NULL;
	}
	if (state->dc_huffman_map) {
		free(state->dc_huffman_map);
		state->dc_huffman_map = NULL;
	}
	if (state->coeff_clamp_map) {
		free(state->coeff_clamp_map);
		state->coeff_clamp_map = NULL;
	}
	for (int i = 0; i < 6; i++) {
		if (state->dct_block_lists[i] != NULL) {
			free(state->dct_block_lists[i]);
			state->dct_block_lists[i] = NULL;
		}
	}
}

void encode_frame_bs(mdec_encoder_t *encoder, const uint8_t *video_frame) {
	mdec_encoder_state_t *state = &(encoder->state);

	assert(state->dct_context);

	int pitch = encoder->video_width;
#if 0
	int real_index = state->frame_index - 1;
	if (real_index > (video_frame_count - 1))
		real_index = video_frame_count - 1;

	const uint8_t *y_plane = video_frames + encoder->video_width * encoder->video_height * 3/2 * real_index;
#else
	const uint8_t *y_plane = video_frame;
	const uint8_t *c_plane = y_plane + (encoder->video_width * encoder->video_height);
#endif

	int dct_block_count_x = (encoder->video_width + 15) / 16;
	int dct_block_count_y = (encoder->video_height + 15) / 16;

	// TODO: non-16x16-aligned videos
	assert((encoder->video_width % 16) == 0);
	assert((encoder->video_height % 16) == 0);

	// Rearrange the Y/C planes returned by libswscale into macroblocks.
	for (int fx = 0; fx < dct_block_count_x; fx++) {
		for (int fy = 0; fy < dct_block_count_y; fy++) {
			// Order: Cr Cb [Y1|Y2]
			//              [Y3|Y4]
			int block_offs = 64 * (fy*dct_block_count_x + fx);
			int16_t *blocks[6] = {
				state->dct_block_lists[0] + block_offs,
				state->dct_block_lists[1] + block_offs,
				state->dct_block_lists[2] + block_offs,
				state->dct_block_lists[3] + block_offs,
				state->dct_block_lists[4] + block_offs,
				state->dct_block_lists[5] + block_offs
			};

			for (int y = 0; y < 8; y++) {
				for (int x = 0; x < 8; x++) {
					int k = y*8 + x;
					int cx = fx*8 + x;
					int cy = fy*8 + y;
					int lx = fx*16 + x;
					int ly = fy*16 + y;

					blocks[0][k] = (int16_t)c_plane[pitch*cy + 2*cx + 0] - 128;
					blocks[1][k] = (int16_t)c_plane[pitch*cy + 2*cx + 1] - 128;
					blocks[2][k] = (int16_t)y_plane[pitch*(ly+0) + (lx+0)] - 128;
					blocks[3][k] = (int16_t)y_plane[pitch*(ly+0) + (lx+8)] - 128;
					blocks[4][k] = (int16_t)y_plane[pitch*(ly+8) + (lx+0)] - 128;
					blocks[5][k] = (int16_t)y_plane[pitch*(ly+8) + (lx+8)] - 128;
				}
			}

			for (int i = 0; i < 6; i++)
#if 0
				transform_dct_block(blocks[i]);
#else
				state->dct_context->fdct(blocks[i]);
#endif
		}
	}

	uint32_t end_of_block;

	if (encoder->video_codec == BS_CODEC_V2) {
		end_of_block = 0x1FF;
	} else {
		end_of_block = 0x3FF;
		assert(state->dc_huffman_map);
	}

	assert(state->ac_huffman_map);
	assert(state->coeff_clamp_map);

	// Attempt encoding the frame at the maximum quality. If the result is too
	// large, increase the quantization scale and try again.
	// TODO: if a frame encoded at scale N is too large but the same frame
	// encoded at scale N+1 leaves a significant amount of free space, attempt
	// compressing at scale N but optimizing coefficients away until it fits
	// (like the old algorithm did)
	for (
		state->quant_scale = 1;
		state->quant_scale < 64;
		state->quant_scale++
	) {
		int16_t quant_table[8*8];

		// The DC coefficient's quantization scale is always 8.
		quant_table[0] = quant_dec[0] * 8;

		for (int i = 1; i < 64; i++)
			quant_table[i] = quant_dec[i] * state->quant_scale;

		memset(state->frame_output, 0, state->frame_max_size);

		state->block_type = 0;
		state->last_dc_values[INDEX_CR] = 0;
		state->last_dc_values[INDEX_CB] = 0;
		state->last_dc_values[INDEX_Y] = 0;

		state->bits_value = 0;
		state->bits_left = 16;
		state->uncomp_hwords_used = 0;
		state->bytes_used = 8;

		bool ok = true;
		for (int fx = 0; ok && (fx < dct_block_count_x); fx++) {
			for (int fy = 0; ok && (fy < dct_block_count_y); fy++) {
				// Order: Cr Cb [Y1|Y2]
				//              [Y3|Y4]
				int block_offs = 64 * (fy*dct_block_count_x + fx);
				int16_t *blocks[6] = {
					state->dct_block_lists[0] + block_offs,
					state->dct_block_lists[1] + block_offs,
					state->dct_block_lists[2] + block_offs,
					state->dct_block_lists[3] + block_offs,
					state->dct_block_lists[4] + block_offs,
					state->dct_block_lists[5] + block_offs
				};

				for(int i = 0; ok && (i < 6); i++)
					ok = encode_dct_block(state, encoder->video_codec, blocks[i], quant_table);
			}
		}

		if (!ok)
			continue;
		if (!encode_bits(state, 10, end_of_block))
			continue;
#if 0
		if (!encode_bits(state, 2, 0x2))
			continue;
#endif
		if (!flush_bits(state))
			continue;

		state->uncomp_hwords_used += 2;
		state->quant_scale_sum += state->quant_scale;
		break;
	}
	assert(state->quant_scale < 64);

	// MDEC DMA is usually configured to transfer data in 32-word chunks.
	state->uncomp_hwords_used = (state->uncomp_hwords_used+0x3F)&~0x3F;

	// This is not the number of 32-byte blocks required for uncompressed data
	// as jPSXdec docs say, but rather the number of 32-*bit* words required.
	// The first 4 bytes of the frame header are in fact the MDEC command to
	// start decoding, which contains the data length in words in the lower 16
	// bits.
	state->blocks_used = (state->uncomp_hwords_used+1)>>1;

	// We need a multiple of 4
	state->bytes_used = (state->bytes_used+0x3)&~0x3;

	// MDEC command (size of decompressed MDEC data)
	state->frame_output[0x000] = (uint8_t)state->blocks_used;
	state->frame_output[0x001] = (uint8_t)(state->blocks_used>>8);
	state->frame_output[0x002] = (uint8_t)0x00;
	state->frame_output[0x003] = (uint8_t)0x38;

	// Quantization scale
	state->frame_output[0x004] = (uint8_t)state->quant_scale;
	state->frame_output[0x005] = (uint8_t)(state->quant_scale>>8);

	// BS version
	if (encoder->video_codec == BS_CODEC_V2)
		state->frame_output[0x006] = 0x02;
	else
		state->frame_output[0x006] = 0x03;

	state->frame_output[0x007] = 0x00;
}

int encode_sector_str(
	mdec_encoder_t *encoder,
	format_t format,
	uint16_t str_video_id,
	const uint8_t *video_frames,
	uint8_t *output
) {
	mdec_encoder_state_t *state = &(encoder->state);
	int frame_size = encoder->video_width * encoder->video_height * 2;
	int frames_used = 0;

	while (state->frame_data_offset >= state->frame_max_size) {
		state->frame_index++;
		// TODO: work out an optimal block count for this
		// TODO: calculate this all based on FPS
		state->frame_block_overflow_num += state->frame_block_base_overflow;
		state->frame_max_size = state->frame_block_overflow_num / state->frame_block_overflow_den * 2016;
		state->frame_block_overflow_num %= state->frame_block_overflow_den;
		state->frame_data_offset = 0;

		encode_frame_bs(encoder, video_frames);
		video_frames += frame_size;
		frames_used++;
	}

	uint8_t header[32];
	memset(header, 0, sizeof(header));

	// STR version
	header[0x000] = 0x60;
	header[0x001] = 0x01;

	// Chunk type
	header[0x002] = (uint8_t)str_video_id;
	header[0x003] = (uint8_t)(str_video_id >> 8);

	// Muxed chunk index/count
	int chunk_index = state->frame_data_offset / 2016;
	int chunk_count = state->frame_max_size / 2016;
	header[0x004] = (uint8_t)chunk_index;
	header[0x005] = (uint8_t)(chunk_index >> 8);
	header[0x006] = (uint8_t)chunk_count;
	header[0x007] = (uint8_t)(chunk_count >> 8);

	// Frame index
	header[0x008] = (uint8_t)state->frame_index;
	header[0x009] = (uint8_t)(state->frame_index >> 8);
	header[0x00A] = (uint8_t)(state->frame_index >> 16);
	header[0x00B] = (uint8_t)(state->frame_index >> 24);

	// Demuxed bytes used as a multiple of 4
	header[0x00C] = (uint8_t)state->bytes_used;
	header[0x00D] = (uint8_t)(state->bytes_used >> 8);
	header[0x00E] = (uint8_t)(state->bytes_used >> 16);
	header[0x00F] = (uint8_t)(state->bytes_used >> 24);

	// Video frame size
	header[0x010] = (uint8_t)encoder->video_width;
	header[0x011] = (uint8_t)(encoder->video_width >> 8);
	header[0x012] = (uint8_t)encoder->video_height;
	header[0x013] = (uint8_t)(encoder->video_height >> 8);

	// Copy of BS header
	memcpy(header + 0x014, state->frame_output, 8);

	int offset;

	if (format == FORMAT_STR)
		offset = 0x008;
	else if (format == FORMAT_STRCD)
		offset = 0x018;
	else
		offset = 0x000;

	memcpy(output + offset, header, sizeof(header));
	memcpy(output + offset + 0x020, state->frame_output + state->frame_data_offset, 2016);

	state->frame_data_offset += 2016;
	return frames_used;
}
