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

static void poll_av_packet(settings_t *settings, AVPacket *packet);

int decode_frame(AVCodecContext *codec, AVFrame *frame, int *frame_size, AVPacket *packet) {
	int ret;

	if (packet != NULL) {
		ret = avcodec_send_packet(codec, packet);
		if (ret != 0) {
			return 0;
		}
	}

	ret = avcodec_receive_frame(codec, frame);
	if (ret >= 0) {
		*frame_size = ret;
		return 1;
	} else {
		return ret == AVERROR(EAGAIN) ? 1 : 0;
	}
}

bool open_av_data(const char *filename, settings_t *settings, bool use_audio, bool use_video, bool audio_required, bool video_required)
{
	av_decoder_state_t* av = &(settings->decoder_state_av);
	av->video_next_pts = 0.0;
	av->frame = NULL;
	av->video_frame_src_size = 0;
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

	if (settings->quiet) {
		av_log_set_level(AV_LOG_QUIET);
	}

	av->format = avformat_alloc_context();
	if (avformat_open_input(&(av->format), filename, NULL, NULL)) {
		return false;
	}
	if (avformat_find_stream_info(av->format, NULL) < 0) {
		return false;
	}

	if (use_audio) {
		for (int i = 0; i < av->format->nb_streams; i++) {
			if (av->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				if (av->audio_stream_index >= 0) {
					fprintf(stderr, "Input file must have a single audio track\n");
					return false;
				}
				av->audio_stream_index = i;
			}
		}
		if (audio_required && av->audio_stream_index == -1) {
			fprintf(stderr, "Input file has no audio data\n");
			return false;
		}
	}

	if (use_video) {
		for (int i = 0; i < av->format->nb_streams; i++) {
			if (av->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
				if (av->video_stream_index >= 0) {
					fprintf(stderr, "Input file must have a single video track\n");
					return false;
				}
				av->video_stream_index = i;
			}
		}
		if (video_required && av->video_stream_index == -1) {
			fprintf(stderr, "Input file has no video data\n");
			return false;
		}
	}

	av->audio_stream = (av->audio_stream_index != -1 ? av->format->streams[av->audio_stream_index] : NULL);
	av->video_stream = (av->video_stream_index != -1 ? av->format->streams[av->video_stream_index] : NULL);

	if (av->audio_stream != NULL) {
		const AVCodec *codec = avcodec_find_decoder(av->audio_stream->codecpar->codec_id);
		av->audio_codec_context = avcodec_alloc_context3(codec);
		if (av->audio_codec_context == NULL) {
			return false;
		}
		if (avcodec_parameters_to_context(av->audio_codec_context, av->audio_stream->codecpar) < 0) {
			return false;
		}
		if (avcodec_open2(av->audio_codec_context, codec, NULL) < 0) {
			return false;
		}

		AVChannelLayout layout;
		layout.nb_channels = settings->channels;
		if (settings->channels <= 2) {
			layout.order = AV_CHANNEL_ORDER_NATIVE;
			layout.u.mask = (settings->channels == 2) ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
		} else {
			layout.order = AV_CHANNEL_ORDER_UNSPEC;
		}
		if (!settings->quiet && settings->channels > av->audio_codec_context->ch_layout.nb_channels) {
			fprintf(stderr, "Warning: input file has less than %d channels\n", settings->channels);
		}

		av->sample_count_mul = settings->channels;
		if (swr_alloc_set_opts2(
			&av->resampler,
			&layout,
			AV_SAMPLE_FMT_S16,
			settings->frequency,
			&av->audio_codec_context->ch_layout,
			av->audio_codec_context->sample_fmt,
			av->audio_codec_context->sample_rate,
			0,
			NULL
		) < 0) {
			return false;
		}
		if (settings->swresample_options) {
			if (av_opt_set_from_string(av->resampler, settings->swresample_options, NULL, "=", ":,") < 0) {
				return false;
			}
		}

		if (swr_init(av->resampler) < 0) {
			return false;
		}
	}

	if (av->video_stream != NULL) {
		const AVCodec *codec = avcodec_find_decoder(av->video_stream->codecpar->codec_id);
		av->video_codec_context = avcodec_alloc_context3(codec);
		if(av->video_codec_context == NULL) {
			return false;
		}
		if (avcodec_parameters_to_context(av->video_codec_context, av->video_stream->codecpar) < 0) {
			return false;
		}
		if (avcodec_open2(av->video_codec_context, codec, NULL) < 0) {
			return false;
		}

		if (!settings->quiet && (
			settings->video_width > av->video_codec_context->width ||
			settings->video_height > av->video_codec_context->height
		)) {
			fprintf(stderr, "Warning: input file has resolution lower than %dx%d\n",
				settings->video_width, settings->video_height
			);
		}
		if (!settings->ignore_aspect_ratio) {
			// Reduce the provided size so that it matches the input file's
			// aspect ratio.
			double src_ratio = (double)av->video_codec_context->width / (double)av->video_codec_context->height;
			double dst_ratio = (double)settings->video_width / (double)settings->video_height;
			if (src_ratio < dst_ratio) {
				settings->video_width = (int)((double)settings->video_height * src_ratio + 15.0) & ~15;
			} else {
				settings->video_height = (int)((double)settings->video_width / src_ratio + 15.0) & ~15;
			}
		}

		av->scaler = sws_getContext(
			av->video_codec_context->width,
			av->video_codec_context->height,
			av->video_codec_context->pix_fmt,
			settings->video_width,
			settings->video_height,
			AV_PIX_FMT_NV21,
			SWS_BICUBIC,
			NULL,
			NULL,
			NULL
		);
		// Is this even necessary? -- spicyjpeg
		sws_setColorspaceDetails(
			av->scaler,
			sws_getCoefficients(av->video_codec_context->colorspace),
			(av->video_codec_context->color_range == AVCOL_RANGE_JPEG),
			sws_getCoefficients(SWS_CS_ITU601),
			true,
			0,
			0,
			0
		);
		if (settings->swscale_options) {
			if (av_opt_set_from_string(av->scaler, settings->swscale_options, NULL, "=", ":,") < 0) {
				return false;
			}
		}

		av->video_frame_src_size = 4*av->video_codec_context->width*av->video_codec_context->height;
		av->video_frame_dst_size = 3*settings->video_width*settings->video_height/2;
	}

	av->frame = av_frame_alloc();
	if (av->frame == NULL) {
		return false;
	}

	settings->audio_samples = NULL;
	settings->audio_sample_count = 0;
	settings->video_frames = NULL;
	settings->video_frame_count = 0;
	settings->end_of_input = false;

	return true;
}

