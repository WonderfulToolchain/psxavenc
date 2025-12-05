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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/avdct.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include "args.h"
#include "decoding.h"

static bool decode_frame(AVCodecContext *codec, AVFrame *frame, int *frame_size, AVPacket *packet) {
	if (packet != NULL) {
		if (avcodec_send_packet(codec, packet) != 0)
			return false;
	}

	int ret = avcodec_receive_frame(codec, frame);

	if (ret >= 0) {
		*frame_size = ret;
		return true;
	}
	if (ret == AVERROR(EAGAIN))
		return true;

	return false;
}

bool open_av_data(decoder_t *decoder, const args_t *args, int flags) {
	decoder->audio_samples = NULL;
	decoder->audio_sample_count = 0;
	decoder->video_frames = NULL;
	decoder->video_frame_count = 0;

	decoder->video_width = args->video_width;
	decoder->video_height = args->video_height;
	decoder->video_fps_num = args->str_fps_num;
	decoder->video_fps_den = args->str_fps_den;
	decoder->end_of_input = false;

	decoder_state_t *av = &(decoder->state);

	av->video_next_pts = 0.0;
	av->frame = NULL;
	av->video_frame_dst_size = 0;
	av->audio_stream_index = -1;
	av->video_stream_index = -1;
	av->format = NULL;
	av->audio_stream = NULL;
	av->video_stream = NULL;
	av->audio_codec_context = NULL;
	av->video_codec_context = NULL;
	av->resampler = NULL;
	av->scaler = NULL;

	if (args->flags & FLAG_QUIET)
		av_log_set_level(AV_LOG_QUIET);

	av->format = avformat_alloc_context();

	if (avformat_open_input(&(av->format), args->input_file, NULL, NULL))
		return false;
	if (avformat_find_stream_info(av->format, NULL) < 0)
		return false;

	if (flags & DECODER_USE_AUDIO) {
		for (int i = 0; i < av->format->nb_streams; i++) {
			if (av->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				if (av->audio_stream_index >= 0) {
					fprintf(stderr, "Input file must have a single audio track\n");
					return false;
				}
				av->audio_stream_index = i;
			}
		}

		if ((flags & DECODER_AUDIO_REQUIRED) && av->audio_stream_index == -1) {
			fprintf(stderr, "Input file has no audio data\n");
			return false;
		}
	}

	if (flags & DECODER_USE_VIDEO) {
		for (int i = 0; i < av->format->nb_streams; i++) {
			if (av->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				if (av->video_stream_index >= 0) {
					fprintf(stderr, "Input file must have a single video track\n");
					return false;
				}
				av->video_stream_index = i;
			}
		}

		if ((flags & DECODER_VIDEO_REQUIRED) && av->video_stream_index == -1) {
			fprintf(stderr, "Input file has no video data\n");
			return false;
		}
	}

	av->audio_stream = (av->audio_stream_index != -1 ? av->format->streams[av->audio_stream_index] : NULL);
	av->video_stream = (av->video_stream_index != -1 ? av->format->streams[av->video_stream_index] : NULL);

	if (av->audio_stream != NULL) {
		const AVCodec *codec = avcodec_find_decoder(av->audio_stream->codecpar->codec_id);
		av->audio_codec_context = avcodec_alloc_context3(codec);

		if (av->audio_codec_context == NULL)
			return false;
		if (avcodec_parameters_to_context(av->audio_codec_context, av->audio_stream->codecpar) < 0)
			return false;
		if (avcodec_open2(av->audio_codec_context, codec, NULL) < 0)
			return false;

		AVChannelLayout layout;
		layout.nb_channels = args->audio_channels;

		if (args->audio_channels == 1) {
			layout.order = AV_CHANNEL_ORDER_NATIVE;
			layout.u.mask = AV_CH_LAYOUT_MONO;
		} else if (args->audio_channels == 2) {
			layout.order = AV_CHANNEL_ORDER_NATIVE;
			layout.u.mask = AV_CH_LAYOUT_STEREO;
		} else {
			layout.order = AV_CHANNEL_ORDER_UNSPEC;
		}

		if (!(args->flags & FLAG_QUIET)) {
			if (args->audio_channels > av->audio_codec_context->ch_layout.nb_channels)
				fprintf(stderr, "Warning: input file has less than %d channels\n", args->audio_channels);
		}

		av->sample_count_mul = args->audio_channels;

		if (swr_alloc_set_opts2(
			&av->resampler,
			&layout,
			AV_SAMPLE_FMT_S16,
			args->audio_frequency,
			&av->audio_codec_context->ch_layout,
			av->audio_codec_context->sample_fmt,
			av->audio_codec_context->sample_rate,
			0,
			NULL
		) < 0) {
			return false;
		}
		if (args->swresample_options) {
			if (av_opt_set_from_string(av->resampler, args->swresample_options, NULL, "=", ":,") < 0)
				return false;
		}
		if (swr_init(av->resampler) < 0)
			return false;
	}

	if (av->video_stream != NULL) {
		const AVCodec *codec = avcodec_find_decoder(av->video_stream->codecpar->codec_id);
		av->video_codec_context = avcodec_alloc_context3(codec);

		if (av->video_codec_context == NULL)
			return false;
		if (avcodec_parameters_to_context(av->video_codec_context, av->video_stream->codecpar) < 0)
			return false;
		if (avcodec_open2(av->video_codec_context, codec, NULL) < 0)
			return false;

		if (!(args->flags & FLAG_QUIET)) {
			if (
				decoder->video_width > av->video_codec_context->width ||
				decoder->video_height > av->video_codec_context->height
			)
				fprintf(stderr, "Warning: input file has resolution lower than %dx%d\n", decoder->video_width, decoder->video_height);
		}

		if (!(args->flags & FLAG_BS_IGNORE_ASPECT)) {
			// Reduce the provided size so that it matches the input file's
			// aspect ratio.
			double src_ratio = (double)av->video_codec_context->width / (double)av->video_codec_context->height;
			double dst_ratio = (double)decoder->video_width / (double)decoder->video_height;

			if (src_ratio < dst_ratio) {
				decoder->video_width = (int)((double)decoder->video_height * src_ratio + 15.0) & ~15;
			} else {
				decoder->video_height = (int)((double)decoder->video_width / src_ratio + 15.0) & ~15;
			}
		}

		av->scaler = sws_getContext(
			av->video_codec_context->width,
			av->video_codec_context->height,
			av->video_codec_context->pix_fmt,
			decoder->video_width,
			decoder->video_height,
			AV_PIX_FMT_NV21,
			SWS_BICUBIC,
			NULL,
			NULL,
			NULL
		);
		if (av->scaler == NULL)
			return false;
		if (sws_setColorspaceDetails(
			av->scaler,
			sws_getCoefficients(av->video_codec_context->colorspace),
			av->video_codec_context->color_range == AVCOL_RANGE_JPEG,
			sws_getCoefficients(SWS_CS_ITU601),
			true,
			0,
			1 << 16,
			1 << 16
		) < 0)
			return false;
		if (args->swscale_options) {
			if (av_opt_set_from_string(av->scaler, args->swscale_options, NULL, "=", ":,") < 0)
				return false;
		}

		av->video_frame_dst_size = 3 * decoder->video_width * decoder->video_height / 2;
	}

	av->frame = av_frame_alloc();

	if (av->frame == NULL)
		return false;

	return true;
}

