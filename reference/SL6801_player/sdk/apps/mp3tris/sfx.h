/* Mp3tris's sound effects (see fw_tone_t). SFX_ALL lists them for sdk/test/sfx_preview.c. */
#pragma once
#include "sdk.h"

static const fw_tone_t sfx_start[] = {{523, 523, 70, FW_WAVE_TRIANGLE, 140}, {784, 784, 120, FW_WAVE_TRIANGLE, 140}};
static const fw_tone_t sfx_move[] = {{1500, 1500, 14, FW_WAVE_SQUARE, 45}};
static const fw_tone_t sfx_rotate[] = {{700, 1100, 35, FW_WAVE_TRIANGLE, 130}};
static const fw_tone_t sfx_land[] = {{900, 200, 70, FW_WAVE_NOISE, 110}};
static const fw_tone_t sfx_clear[] = {
	{784, 784, 60, FW_WAVE_SQUARE, 90}, {988, 988, 60, FW_WAVE_SQUARE, 90}, {1175, 1175, 140, FW_WAVE_SQUARE, 90}};
static const fw_tone_t sfx_clear4[] = {{523, 523, 60, FW_WAVE_SQUARE, 100}, {659, 659, 60, FW_WAVE_SQUARE, 100},
				       {784, 784, 60, FW_WAVE_SQUARE, 100}, {1047, 1047, 60, FW_WAVE_SQUARE, 100},
				       {784, 784, 60, FW_WAVE_SQUARE, 100}, {1047, 1047, 260, FW_WAVE_SQUARE, 100}};
static const fw_tone_t sfx_level[] = {{880, 1760, 200, FW_WAVE_TRIANGLE, 130}};
static const fw_tone_t sfx_over[] = {{392, 392, 170, FW_WAVE_SQUARE, 100}, {330, 330, 170, FW_WAVE_SQUARE, 100},
				     {262, 262, 170, FW_WAVE_SQUARE, 100}, {196, 98, 500, FW_WAVE_SQUARE, 100}};

#define SFX_ALL X(sfx_start) X(sfx_move) X(sfx_rotate) X(sfx_land) X(sfx_clear) X(sfx_clear4) X(sfx_level) X(sfx_over)