static void poll_av_packet_audio(settings_t *settings, AVPacket *packet)
{
	av_decoder_state_t* av = &(settings->decoder_state_av);

	int frame_size, frame_sample_count;
	uint8_t *buffer[1];

	if (decode_frame(av->audio_codec_context, av->frame, &frame_size, packet)) {
		size_t buffer_size = sizeof(int16_t) * av->sample_count_mul * swr_get_out_samples(av->resampler, av->frame->nb_samples);
		buffer[0] = malloc(buffer_size);
		memset(buffer[0], 0, buffer_size);
		frame_sample_count = swr_convert(av->resampler, buffer, av->frame->nb_samples, (const uint8_t**)av->frame->data, av->frame->nb_samples);
		settings->audio_samples = realloc(settings->audio_samples, (settings->audio_sample_count + ((frame_sample_count + 4032) * av->sample_count_mul)) * sizeof(int16_t));
		memmove(&(settings->audio_samples[settings->audio_sample_count]), buffer[0], sizeof(int16_t) * frame_sample_count * av->sample_count_mul);
		settings->audio_sample_count += frame_sample_count * av->sample_count_mul;
		free(buffer[0]);
	}
}

static void poll_av_packet_video(settings_t *settings, AVPacket *packet)
{
	av_decoder_state_t* av = &(settings->decoder_state_av);

	int frame_size;
	double pts_step = ((double)1.0*(double)settings->video_fps_den)/(double)settings->video_fps_num;

	int plane_size = settings->video_width*settings->video_height;
	int dst_strides[2] = {
		settings->video_width, settings->video_width
	};

	if (decode_frame(av->video_codec_context, av->frame, &frame_size, packet)) {
		if (!av->frame->width || !av->frame->height || !av->frame->data) {
			return;
		}

		// Some files seem to have timestamps starting from a negative value
		// (but otherwise valid) for whatever reason.
		double pts = (((double)av->frame->pts)*(double)av->video_stream->time_base.num)/av->video_stream->time_base.den;
		//if (pts < 0.0) {
			//return;
		//}
		if (settings->video_frame_count >= 1 && pts < av->video_next_pts) {
			return;
		}
		if ((settings->video_frame_count) < 1) {
			av->video_next_pts = pts;
		} else {
			av->video_next_pts += pts_step;
		}

		//fprintf(stderr, "%d %f %f %f\n", (settings->video_frame_count), pts, av->video_next_pts, pts_step);

		// Insert duplicate frames if the frame rate of the input stream is
		// lower than the target frame rate.
		int dupe_frames = (int) ceil((pts - av->video_next_pts) / pts_step);
		if (dupe_frames < 0) dupe_frames = 0;
		settings->video_frames = realloc(
			settings->video_frames,
			(settings->video_frame_count + dupe_frames + 1) * av->video_frame_dst_size
		);

		for (; dupe_frames; dupe_frames--) {
			memcpy(
				(settings->video_frames) + av->video_frame_dst_size*(settings->video_frame_count),
				(settings->video_frames) + av->video_frame_dst_size*(settings->video_frame_count-1),
				av->video_frame_dst_size
			);
			settings->video_frame_count += 1;
			av->video_next_pts += pts_step;
		}

		uint8_t *dst_frame = (settings->video_frames) + av->video_frame_dst_size*(settings->video_frame_count);
		uint8_t *dst_pointers[2] = {
			dst_frame, dst_frame + plane_size
		};
		sws_scale(av->scaler, (const uint8_t *const *) av->frame->data, av->frame->linesize, 0, av->frame->height, dst_pointers, dst_strides);

		settings->video_frame_count += 1;
	}
}

