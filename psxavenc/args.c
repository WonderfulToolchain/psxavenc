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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "args.h"
#include "config.h"

#define INVALID_PARAM -1

static int parse_int(
	int *output,
	const char *name,
	const char *value,
	int min_value,
	int max_value
) {
	if (value == NULL) {
		fprintf(stderr, "Missing %s value after option\n", name);
		return INVALID_PARAM;
	}

	*output = strtol(value, NULL, 0);

	if (
		(*output < min_value) ||
		(max_value >= 0 && *output > max_value)
	) {
		if (max_value >= 0)
			fprintf(stderr, "Invalid %s: %d (must be in %d-%d range)\n", name, *output, min_value, max_value);
		else
			fprintf(stderr, "Invalid %s: %d (must be %d or greater)\n", name, *output, min_value);
		return INVALID_PARAM;
	}

	return 2;
}

static int parse_int_one_of(
	int *output,
	const char *name,
	const char *value,
	int value_a,
	int value_b
) {
	if (value == NULL) {
		fprintf(stderr, "Missing %s value after option\n", name);
		return INVALID_PARAM;
	}

	*output = strtol(value, NULL, 0);

	if (*output != value_a && *output != value_b) {
		fprintf(stderr, "Invalid %s: %d (must be %d or %d)\n", name, *output, value_a, value_b);
		return INVALID_PARAM;
	}

	return 2;
}

static int parse_enum(
	int *output,
	const char *name,
	const char *value,
	const char *const *choices,
	int count
) {
	if (value == NULL) {
		fprintf(stderr, "Missing %s value after option\n", name);
		return INVALID_PARAM;
	}
	for (int i = 0; i < count; i++) {
		if (strcmp(value, choices[i]) == 0) {
			*output = i;
			return 2;
		}
	}

	fprintf(
		stderr,
		"Invalid %s: %s\n"
		"Must be one of the following values:\n",
		name,
		value
	);
	for (int i = 0; i < count; i++)
		fprintf(stderr, "    %s\n", choices[i]);
	return INVALID_PARAM;
}

static const char *const general_options_help =
	"General options:\n"
	"    -h                Show this help message and exit\n"
	"    -V                Show version information and exit\n"
	"    -q                Suppress all non-error messages\n"
	"    -t format         Use (or show help for) specified output format\n"
	"                        xa:     [A.] XA-ADPCM, 2336-byte sectors\n"
	"                        xacd:   [A.] XA-ADPCM, 2352-byte sectors\n"
	"                        spu:    [A.] raw SPU-ADPCM mono data\n"
	"                        spui:   [A.] raw SPU-ADPCM interleaved data\n"
	"                        vag:    [A.] .vag SPU-ADPCM mono\n"
	"                        vagi:   [A.] .vag SPU-ADPCM interleaved\n"
	"                        str:    [AV] .str video + XA-ADPCM, 2336-byte sectors\n"
	"                        strcd:  [AV] .str video + XA-ADPCM, 2352-byte sectors\n"
	//"                        strspu: [AV] .str video + SPU-ADPCM, 2048-byte sectors\n"
	"                        strv:   [.V] .str video, 2048-byte sectors\n"
	"                        sbs:    [.V] .sbs video\n"
	"    -R key=value,...  Pass custom options to libswresample (see FFmpeg docs)\n"
	"    -S key=value,...  Pass custom options to libswscale (see FFmpeg docs)\n"
	"\n";

static const char *const format_names[NUM_FORMATS] = {
	"xa",
	"xacd",
	"spu",
	"vag",
	"spui",
	"vagi",
	"str",
	"strcd",
	"strspu",
	"strv",
	"sbs"
};

