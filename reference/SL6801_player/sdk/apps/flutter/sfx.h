/* Flutter's sound effects (see fw_tone_t). SFX_ALL lists them for sdk/test/sfx_preview.c. */
#pragma once
#include "sdk.h"

static const fw_tone_t sfx_start[] = {{660, 660, 60, FW_WAVE_TRIANGLE, 140}, {990, 990, 110, FW_WAVE_TRIANGLE, 140}};
static const fw_tone_t sfx_flap[] = {{520, 980, 70, FW_WAVE_TRIANGLE, 150}};
static const fw_tone_t sfx_score[] = {{1319, 1319, 60, FW_WAVE_SQUARE, 80}, {1760, 1760, 150, FW_WAVE_SQUARE, 80}};
/* The crash plays both at once. */
static const fw_tone_t sfx_crash_noise[] = {{4000, 300, 220, FW_WAVE_NOISE, 200}};
static const fw_tone_t sfx_crash_bonk[] = {{320, 70, 380, FW_WAVE_SQUARE, 130}};

#define SFX_ALL X(sfx_start) X(sfx_flap) X(sfx_score) X(sfx_crash_noise) X(sfx_crash_bonk)
