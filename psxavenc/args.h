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

#define NUM_FORMATS   11
#define NUM_BS_CODECS 3

enum {
	FLAG_IGNORE_OPTIONS       = 1 << 0,
	FLAG_QUIET                = 1 << 1,
	FLAG_HIDE_PROGRESS        = 1 << 2,
	FLAG_PRINT_HELP           = 1 << 3,
	FLAG_PRINT_VERSION        = 1 << 4,
	FLAG_OVERRIDE_LOOP_POINT  = 1 << 5,
	FLAG_SPU_ENABLE_LOOP      = 1 << 6,
	FLAG_SPU_NO_LEADING_DUMMY = 1 << 7,
	FLAG_BS_IGNORE_ASPECT     = 1 << 8,
	FLAG_STR_TRAILING_AUDIO   = 1 << 9
};

typedef enum {
	FORMAT_INVALID = -1,
	FORMAT_XA,
	FORMAT_XACD,
	FORMAT_SPU,
	FORMAT_VAG,
	FORMAT_SPUI,
	FORMAT_VAGI,
	FORMAT_STR,
	FORMAT_STRCD,
	FORMAT_STRSPU,
	FORMAT_STRV,
	FORMAT_SBS
} format_t;

typedef enum {
	BS_CODEC_INVALID = -1,
	BS_CODEC_V2,
	BS_CODEC_V3,
	BS_CODEC_V3DC
} bs_codec_t;

typedef struct {
	int flags;

	format_t format;
	const char *input_file;
	const char *output_file;
	const char *swresample_options;
	const char *swscale_options;

	int audio_frequency; // 18900 or 37800 Hz
	int audio_channels;
	int audio_bit_depth; // 4 or 8
	int audio_xa_file; // 00-FF
	int audio_xa_channel; // 00-1F
	int audio_interleave;
	int audio_loop_point;

	bs_codec_t video_codec;
	int video_width;
	int video_height;

	int str_fps_num;
	int str_fps_den;
	int str_cd_speed; // 1 or 2
	int str_video_id;
	int str_audio_id;
	int alignment;
} args_t;

bool parse_args(args_t *args, const char *const *options, int count);