static void init_default_args(args_t *args) {
	if (
		args->format == FORMAT_XA ||
		args->format == FORMAT_XACD ||
		args->format == FORMAT_STR ||
		args->format == FORMAT_STRCD
	)
		args->audio_frequency = 37800;
	else
		args->audio_frequency = 44100;

	if (args->format == FORMAT_SPU || args->format == FORMAT_VAG)
		args->audio_channels = 1;
	else
		args->audio_channels = 2;

	args->audio_bit_depth = 4;
	args->audio_xa_file = 0;
	args->audio_xa_channel = 0;
	args->audio_interleave = 2048;
	args->audio_loop_point = -1;

	args->video_codec = BS_CODEC_V2;
	args->video_width = 320;
	args->video_height = 240;

	args->str_fps_num = 15;
	args->str_fps_den = 1;
	args->str_cd_speed = 2;
	args->str_video_id = 0x8001;
	args->str_audio_id = 0x0001;

	if (args->format == FORMAT_SPU || args->format == FORMAT_VAG)
		args->alignment = 64; // Default SPU DMA chunk size
	else if (args->format == FORMAT_SBS)
		args->alignment = 8192; // Default for System 573 games
	else
		args->alignment = 2048;
}

static int parse_general_option(args_t *args, char option, const char *param) {
	int parsed;

	switch (option) {
		case '-':
			args->flags |= FLAG_IGNORE_OPTIONS;
			return 1;

		case 'h':
			args->flags |= FLAG_PRINT_HELP;
			return 1;

		case 'V':
			args->flags |= FLAG_PRINT_VERSION;
			return 1;

		case 'q':
			args->flags |= FLAG_QUIET | FLAG_HIDE_PROGRESS;
			return 1;

		case 't':
			parsed = parse_enum(&(args->format), "format", param, format_names, NUM_FORMATS);
			if (parsed > 0)
				init_default_args(args);
			return parsed;

		case 'R':
			if (param == NULL) {
				fprintf(stderr, "Missing libswresample parameter list after option\n");
				return INVALID_PARAM;
			}

			args->swresample_options = param;
			return 2;

		case 'S':
			if (param == NULL) {
				fprintf(stderr, "Missing libswscale parameter list after option\n");
				return INVALID_PARAM;
			}

			args->swscale_options = param;
			return 2;

		default:
			return 0;
	}
}

static const char *const xa_options_help =
	"XA-ADPCM options:\n"
	"    [-f 18900|37800] [-c 1|2] [-b 4|8] [-F 0-255] [-C 0-31]\n"
	"\n"
	"    -f 18900|37800    Use specified sample rate (default 37800)\n"
	"    -c 1|2            Use specified channel count (default 2)\n"
	"    -b 4|8            Use specified bit depth (default 4)\n"
	"    -F 0-255          Set CD-XA file number (for both audio and video, default 0)\n"
	"    -C 0-31           Set CD-XA channel number (for both audio and video, default 0)\n"
	"\n";

static int parse_xa_option(args_t *args, char option, const char *param) {
	switch (option) {
		case 'f':
			return parse_int_one_of(&(args->audio_frequency), "sample rate", param, 18900, 37800);

		case 'c':
			return parse_int_one_of(&(args->audio_channels), "channel count", param, 1, 2);

		case 'b':
			return parse_int_one_of(&(args->audio_bit_depth), "bit depth", param, 4, 8);

		case 'F':
			return parse_int(&(args->audio_xa_file), "file number", param, 0, 255);

		case 'C':
			return parse_int(&(args->audio_xa_channel), "channel number", param, 0, 31);

		default:
			return 0;
	}
}

static const char *const spu_options_help =
	"Mono SPU-ADPCM options:\n"
	"    [-f freq] [-a size] [-l ms | -n | -L] [-D]\n"
	"\n"
	"    -f freq           Use specified sample rate (default 44100)\n"
	"    -a size           Pad audio data excluding header to multiple of given size (default 64)\n"
	"    -l ms             Add loop point at specified timestamp (in milliseconds, overrides any loop point present in input file)\n"
	"    -n                Do not set loop end flag nor add a loop point (even if input file has one)\n"
	"    -L                Set ADPCM loop end flag at end of data but do not add a loop point (even if input file has one)\n"
	"    -D                Do not prepend encoded data with a dummy silent block to reset decoder state\n"
	"\n";

