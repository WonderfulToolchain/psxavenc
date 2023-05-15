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

const char *format_names[NUM_FORMATS] = {
	"xa", "xacd",
	"spu", "spui",
	"vag", "vagi",
	"str2", "str2cd",
	"sbs2"
};

void print_help(void) {
	fprintf(stderr,
		"Usage:\n"
		"    psxavenc -t <xa|xacd>     [-f 18900|37800] [-b 4|8] [-c 1|2] [-F 0-255] [-C 0-31] <in> <out.xa>\n"
		"    psxavenc -t <str2|str2cd> [-f 18900|37800] [-b 4|8] [-c 1|2] [-F 0-255] [-C 0-31] [-s WxH] [-I] [-r num/den] [-x 1|2] <in> <out.str>\n"
		"    psxavenc -t sbs2          [-s WxH] [-I] [-r num/den] [-a size] <in> <out.str>\n"
		"    psxavenc -t <spu|vag>     [-f freq] [-L] <in> <out.vag>\n"
		"    psxavenc -t <spui|vagi>   [-f freq] [-c 1-24] [-L] [-i size] [-a size] <in> <out.vag>\n"
		"\nTool options:\n"
		"    -h               Show this help message and exit\n"
		"    -q               Suppress all non-error messages\n"
		"\nOutput options:\n"
		"    -t format        Use specified output type:\n"
		"                       xa     [A.] .xa, 2336-byte sectors\n"
		"                       xacd   [A.] .xa, 2352-byte sectors\n"
		"                       spu    [A.] raw SPU-ADPCM mono data\n"
		"                       spui   [A.] raw SPU-ADPCM interleaved data\n"
		"                       vag    [A.] .vag SPU-ADPCM mono\n"
		"                       vagi   [A.] .vag SPU-ADPCM interleaved\n"
		"                       str2   [AV] v2 .str video, 2336-byte sectors\n"
		"                       str2cd [AV] v2 .str video, 2352-byte sectors\n"
		"                       sbs2   [.V] v2 .sbs video, 2048-byte sectors\n"
		"    -F num           Set the XA file number for xa/str2 (0-255)\n"
		"    -C num           Set the XA channel number for xa/str2 (0-31)\n"
		"\nAudio options:\n"
		"    -f freq          Use specified sample rate (must be 18900 or 37800 for xa/str2)\n"
		"    -b bitdepth      Use specified bit depth for xa/str2 (4 or 8)\n"
		"    -c channels      Use specified channel count (1-2 for xa/str2, any for spui/vagi)\n"
		"    -L               Add a loop marker at the end of SPU-ADPCM data\n"
		"    -R key=value,... Pass custom options to libswresample (see ffmpeg docs)\n"
		"\nSPU interleaving options (spui/vagi format):\n"
		"    -i size          Use specified interleave\n"
		"    -a size          Pad header and each interleaved chunk to specified size\n"
		"\nVideo options (str2/str2cd/sbs2 format):\n"
		"    -s WxH           Rescale input file to fit within specified size (default 320x240)\n"
		"    -I               Force stretching to given size without preserving aspect ratio\n"
		"    -S key=value,... Pass custom options to libswscale (see ffmpeg docs)\n"
		"    -r num/den       Set frame rate to specified integer or fraction (default 15)\n"
		"    -x speed         Set the CD-ROM speed the file is meant to played at (1-2)\n"
		"    -a size          Set the size of each frame for sbs2\n"
	);
}

