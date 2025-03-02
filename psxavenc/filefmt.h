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

#include <stdio.h>
#include "args.h"
#include "decoding.h"

void encode_file_xa(const args_t *args, decoder_t *decoder, FILE *output);
void encode_file_spu(const args_t *args, decoder_t *decoder, FILE *output);
void encode_file_spui(const args_t *args, decoder_t *decoder, FILE *output);
void encode_file_str(const args_t *args, decoder_t *decoder, FILE *output);
void encode_file_strspu(const args_t *args, decoder_t *decoder, FILE *output);
void encode_file_sbs(const args_t *args, decoder_t *decoder, FILE *output);