static int parse_spu_option(args_t *args, char option, const char *param) {
	switch (option) {
		case 'f':
			return parse_int(&(args->audio_frequency), "sample rate", param, 1, -1);

		case 'a':
			return parse_int(&(args->alignment), "alignment", param, 1, -1);

		case 'l':
			args->flags |= FLAG_OVERRIDE_LOOP_POINT | FLAG_SPU_ENABLE_LOOP;
			return parse_int(&(args->audio_loop_point), "loop offset", param, 0, -1);

		case 'n':
			args->flags |= FLAG_OVERRIDE_LOOP_POINT;
			args->audio_loop_point = -1;
			return 1;

		case 'L':
			args->flags |= FLAG_OVERRIDE_LOOP_POINT | FLAG_SPU_ENABLE_LOOP;
			args->audio_loop_point = -1;
			return 1;

		case 'D':
			args->flags |= FLAG_SPU_NO_LEADING_DUMMY;
			return 1;

		default:
			return 0;
	}
}

static const char *const spui_options_help =
	"Interleaved SPU-ADPCM options:\n"
	"    [-f freq] [-c channels] [-i size] [-a size] [-l ms | -n] [-L] [-D]\n"
	"\n"
	"    -f freq           Use specified sample rate (default 44100)\n"
	"    -c channels       Use specified channel count (default 2)\n"
	"    -i size           Use specified channel interleave size (default 2048)\n"
	"    -a size           Pad .vag header and each audio chunk to multiples of given size (default 2048)\n"
	"    -l ms             Store specified timestamp in file header as loop point (in milliseconds, overrides any loop point present in input file)\n"
	"    -n                Do not store any loop point in file header (even if input file has one)\n"
	"    -L                Set ADPCM loop end flag at the end of each audio chunk (separately from loop point in file header)\n"
	"    -D                Do not prepend first chunk's data with a dummy silent block to reset decoder state\n"
	"\n";

static int parse_spui_option(args_t *args, char option, const char *param) {
	int parsed;

	switch (option) {
		case 'f':
			return parse_int(&(args->audio_frequency), "sample rate", param, 1, -1);

		case 'c':
			return parse_int(&(args->audio_channels), "channel count", param, 1, -1);

		case 'i':
			parsed = parse_int(&(args->audio_interleave), "interleave", param, 16, -1);

			// Round up to nearest multiple of 16
			args->audio_interleave = (args->audio_interleave + 15) & ~15;
			return parsed;

		case 'a':
			return parse_int(&(args->alignment), "alignment", param, 1, -1);

		case 'l':
			args->flags |= FLAG_OVERRIDE_LOOP_POINT;
			return parse_int(&(args->audio_loop_point), "loop offset", param, 0, -1);

		case 'n':
			args->flags |= FLAG_OVERRIDE_LOOP_POINT;
			args->audio_loop_point = -1;
			return 1;

		case 'L':
			args->flags |= FLAG_SPU_ENABLE_LOOP;
			return 1;

		case 'D':
			args->flags |= FLAG_SPU_NO_LEADING_DUMMY;
			return 1;

		default:
			return 0;
	}
}

static const char *const bs_options_help =
	"Video options:\n"
	"    [-v v2|v3|v3dc] [-s WxH] [-I]\n"
	"\n"
	"    -v codec          Use specified video codec\n"
	"                        v2:   MDEC BS v2 (default)\n"
	"                        v3:   MDEC BS v3\n"
	"                        v3dc: MDEC BS v3, expect decoder to wrap DC coefficients\n"
	"    -s WxH            Rescale input file to fit within specified size (16x16-640x512 in 16-pixel increments, default 320x240)\n"
	"    -I                Force stretching to given size without preserving aspect ratio\n"
	"\n";

const char *const bs_codec_names[NUM_BS_CODECS] = {
	"v2",
	"v3",
	"v3dc"
};