static void poll_av_packet_audio(decoder_t *decoder, AVPacket *packet) {
	decoder_state_t *av = &(decoder->state);

	int frame_size;

	if (!decode_frame(av->audio_codec_context, av->frame, &frame_size, packet))
		return;

	int frame_sample_count = swr_get_out_samples(av->resampler, av->frame->nb_samples);

	if (frame_sample_count == 0)
		return;

	size_t buffer_size = sizeof(int16_t) * av->sample_count_mul * frame_sample_count;
	uint8_t *buffer = malloc(buffer_size);
	memset(buffer, 0, buffer_size);

	frame_sample_count = swr_convert(
		av->resampler,
		&buffer,
		frame_sample_count,
		(const uint8_t**)av->frame->data,
		av->frame->nb_samples
	);

	decoder->audio_samples = realloc(
		decoder->audio_samples,
		(decoder->audio_sample_count + ((frame_sample_count + 4032) * av->sample_count_mul)) * sizeof(int16_t)
	);
	memmove(
		&(decoder->audio_samples[decoder->audio_sample_count]),
		buffer,
		sizeof(int16_t) * frame_sample_count * av->sample_count_mul
	);
	decoder->audio_sample_count += frame_sample_count * av->sample_count_mul;
	free(buffer);
}

static void poll_av_packet_video(decoder_t *decoder, AVPacket *packet) {
	decoder_state_t *av = &(decoder->state);

	int frame_size;
	double pts_step = (double)decoder->video_fps_den / (double)decoder->video_fps_num;

	int plane_size = decoder->video_width * decoder->video_height;
	int dst_strides[2] = {
		decoder->video_width, decoder->video_width
	};

	if (!decode_frame(av->video_codec_context, av->frame, &frame_size, packet))
		return;
	if (!av->frame->width || !av->frame->height || !av->frame->data[0])
		return;

	// Some files seem to have timestamps starting from a negative value
	// (but otherwise valid) for whatever reason.
	double pts =
		((double)av->frame->pts * (double)av->video_stream->time_base.num)
		/ av->video_stream->time_base.den;
#if 0
	if (pts < 0.0)
		return;
#endif
	if (decoder->video_frame_count >= 1 && pts < av->video_next_pts)
		return;
	if (decoder->video_frame_count < 1)
		av->video_next_pts = pts;
	else
		av->video_next_pts += pts_step;

	//fprintf(stderr, "%d %f %f %f\n", decoder->video_frame_count, pts, av->video_next_pts, pts_step);

	// Insert duplicate frames if the frame rate of the input stream is
	// lower than the target frame rate.
	int dupe_frames = (int) ceil((pts - av->video_next_pts) / pts_step);
	if (dupe_frames < 0) dupe_frames = 0;
	decoder->video_frames = realloc(
		decoder->video_frames,
		(decoder->video_frame_count + dupe_frames + 1) * av->video_frame_dst_size
	);

	for (; dupe_frames; dupe_frames--) {
		memcpy(
			(decoder->video_frames) + av->video_frame_dst_size * decoder->video_frame_count,
			(decoder->video_frames) + av->video_frame_dst_size * (decoder->video_frame_count - 1),
			av->video_frame_dst_size
		);
		decoder->video_frame_count += 1;
		av->video_next_pts += pts_step;
	}

	uint8_t *dst_frame = decoder->video_frames + av->video_frame_dst_size * decoder->video_frame_count;
	uint8_t *dst_pointers[2] = {
		dst_frame, dst_frame + plane_size
	};
	sws_scale(
		av->scaler,
		(const uint8_t *const *) av->frame->data,
		av->frame->linesize,
		0,
		av->frame->height,
		dst_pointers,
		dst_strides
	);

	decoder->video_frame_count += 1;
}