int parse_args(settings_t* settings, int argc, char** argv) {
	int c, i;
	char *next;
	while ((c = getopt(argc, argv, "?hqt:F:C:f:b:c:LR:i:a:s:IS:r:x:")) != -1) {
		switch (c) {
			case '?':
			case 'h': {
				print_help();
				return -1;
			} break;
			case 'q': {
				settings->quiet = true;
				settings->show_progress = false;
			} break;
			case 't': {
				settings->format = -1;
				for (i = 0; i < NUM_FORMATS; i++) {
					if (!strcmp(optarg, format_names[i])) {
						settings->format = i;
						break;
					}
				}
				if (settings->format < 0) {
					fprintf(stderr, "Invalid format: %s\n", optarg);
					return -1;
				}
			} break;
			case 'F': {
				settings->file_number = strtol(optarg, NULL, 0);
				if (settings->file_number < 0 || settings->file_number > 255) {
					fprintf(stderr, "Invalid file number: %d\n", settings->file_number);
					return -1;
				}
			} break;
			case 'C': {
				settings->channel_number = strtol(optarg, NULL, 0);
				if (settings->channel_number < 0 || settings->channel_number > 31) {
					fprintf(stderr, "Invalid channel number: %d\n", settings->channel_number);
					return -1;
				}
			} break;
			case 'f': {
				settings->frequency = strtol(optarg, NULL, 0);
			} break;
			case 'b': {
				settings->bits_per_sample = strtol(optarg, NULL, 0);
				if (settings->bits_per_sample != 4 && settings->bits_per_sample != 8) {
					fprintf(stderr, "Invalid bit depth: %d\n", settings->frequency);
					return -1;
				}
			} break;
			case 'c': {
				settings->channels = strtol(optarg, NULL, 0);
				if (settings->channels < 1 || settings->channels > 24) {
					fprintf(stderr, "Invalid channel count: %d\n", settings->channels);
					return -1;
				}
			} break;
			case 'L': {
				settings->loop = true;
			} break;
			case 'R': {
				settings->swresample_options = optarg;
			} break;
			case 'i': {
				settings->interleave = (strtol(optarg, NULL, 0) + 15) & ~15;
				if (settings->interleave < 16) {
					fprintf(stderr, "Invalid interleave: %d\n", settings->interleave);
					return -1;
				}
			} break;
			case 'a': {
				settings->alignment = strtol(optarg, NULL, 0);
				if (settings->alignment < 1) {
					fprintf(stderr, "Invalid alignment: %d\n", settings->alignment);
					return -1;
				}
			} break;
			case 's': {
				settings->video_width = (strtol(optarg, &next, 0) + 15) & ~15;
				if (*next != 'x') {
					fprintf(stderr, "Invalid video size (must be specified as <width>x<height>)\n");
					return -1;
				}
				settings->video_height = (strtol(next + 1, NULL, 0) + 15) & ~15;

				if (settings->video_width < 16 || settings->video_width > 320) {
					fprintf(stderr, "Invalid video width: %d\n", settings->video_width);
					return -1;
				}
				if (settings->video_height < 16 || settings->video_height > 240) {
					fprintf(stderr, "Invalid video height: %d\n", settings->video_height);
					return -1;
				}
			} break;
			case 'I': {
				settings->ignore_aspect_ratio = true;
			} break;
			case 'S': {
				settings->swscale_options = optarg;
			} break;
			case 'r': {
				settings->video_fps_num = strtol(optarg, &next, 0);
				if (*next == '/') {
					settings->video_fps_den = strtol(next + 1, NULL, 0);
				} else {
					settings->video_fps_den = 1;
				}

				if (!settings->video_fps_den) {
					fprintf(stderr, "Invalid frame rate denominator\n");
					return -1;
				}
				i = settings->video_fps_num / settings->video_fps_den;
				if (i < 1 || i > 30) {
					fprintf(stderr, "Invalid frame rate: %d/%d\n", settings->video_fps_num, settings->video_fps_den);
					return -1;
				}
			} break;
			case 'x': {
				settings->cd_speed = strtol(optarg, NULL, 0);
				if (settings->cd_speed < 1 || settings->cd_speed > 2) {
					fprintf(stderr, "Invalid CD-ROM speed: %d\n", settings->cd_speed);
					return -1;
				}
			} break;
		}
	}

	// Validate settings
	switch (settings->format) {
		case FORMAT_XA:
		case FORMAT_XACD:
		case FORMAT_STR2:
		case FORMAT_STR2CD:
			if (settings->frequency != PSX_AUDIO_XA_FREQ_SINGLE && settings->frequency != PSX_AUDIO_XA_FREQ_DOUBLE) {
				fprintf(
					stderr, "Invalid XA-ADPCM frequency: %d Hz (must be %d or %d Hz)\n", settings->frequency,
					PSX_AUDIO_XA_FREQ_SINGLE, PSX_AUDIO_XA_FREQ_DOUBLE
				);
				return -1;
			}
			if (settings->channels > 2) {
				fprintf(stderr, "Invalid XA-ADPCM channel count: %d (must be 1 or 2)\n", settings->channels);
				return -1;
			}
			if (settings->loop) {
				fprintf(stderr, "XA-ADPCM does not support loop markers\n");
				return -1;
			}
			break;
		case FORMAT_SPU:
		case FORMAT_VAG:
			if (settings->bits_per_sample != 4) {
				fprintf(stderr, "Invalid SPU-ADPCM bit depth: %d (must be 4)\n", settings->bits_per_sample);
				return -1;
			}
			if (settings->channels != 1) {
				fprintf(stderr, "Invalid SPU-ADPCM channel count: %d (must be 1)\n", settings->channels);
				return -1;
			}
			if (settings->interleave) {
				fprintf(stderr, "Interleave cannot be specified for mono SPU-ADPCM\n");
				return -1;
			}
			break;
		case FORMAT_SPUI:
		case FORMAT_VAGI:
			if (settings->bits_per_sample != 4) {
				fprintf(stderr, "Invalid SPU-ADPCM bit depth: %d (must be 4)\n", settings->bits_per_sample);
				return -1;
			}
			if (!settings->interleave) {
				fprintf(stderr, "Interleave must be specified for interleaved SPU-ADPCM\n");
				return -1;
			}
			break;
		case FORMAT_SBS2:
			if (!settings->alignment) {
				fprintf(stderr, "Alignment (frame size) must be specified\n");
				return -1;
			}
			if (settings->alignment < 256) {
				fprintf(stderr, "Invalid frame size: %d (must be at least 256)\n", settings->alignment);
				return -1;
			}
			break;
		default:
			fprintf(stderr, "Output format must be specified\n");
			return -1;
	}

	return optind;
}