static int parse_bs_option(args_t *args, char option, const char *param) {
	char *next = NULL;

	switch (option) {
		case 'v':
			return parse_enum(&(args->video_codec), "video codec", param, bs_codec_names, NUM_BS_CODECS);

		case 's':
			if (param == NULL) {
				fprintf(stderr, "Missing video size after option\n");
				return INVALID_PARAM;
			}

			args->video_width = strtol(param, &next, 10);

			if (next && *next == 'x') {
				args->video_height = strtol(next + 1, NULL, 10);
			} else {
				fprintf(stderr, "Invalid video size (must be specified as <width>x<height>)\n");
				return INVALID_PARAM;
			}

			if (args->video_width < 16 || args->video_width > 640) {
				fprintf(stderr, "Invalid video width: %d (must be in 16-640 range)\n", args->video_width);
				return INVALID_PARAM;
			}
			if (args->video_height < 16 || args->video_height > 512) {
				fprintf(stderr, "Invalid video height: %d (must be in 16-512 range)\n", args->video_height);
				return INVALID_PARAM;
			}

			// Round up to nearest multiples of 16
			args->video_width = (args->video_width + 15) & ~15;
			args->video_height = (args->video_height + 15) & ~15;
			return 2;

		case 'I':
			args->flags |= FLAG_BS_IGNORE_ASPECT;
			return 1;

		default:
			return 0;
	}
}

static const char *const str_options_help =
	".str container options:\n"
	"    [-r num[/den]] [-x 1|2] [-T id] [-A id] [-X]\n"
	"\n"
	"    -r num[/den]      Set video frame rate to specified integer or fraction (default 15)\n"
	"    -x 1|2            Set CD-ROM speed the file is meant to played at (default 2)\n"
	"    -T id             Tag video sectors with specified .str type ID (default 0x8001)\n"
	"    -A id             Tag SPU-ADPCM sectors with specified .str type ID (default 0x0001)\n"
	"    -X                Place audio sectors after corresponding video sectors rather than ahead of them\n"
	"\n";

static int parse_str_option(args_t *args, char option, const char *param) {
	char *next = NULL;
	int fps;

	switch (option) {
		case 'r':
			if (param == NULL) {
				fprintf(stderr, "Missing frame rate value after option\n");
				return INVALID_PARAM;
			}

			args->str_fps_num = strtol(param, &next, 10);

			if (next && *next == '/')
				args->str_fps_den = strtol(next + 1, NULL, 10);
			else
				args->str_fps_den = 1;

			if (args->str_fps_num <= 0 || args->str_fps_den <= 0) {
				fprintf(stderr, "Invalid frame rate (must be a non-zero integer or fraction)\n");
				return INVALID_PARAM;
			}

			fps = args->str_fps_num / args->str_fps_den;

			if (fps < 1 || fps > 60) {
				fprintf(stderr, "Invalid frame rate: %d/%d (must be in 1-60 range)\n", args->str_fps_num, args->str_fps_den);
				return INVALID_PARAM;
			}
			return 2;

		case 'x':
			return parse_int_one_of(&(args->str_cd_speed), "CD-ROM speed", param, 1, 2);

		case 'T':
			return parse_int(&(args->str_video_id), "video track type ID", param, 0x0000, 0xFFFF);

		case 'A':
			return parse_int(&(args->str_audio_id), "audio track type ID", param, 0x0000, 0xFFFF);

		case 'X':
			args->flags |= FLAG_STR_TRAILING_AUDIO;
			return 1;

		default:
			return 0;
	}
}

static const char *const sbs_options_help =
	".sbs container options:\n"
	"    [-a size]\n"
	"\n"
	"    -a size           Set size of each video frame (default 8192)\n"
	"\n";

static int parse_sbs_option(args_t *args, char option, const char *param) {
	switch (option) {
		case 'a':
			return parse_int(&(args->alignment), "video frame size", param, 256, -1);

		default:
			return 0;
	}
}

