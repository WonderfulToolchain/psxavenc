/*
libpsxav: MDEC video + SPU/XA-ADPCM audio library

Copyright (c) 2019, 2020 Adrian "asie" Siekierka
Copyright (c) 2019 Ben "GreaseMonkey" Russell

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

// audio.c

#define PSX_AUDIO_XA_FREQ_SINGLE 18900
#define PSX_AUDIO_XA_FREQ_DOUBLE 37800

typedef enum {
	PSX_AUDIO_XA_FORMAT_XA, // .xa file
	PSX_AUDIO_XA_FORMAT_XACD // 2352-byte sector
} psx_audio_xa_format_t;

typedef struct {
	psx_audio_xa_format_t format;
	bool stereo; // false or true
	int frequency; // 18900 or 37800 Hz
	int bits_per_sample; // 4 or 8
	int file_number; // 00-FF
	int channel_number; // 00-1F
} psx_audio_xa_settings_t;

typedef struct {
	int qerr; // quanitisation error
	uint64_t mse; // mean square error
	int prev1, prev2;
} psx_audio_encoder_channel_state_t;

typedef struct {
	psx_audio_encoder_channel_state_t left;
	psx_audio_encoder_channel_state_t right;
} psx_audio_encoder_state_t;

#define PSX_AUDIO_SPU_LOOP_END 1
#define PSX_AUDIO_SPU_LOOP_REPEAT 3
#define PSX_AUDIO_SPU_LOOP_START 4

uint32_t psx_audio_xa_get_buffer_size(psx_audio_xa_settings_t settings, int sample_count);
uint32_t psx_audio_spu_get_buffer_size(int sample_count);
uint32_t psx_audio_xa_get_buffer_size_per_sector(psx_audio_xa_settings_t settings);
uint32_t psx_audio_spu_get_buffer_size_per_block(void);
uint32_t psx_audio_xa_get_samples_per_sector(psx_audio_xa_settings_t settings);
uint32_t psx_audio_spu_get_samples_per_block(void);
uint32_t psx_audio_xa_get_sector_interleave(psx_audio_xa_settings_t settings);
int psx_audio_xa_encode(psx_audio_xa_settings_t settings, psx_audio_encoder_state_t *state, int16_t* samples, int sample_count, uint8_t *output);
int psx_audio_xa_encode_simple(psx_audio_xa_settings_t settings, int16_t* samples, int sample_count, uint8_t *output);
int psx_audio_spu_encode(psx_audio_encoder_channel_state_t *state, int16_t* samples, int sample_count, int pitch, uint8_t *output);
int psx_audio_spu_encode_simple(int16_t* samples, int sample_count, uint8_t *output, int loop_start);
void psx_audio_xa_encode_finalize(psx_audio_xa_settings_t settings, uint8_t *output, int output_length);
void psx_audio_spu_set_flag_at_sample(uint8_t* spu_data, int sample_pos, int flag);

// cdrom.c

#define PSX_CDROM_SECTOR_SIZE 2352

typedef struct {
	uint8_t minute;
	uint8_t second;
	uint8_t sector;
	uint8_t mode;
} psx_cdrom_sector_header_t;

typedef struct {
	uint8_t file;
	uint8_t channel;
	uint8_t submode;
	uint8_t coding;
} psx_cdrom_sector_xa_subheader_t;

typedef struct {
	uint8_t sync[12];
	psx_cdrom_sector_header_t header;
	uint8_t data[0x920];
} psx_cdrom_sector_mode1_t;

typedef struct {
	uint8_t sync[12];
	psx_cdrom_sector_header_t header;
	psx_cdrom_sector_xa_subheader_t subheader[2];
	uint8_t data[0x918];
} psx_cdrom_sector_mode2_t;

typedef union {
	psx_cdrom_sector_mode1_t mode1;
	psx_cdrom_sector_mode2_t mode2;
} psx_cdrom_sector_t;

_Static_assert(sizeof(psx_cdrom_sector_mode1_t) == PSX_CDROM_SECTOR_SIZE, "Invalid Mode1 sector size");
_Static_assert(sizeof(psx_cdrom_sector_mode2_t) == PSX_CDROM_SECTOR_SIZE, "Invalid Mode2 sector size");

#define PSX_CDROM_SECTOR_XA_CHANNEL_MASK 0x1F

#define PSX_CDROM_SECTOR_XA_SUBMODE_EOR     0x01
#define PSX_CDROM_SECTOR_XA_SUBMODE_VIDEO   0x02
#define PSX_CDROM_SECTOR_XA_SUBMODE_AUDIO   0x04
#define PSX_CDROM_SECTOR_XA_SUBMODE_DATA    0x08
#define PSX_CDROM_SECTOR_XA_SUBMODE_TRIGGER 0x10
#define PSX_CDROM_SECTOR_XA_SUBMODE_FORM2   0x20
#define PSX_CDROM_SECTOR_XA_SUBMODE_RT      0x40
#define PSX_CDROM_SECTOR_XA_SUBMODE_EOF     0x80

#define PSX_CDROM_SECTOR_XA_CODING_MONO         0x00
#define PSX_CDROM_SECTOR_XA_CODING_STEREO       0x01
#define PSX_CDROM_SECTOR_XA_CODING_CHANNEL_MASK 0x03
#define PSX_CDROM_SECTOR_XA_CODING_FREQ_DOUBLE  0x00
#define PSX_CDROM_SECTOR_XA_CODING_FREQ_SINGLE  0x04
#define PSX_CDROM_SECTOR_XA_CODING_FREQ_MASK    0x0C
#define PSX_CDROM_SECTOR_XA_CODING_BITS_4       0x00
#define PSX_CDROM_SECTOR_XA_CODING_BITS_8       0x10
#define PSX_CDROM_SECTOR_XA_CODING_BITS_MASK    0x30
#define PSX_CDROM_SECTOR_XA_CODING_EMPHASIS     0x40

typedef enum {
	PSX_CDROM_SECTOR_TYPE_MODE1,
	PSX_CDROM_SECTOR_TYPE_MODE2_FORM1,
	PSX_CDROM_SECTOR_TYPE_MODE2_FORM2
} psx_cdrom_sector_type_t;

void psx_cdrom_init_sector(psx_cdrom_sector_t *sector, int lba, psx_cdrom_sector_type_t type);
void psx_cdrom_calculate_checksums(psx_cdrom_sector_t *sector, psx_cdrom_sector_type_t type);
