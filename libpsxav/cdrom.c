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

#include <stdint.h>
#include <string.h>
#include "libpsxav.h"

#define EDC_CRC32_POLYNOMIAL 0xD8018001

static uint32_t edc_crc32(uint8_t *data, int length) {
	uint32_t edc = 0;

	for (int i = 0; i < length; i++) {
		edc ^= 0xFF & (uint32_t)data[i];

		for (int j = 0; j < 8; j++)
			edc = (edc >> 1) ^ (EDC_CRC32_POLYNOMIAL * (edc & 0x1));
	}

	return edc;
}

#define TO_BCD(x) ((x) + ((x) / 10) * 6)

void psx_cdrom_init_sector(psx_cdrom_sector_t *sector, int lba, psx_cdrom_sector_type_t type) {
	// Sync sequence
	memset(sector->mode1.sync + 1, 0xff, 10);
	sector->mode1.sync[0x0] = 0x00;
	sector->mode1.sync[0xb] = 0x00;

	// Timecode
	lba += 150;
	sector->mode1.header.minute = TO_BCD(lba / 4500);
	sector->mode1.header.second = TO_BCD((lba / 75) % 60);
	sector->mode1.header.sector = TO_BCD(lba % 75);

	// Mode
	if (type == PSX_CDROM_SECTOR_TYPE_MODE1) {
		sector->mode1.header.mode = 0x01;
	} else {
		sector->mode2.header.mode = 0x02;

		memset(sector->mode2.subheader, 0, sizeof(psx_cdrom_sector_xa_subheader_t));
		sector->mode2.subheader[0].submode = PSX_CDROM_SECTOR_XA_SUBMODE_DATA;

		if (type == PSX_CDROM_SECTOR_TYPE_MODE2_FORM2)
			sector->mode2.subheader[0].submode |= PSX_CDROM_SECTOR_XA_SUBMODE_FORM2;

		memcpy(sector->mode2.subheader + 1, sector->mode2.subheader, sizeof(psx_cdrom_sector_xa_subheader_t));
	}
}

void psx_cdrom_calculate_checksums(psx_cdrom_sector_t *sector, psx_cdrom_sector_type_t type) {
	uint8_t *data = (uint8_t *)sector;
	uint32_t edc;

	switch (type) {
		case PSX_CDROM_SECTOR_TYPE_MODE1:
			edc = edc_crc32(data, 0x810);

			data[0x810] = (uint8_t)(edc);
			data[0x811] = (uint8_t)(edc >> 8);
			data[0x812] = (uint8_t)(edc >> 16);
			data[0x813] = (uint8_t)(edc >> 24);
			memset(sector + 0x814, 0, 8);
			// TODO: ECC
			break;

		case PSX_CDROM_SECTOR_TYPE_MODE2_FORM1:
			edc = edc_crc32(data + 0x10, 0x808);

			data[0x818] = (uint8_t)(edc);
			data[0x819] = (uint8_t)(edc >> 8);
			data[0x81A] = (uint8_t)(edc >> 16);
			data[0x81B] = (uint8_t)(edc >> 24);
			// TODO: ECC
			break;

		case PSX_CDROM_SECTOR_TYPE_MODE2_FORM2:
			edc = edc_crc32(data + 0x10, 0x91C);

			data[0x92C] = (uint8_t)(edc);
			data[0x92D] = (uint8_t)(edc >> 8);
			data[0x92E] = (uint8_t)(edc >> 16);
			data[0x92F] = (uint8_t)(edc >> 24);
			break;
	}
}