static const char *const general_usage =
	"Usage:\n"
	"    psxavenc -t xa|xacd   [xa-options]                              <in> <out.xa>\n"
	"    psxavenc -t spu|vag   [spu-options]                             <in> <out.vag>\n"
	"    psxavenc -t spui|vagi [spui-options]                            <in> <out.vag>\n"
	"    psxavenc -t str|strcd [xa-options]   [bs-options] [str-options] <in> <out.str>\n"
	//"    psxavenc -t strspu    [spui-options] [bs-options] [str-options] <in> <out.str>\n"
	"    psxavenc -t strv                     [bs-options] [str-options] <in> <out.str>\n"
	"    psxavenc -t sbs                      [bs-options] [sbs-options] <in> <out.sbs>\n"
	"\n";

static const struct {
	const char *usage;
	const char *audio_options_help;
	const char *video_options_help;
	const char *container_options_help;
	int (*parse_audio_option)(args_t *, char, const char *);
	int (*parse_video_option)(args_t *, char, const char *);
	int (*parse_container_option)(args_t *, char, const char *);
} format_info[NUM_FORMATS] = {
	{
		.usage = "psxavenc -t xa [xa-options] <in> <out.xa>",
		.audio_options_help = xa_options_help,
		.video_options_help = NULL,
		.container_options_help = NULL,
		.parse_audio_option = parse_xa_option,
		.parse_video_option = NULL,
		.parse_container_option = NULL
	}, {
		.usage = "psxavenc -t xacd [xa-options] <in> <out.xa>",
		.audio_options_help = xa_options_help,
		.video_options_help = NULL,
		.container_options_help = NULL,
		.parse_audio_option = parse_xa_option,
		.parse_video_option = NULL,
		.parse_container_option = NULL
	}, {
		.usage = "psxavenc -t spu [spu-options] <in> <out>",
		.audio_options_help = spu_options_help,
		.video_options_help = NULL,
		.container_options_help = NULL,
		.parse_audio_option = parse_spu_option,
		.parse_video_option = NULL,
		.parse_container_option = NULL
	}, {
		.usage = "psxavenc -t vag [spu-options] <in> <out.vag>",
		.audio_options_help = spu_options_help,
		.video_options_help = NULL,
		.container_options_help = NULL,
		.parse_audio_option = parse_spu_option,
		.parse_video_option = NULL,
		.parse_container_option = NULL
	}, {
		.usage = "psxavenc -t spui [spui-options] <in> <out>",
		.audio_options_help = spui_options_help,
		.video_options_help = NULL,
		.container_options_help = NULL,
		.parse_audio_option = parse_spui_option,
		.parse_video_option = NULL,
		.parse_container_option = NULL
	}, {
		.usage = "psxavenc -t vagi [spui-options] <in> <out.vag>",
		.audio_options_help = spui_options_help,
		.video_options_help = NULL,
		.container_options_help = NULL,
		.parse_audio_option = parse_spui_option,
		.parse_video_option = NULL,
		.parse_container_option = NULL
	}, {
		.usage = "psxavenc -t str [xa-options] [bs-options] [str-options] <in> <out.str>",
		.audio_options_help = xa_options_help,
		.video_options_help = bs_options_help,
		.container_options_help = str_options_help,
		.parse_audio_option = parse_xa_option,
		.parse_video_option = parse_bs_option,
		.parse_container_option = parse_str_option
	}, {
		.usage = "psxavenc -t strcd [xa-options] [bs-options] [str-options] <in> <out.str>",
		.audio_options_help = xa_options_help,
		.video_options_help = bs_options_help,
		.container_options_help = str_options_help,
		.parse_audio_option = parse_xa_option,
		.parse_video_option = parse_bs_option,
		.parse_container_option = parse_str_option
	}, {
		.usage = "psxavenc -t strspu [spui-options] [bs-options] [str-options] <in> <out.str>",
		.audio_options_help = spui_options_help,
		.video_options_help = bs_options_help,
		.container_options_help = str_options_help,
		.parse_audio_option = parse_spui_option,
		.parse_video_option = parse_bs_option,
		.parse_container_option = parse_str_option
	}, {
		.usage = "psxavenc -t strv [bs-options] [str-options] <in> <out.str>",
		.audio_options_help = NULL,
		.video_options_help = bs_options_help,
		.container_options_help = str_options_help,
		.parse_audio_option = NULL,
		.parse_video_option = parse_bs_option,
		.parse_container_option = parse_str_option
	}, {
		.usage = "psxavenc -t sbs [bs-options] [sbs-options] <in> <out.sbs>",
		.audio_options_help = NULL,
		.video_options_help = bs_options_help,
		.container_options_help = sbs_options_help,
		.parse_audio_option = NULL,
		.parse_video_option = parse_bs_option,
		.parse_container_option = parse_sbs_option
	}
};