bool poll_av_data(decoder_t *decoder) {
	decoder_state_t *av = &(decoder->state);

	if (decoder->end_of_input)
		return false;

	AVPacket packet;

	if (av_read_frame(av->format, &packet) >= 0) {
		if (packet.stream_index == av->audio_stream_index)
			poll_av_packet_audio(decoder, &packet);
		else if (packet.stream_index == av->video_stream_index)
			poll_av_packet_video(decoder, &packet);

		av_packet_unref(&packet);
		return true;
	} else {
		// out is always padded out with 4032 "0" samples, this makes calculations elsewhere easier
		if (av->audio_stream)
			memset(
				decoder->audio_samples + decoder->audio_sample_count,
				0,
				4032 * av->sample_count_mul * sizeof(int16_t)
			);

		decoder->end_of_input = true;
		return false;
	}
}

bool ensure_av_data(decoder_t *decoder, int needed_audio_samples, int needed_video_frames) {
	// HACK: in order to update decoder->end_of_input as soon as all data has
	// been read from the input file, this loop waits for more data than
	// strictly needed.
#if 0
	while (decoder->audio_sample_count < needed_audio_samples || decoder->video_frame_count < needed_video_frames) {
#else
	while (
		(needed_audio_samples && decoder->audio_sample_count <= needed_audio_samples) ||
		(needed_video_frames && decoder->video_frame_count <= needed_video_frames)
	) {
#endif
		//fprintf(stderr, "ensure %d -> %d, %d -> %d\n", decoder->audio_sample_count, needed_audio_samples, decoder->video_frame_count, needed_video_frames);
		if (!poll_av_data(decoder)) {
			// Keep returning true even if the end of the input file has been
			// reached, if the buffer is not yet completely empty.
			return
				(decoder->audio_sample_count || !needed_audio_samples) &&
				(decoder->video_frame_count || !needed_video_frames);
		}
	}
	//fprintf(stderr, "ensure %d -> %d, %d -> %d\n", decoder->audio_sample_count, needed_audio_samples, decoder->video_frame_count, needed_video_frames);

	return true;
}

void retire_av_data(decoder_t *decoder, int retired_audio_samples, int retired_video_frames) {
	//fprintf(stderr, "retire %d -> %d, %d -> %d\n", decoder->audio_sample_count, retired_audio_samples, decoder->video_frame_count, retired_video_frames);
	assert(retired_audio_samples <= decoder->audio_sample_count);
	assert(retired_video_frames <= decoder->video_frame_count);

	int sample_size = sizeof(int16_t);
	int frame_size = decoder->state.video_frame_dst_size;

	if (decoder->audio_sample_count > retired_audio_samples)
		memmove(
			decoder->audio_samples,
			decoder->audio_samples + retired_audio_samples,
			(decoder->audio_sample_count - retired_audio_samples) * sample_size
		);
	if (decoder->video_frame_count > retired_video_frames)
		memmove(
			decoder->video_frames,
			decoder->video_frames + retired_video_frames * frame_size,
			(decoder->video_frame_count - retired_video_frames) * frame_size
		);

	decoder->audio_sample_count -= retired_audio_samples;
	decoder->video_frame_count -= retired_video_frames;
}

void close_av_data(decoder_t *decoder) {
	decoder_state_t *av = &(decoder->state);

	av_frame_free(&(av->frame));
	swr_free(&(av->resampler));
#if LIBAVCODEC_VERSION_MAJOR < 61
	// Deprecated, kept for compatibility with older FFmpeg versions.
	avcodec_close(av->audio_codec_context);
#endif
	avcodec_free_context(&(av->audio_codec_context));
	avformat_free_context(av->format);

	if(decoder->audio_samples != NULL) {
		free(decoder->audio_samples);
		decoder->audio_samples = NULL;
	}
	if(decoder->video_frames != NULL) {
		free(decoder->video_frames);
		decoder->video_frames = NULL;
	}
}
