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
#include "args.h"
#include "decoding.h"
#include "filefmt.h"

static const char *const bs_codec_names[NUM_BS_CODECS] = {
	"BS v2",
	"BS v3",
	"BS v3 (with DC wrapping)"
};

static const uint8_t decoder_flags[NUM_FORMATS] = {
	DECODER_USE_AUDIO | DECODER_AUDIO_REQUIRED, // xa
	DECODER_USE_AUDIO | DECODER_AUDIO_REQUIRED, // xacd
	DECODER_USE_AUDIO | DECODER_AUDIO_REQUIRED, // spu
	DECODER_USE_AUDIO | DECODER_AUDIO_REQUIRED, // vag
	DECODER_USE_AUDIO | DECODER_AUDIO_REQUIRED, // spui
	DECODER_USE_AUDIO | DECODER_AUDIO_REQUIRED, // vagi
	DECODER_USE_AUDIO | DECODER_USE_VIDEO | DECODER_VIDEO_REQUIRED, // str
	DECODER_USE_AUDIO | DECODER_USE_VIDEO | DECODER_VIDEO_REQUIRED, // strcd
	DECODER_USE_AUDIO | DECODER_USE_VIDEO | DECODER_VIDEO_REQUIRED, // strspu
	DECODER_USE_VIDEO | DECODER_VIDEO_REQUIRED, // strv
	DECODER_USE_VIDEO | DECODER_VIDEO_REQUIRED // sbs
};

int main(int argc, const char **argv) {
	args_t args;
	decoder_t decoder;
	FILE *output;

	args.flags = 0;

	args.format = FORMAT_INVALID;
	args.input_file = NULL;
	args.output_file = NULL;
	args.swresample_options = NULL;
	args.swscale_options = NULL;

	if (!parse_args(&args, argv + 1, argc - 1))
		return 1;
	if (!open_av_data(&decoder, &args, decoder_flags[args.format])) {
		fprintf(stderr, "Failed to open input file: %s\n", args.input_file);
		return 1;
	}

	output = fopen(args.output_file, "wb");

	if (output == NULL) {
		fprintf(stderr, "Failed to open output file: %s\n", args.output_file);
		close_av_data(&decoder);
		return 1;
	}

	switch (args.format) {
		case FORMAT_XA:
		case FORMAT_XACD:
			if (!(args.flags & FLAG_QUIET))
				fprintf(
					stderr,
					"Audio format: XA-ADPCM, %d Hz %d-bit %s, F=%d C=%d\n",
					args.audio_frequency,
					args.audio_bit_depth,
					(args.audio_channels == 2) ? "stereo" : "mono",
					args.audio_xa_file,
					args.audio_xa_channel
				);

			encode_file_xa(&args, &decoder, output);
			break;

		case FORMAT_SPU:
		case FORMAT_VAG:
			if (!(args.flags & FLAG_OVERRIDE_LOOP_POINT)) {
				args.audio_loop_point = get_av_loop_point(&decoder, &args);

				if (args.audio_loop_point >= 0)
					args.flags |= FLAG_SPU_ENABLE_LOOP;
			}

			if (!(args.flags & FLAG_QUIET))
				fprintf(
					stderr,
					"Audio format: SPU-ADPCM, %d Hz mono\n",
					args.audio_frequency
				);

			encode_file_spu(&args, &decoder, output);
			break;

		case FORMAT_SPUI:
		case FORMAT_VAGI:
			if (!(args.flags & FLAG_OVERRIDE_LOOP_POINT))
				args.audio_loop_point = get_av_loop_point(&decoder, &args);

			if (!(args.flags & FLAG_QUIET))
				fprintf(
					stderr,
					"Audio format: SPU-ADPCM, %d Hz %d channels, interleave=%d\n",
					args.audio_frequency,
					args.audio_channels,
					args.audio_interleave
				);

			encode_file_spui(&args, &decoder, output);
			break;

		case FORMAT_STR:
		case FORMAT_STRCD:
			if (!(args.flags & FLAG_QUIET)) {
				if (decoder.state.audio_stream != NULL)
					fprintf(
						stderr,
						"Audio format: XA-ADPCM, %d Hz %d-bit %s, F=%d C=%d\n",
						args.audio_frequency,
						args.audio_bit_depth,
						(args.audio_channels == 2) ? "stereo" : "mono",
						args.audio_xa_file,
						args.audio_xa_channel
					);

				fprintf(
					stderr,
					"Video format: %s, %dx%d, %.2f fps\n",
					bs_codec_names[args.video_codec],
					args.video_width,
					args.video_height,
					(double)args.str_fps_num / (double)args.str_fps_den
				);
			}

			encode_file_str(&args, &decoder, output);
			break;

		case FORMAT_STRSPU:
			// TODO: implement and remove this check
			fprintf(stderr, "This format is not currently supported\n");
			break;

		case FORMAT_STRV:
			if (!(args.flags & FLAG_QUIET)) {
				if (decoder.state.audio_stream != NULL)
					fprintf(
						stderr,
						"Audio format: SPU-ADPCM, %d Hz %d channels, interleave=%d\n",
						args.audio_frequency,
						args.audio_channels,
						args.audio_interleave
					);

				fprintf(
					stderr,
					"Video format: %s, %dx%d, %.2f fps\n",
					bs_codec_names[args.video_codec],
					args.video_width,
					args.video_height,
					(double)args.str_fps_num / (double)args.str_fps_den
				);
			}

			encode_file_strspu(&args, &decoder, output);
			break;

		case FORMAT_SBS:
			if (!(args.flags & FLAG_QUIET))
				fprintf(
					stderr,
					"Video format: %s, %dx%d, %.2f fps\n",
					bs_codec_names[args.video_codec],
					args.video_width,
					args.video_height,
					(double)args.str_fps_num / (double)args.str_fps_den
				);

			encode_file_sbs(&args, &decoder, output);
			break;

		default:
			;
	}

	if (!(args.flags & FLAG_HIDE_PROGRESS))
		fprintf(stderr, "\nDone.\n");

	fclose(output);
	close_av_data(&decoder);
	return 0;
}