bool poll_av_data(settings_t *settings)
{
	av_decoder_state_t* av = &(settings->decoder_state_av);
	AVPacket packet;

	if (settings->end_of_input) {
		return false;
	}

	if (av_read_frame(av->format, &packet) >= 0) {
		if (packet.stream_index == av->audio_stream_index) {
			poll_av_packet_audio(settings, &packet);
		} else if (packet.stream_index == av->video_stream_index) {
			poll_av_packet_video(settings, &packet);
		}
		av_packet_unref(&packet);
		return true;
	} else {
		// out is always padded out with 4032 "0" samples, this makes calculations elsewhere easier
		if (av->audio_stream) {
			memset((settings->audio_samples) + (settings->audio_sample_count), 0, 4032 * av->sample_count_mul * sizeof(int16_t));
		}

		settings->end_of_input = true;
		return false;
	}
}

bool ensure_av_data(settings_t *settings, int needed_audio_samples, int needed_video_frames)
{
	while (settings->audio_sample_count < needed_audio_samples || settings->video_frame_count < needed_video_frames) {
		//fprintf(stderr, "ensure %d -> %d, %d -> %d\n", settings->audio_sample_count, needed_audio_samples, settings->video_frame_count, needed_video_frames);
		if (!poll_av_data(settings)) {
			// Keep returning true even if the end of the input file has been
			// reached, if the buffer is not yet completely empty.
			return (settings->audio_sample_count || !needed_audio_samples)
				&& (settings->video_frame_count || !needed_video_frames);
		}
	}
	//fprintf(stderr, "ensure %d -> %d, %d -> %d\n", settings->audio_sample_count, needed_audio_samples, settings->video_frame_count, needed_video_frames);

	return true;
}

void retire_av_data(settings_t *settings, int retired_audio_samples, int retired_video_frames)
{
	av_decoder_state_t* av = &(settings->decoder_state_av);

	//fprintf(stderr, "retire %d -> %d, %d -> %d\n", settings->audio_sample_count, retired_audio_samples, settings->video_frame_count, retired_video_frames);
	assert(retired_audio_samples <= settings->audio_sample_count);
	assert(retired_video_frames <= settings->video_frame_count);

	int sample_size = sizeof(int16_t);
	if (settings->audio_sample_count > retired_audio_samples) {
		memmove(settings->audio_samples, settings->audio_samples + retired_audio_samples, (settings->audio_sample_count - retired_audio_samples)*sample_size);
	}
	settings->audio_sample_count -= retired_audio_samples;

	int frame_size = av->video_frame_dst_size;
	if (settings->video_frame_count > retired_video_frames) {
		memmove(settings->video_frames, settings->video_frames + retired_video_frames*frame_size, (settings->video_frame_count - retired_video_frames)*frame_size);
	}
	settings->video_frame_count -= retired_video_frames;
}

void close_av_data(settings_t *settings)
{
	av_decoder_state_t* av = &(settings->decoder_state_av);

	av_frame_free(&(av->frame));
	swr_free(&(av->resampler));
	avcodec_close(av->audio_codec_context);
	avcodec_free_context(&(av->audio_codec_context));
	avformat_free_context(av->format);

	if(settings->audio_samples != NULL) {
		free(settings->audio_samples);
		settings->audio_samples = NULL;
	}
	if(settings->video_frames != NULL) {
		free(settings->video_frames);
		settings->video_frames = NULL;
	}
}
