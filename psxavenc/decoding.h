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
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/avdct.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "args.h"

typedef struct {
	int video_frame_dst_size;
	int audio_stream_index;
	int video_stream_index;
	AVFormatContext* format;
	AVStream* audio_stream;
	AVStream* video_stream;
	AVCodecContext* audio_codec_context;
	AVCodecContext* video_codec_context;
	struct SwrContext* resampler;
	struct SwsContext* scaler;
	AVFrame* frame;

	int sample_count_mul;

	double video_next_pts;
} decoder_state_t;

typedef struct {
	int16_t *audio_samples;
	int audio_sample_count;
	uint8_t *video_frames;
	int video_frame_count;

	int video_width;
	int video_height;
	int video_fps_num;
	int video_fps_den;
	bool end_of_input;

	decoder_state_t state;
} decoder_t;

enum {
	DECODER_USE_AUDIO      = 1 << 0,
	DECODER_USE_VIDEO      = 1 << 1,
	DECODER_AUDIO_REQUIRED = 1 << 2,
	DECODER_VIDEO_REQUIRED = 1 << 3
};

bool open_av_data(decoder_t *decoder, const args_t *args, int flags);
int get_av_loop_point(decoder_t *decoder, const args_t *args);
bool poll_av_data(decoder_t *decoder);
bool ensure_av_data(decoder_t *decoder, int needed_audio_samples, int needed_video_frames);
void retire_av_data(decoder_t *decoder, int retired_audio_samples, int retired_video_frames);
void close_av_data(decoder_t *decoder);