static int parse_option(args_t *args, char option, const char *param) {
	int parsed = parse_general_option(args, option, param);

	if (parsed == 0 && args->format != FORMAT_INVALID) {
		if (format_info[args->format].parse_audio_option != NULL)
			parsed = format_info[args->format].parse_audio_option(args, option, param);
	}
	if (parsed == 0 && args->format != FORMAT_INVALID) {
		if (format_info[args->format].parse_video_option != NULL)
			parsed = format_info[args->format].parse_video_option(args, option, param);
	}
	if (parsed == 0 && args->format != FORMAT_INVALID) {
		if (format_info[args->format].parse_container_option != NULL)
			parsed = format_info[args->format].parse_container_option(args, option, param);
	}
	if (parsed == 0) {
		if (args->format == FORMAT_INVALID)
			fprintf(
				stderr,
				"Unknown general option: -%c\n"
				"(if this is a format-specific option, it shall be passed after -t)\n",
				option
			);
		else
			fprintf(stderr, "Unknown option for format %s: -%c\n", format_names[args->format], option);
	}

	return parsed;
}

static void print_help(format_t format) {
	if (format == FORMAT_INVALID) {
		printf(
			"%s%s%s%s%s%s%s%s",
			general_usage,
			general_options_help,
			xa_options_help,
			spu_options_help,
			spui_options_help,
			bs_options_help,
			str_options_help,
			sbs_options_help
		);
		return;
	}

	printf(
		"Usage:\n"
		"    %s\n"
		"\n"
		"%s",
		format_info[format].usage,
		general_options_help
	);
	if (format_info[format].audio_options_help != NULL)
		printf("%s", format_info[format].audio_options_help);
	if (format_info[format].video_options_help != NULL)
		printf("%s", format_info[format].video_options_help);
	if (format_info[format].container_options_help != NULL)
		printf("%s", format_info[format].container_options_help);
}

bool parse_args(args_t *args, const char *const *options, int count) {
	int arg_index = 0;

	while (arg_index < count) {
		const char *option = options[arg_index];

		if (option[0] == '-' && option[2] == 0 && !(args->flags & FLAG_IGNORE_OPTIONS)) {
			const char *param;
			if ((arg_index + 1) < count)
				param = options[arg_index + 1];
			else
				param = NULL;

			int parsed = parse_option(args, option[1], param);
			if (parsed <= 0)
				return false;

			arg_index += parsed;
			continue;
		}

		if (args->input_file == NULL) {
			args->input_file = option;
		} else if (args->output_file == NULL) {
			args->output_file = option;
		} else {
			fprintf(stderr, "There should be no arguments after the output file path\n");
			return false;
		}
		arg_index++;
	}

	if (args->flags & FLAG_PRINT_HELP) {
		print_help(args->format);
		return false;
	}
	if (args->flags & FLAG_PRINT_VERSION) {
		printf("psxavenc " VERSION "\n");
		return false;
	}
	if (args->format == FORMAT_INVALID || args->input_file == NULL || args->output_file == NULL) {
		fprintf(
			stderr,
			"%s"
			"For more information about the options supported for a given output format, run:\n"
			"    psxavenc -t <format> -h\n"
			"To view the full list of supported options, run:\n"
			"    psxavenc -h\n",
			general_usage
		);
		return false;
	}

	return true;
}