int main(int argc, char **argv) {
	settings_t settings;
	int arg_offset;
	FILE* output;

	memset(&settings,0,sizeof(settings_t));

	settings.quiet = false;
	settings.show_progress = isatty(fileno(stderr));

	settings.format = -1;
	settings.file_number = 0;
	settings.channel_number = 0;
	settings.cd_speed = 2;
	settings.channels = 1;
	settings.frequency = PSX_AUDIO_XA_FREQ_DOUBLE;
	settings.bits_per_sample = 4;
	settings.interleave = 0;
	settings.alignment = 2048;
	settings.loop = false;

	// NOTE: ffmpeg/ffplay's .str demuxer has the frame rate hardcoded to 15fps
	// so if you're messing around with this make sure you test generated files
	// with another player and/or in an emulator.
	settings.video_width = 320;
	settings.video_height = 240;
	settings.video_fps_num = 15;
	settings.video_fps_den = 1;
	settings.ignore_aspect_ratio = false;

	settings.swresample_options = NULL;
	settings.swscale_options = NULL;

	settings.audio_samples = NULL;
	settings.audio_sample_count = 0;
	settings.video_frames = NULL;
	settings.video_frame_count = 0;

	for(int i = 0; i < 6; i++) {
		settings.state_vid.dct_block_lists[i] = NULL;
	}

	if (argc < 2) {
		print_help();
		return 1;
	}

	arg_offset = parse_args(&settings, argc, argv);
	if (arg_offset < 0) {
		return 1;
	} else if (argc < arg_offset + 2) {
		print_help();
		return 1;
	}

	bool has_audio = (settings.format != FORMAT_SBS2);
	bool has_video = (settings.format == FORMAT_STR2) ||
		(settings.format == FORMAT_STR2CD) || (settings.format == FORMAT_SBS2);

	bool did_open_data = open_av_data(argv[arg_offset + 0], &settings,
		has_audio, has_video, !has_video, has_video);
	if (!did_open_data) {
		fprintf(stderr, "Could not open input file!\n");
		return 1;
	}

	output = fopen(argv[arg_offset + 1], "wb");
	if (output == NULL) {
		fprintf(stderr, "Could not open output file!\n");
		return 1;
	}

	settings.start_time = time(NULL);
	settings.last_progress_update = 0;

	switch (settings.format) {
		case FORMAT_XA:
		case FORMAT_XACD:
			if (!settings.quiet) {
				fprintf(stderr, "Audio format: XA-ADPCM, %d Hz %d-bit %s, F=%d C=%d\n",
					settings.frequency, settings.bits_per_sample,
					(settings.channels == 2) ? "stereo" : "mono",
					settings.file_number, settings.channel_number
				);
			}

			encode_file_xa(&settings, output);
			break;
		case FORMAT_SPU:
		case FORMAT_VAG:
			if (!settings.quiet) {
				fprintf(stderr, "Audio format: SPU-ADPCM, %d Hz mono\n",
					settings.frequency
				);
			}

			encode_file_spu(&settings, output);
			break;
		case FORMAT_SPUI:
		case FORMAT_VAGI:
			if (!settings.quiet) {
				fprintf(stderr, "Audio format: SPU-ADPCM, %d Hz %d channels, interleave=%d\n",
					settings.frequency, settings.channels, settings.interleave
				);
			}

			encode_file_spu_interleaved(&settings, output);
			break;
		case FORMAT_STR2:
		case FORMAT_STR2CD:
			if (!settings.quiet) {
				if (settings.decoder_state_av.audio_stream) {
					fprintf(stderr, "Audio format: XA-ADPCM, %d Hz %d-bit %s, F=%d C=%d\n",
						settings.frequency, settings.bits_per_sample,
						(settings.channels == 2) ? "stereo" : "mono",
						settings.file_number, settings.channel_number
					);
				}
				fprintf(stderr, "Video format: BS v2, %dx%d, %.2f fps\n",
					settings.video_width, settings.video_height,
					(double)settings.video_fps_num / (double)settings.video_fps_den
				);
			}

			encode_file_str(&settings, output);
			break;
		case FORMAT_SBS2:
			if (!settings.quiet) {
				fprintf(stderr, "Video format: BS v2, %dx%d, %.2f fps\n",
					settings.video_width, settings.video_height,
					(double)settings.video_fps_num / (double)settings.video_fps_den
				);
			}

			encode_file_sbs(&settings, output);
			break;
	}

	if (settings.show_progress) {
		fprintf(stderr, "\nDone.\n");
	}
	fclose(output);
	close_av_data(&settings);
	return 0;
}
