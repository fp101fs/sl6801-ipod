/*
 * Renders an app's sound effects (its sfx.h) to a WAV file with the same mixer as sdk/src/sound.c,
 * each effect followed by a short pause. Build and run on the PC, e.g. with MSYS2 UCRT64 gcc:
 *   gcc -I sdk/include -I sdk/src -I sdk/apps/flutter sdk/test/sfx_preview.c -o sfx_preview.exe
 *   ./sfx_preview.exe flutter_sfx.wav
 * The firmware's volume curve is not applied.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk.h"

/* The ROM routine the mixer calls, working from the end like the ROM's so it can run in place. */
#undef rom_pcm_mono_to_stereo
static void rom_pcm_mono_to_stereo(int16_t *dst, const int16_t *src, uint32_t n)
{
	while (n--)
		dst[2 * n] = dst[2 * n + 1] = src[n];
}

/* Only sound.c's mixer is used here, not its device, task or heap calls. */
#include "sound.c"

bool fw_dev_busy(const char *name)
{
	(void)name;
	return false;
}

/* The worker is a plain struct here, already running with its memory. */
fw_worker_t *fw_worker_start(void (*entry)(fw_worker_t *w), uint32_t mem_size, const char *name, int priority,
			     uint32_t stack_words)
{
	(void)entry, (void)mem_size, (void)name, (void)priority, (void)stack_words;
	return NULL;
}

void fw_worker_release(fw_worker_t *w)
{
	(void)w;
}

void *fw_worker_mem(const fw_worker_t *w)
{
	return w ? w->mem : NULL;
}

#include "sfx.h"

static FILE *out;
static uint32_t frames;

static void put_period(const int16_t *buf)
{
	fwrite(buf, sizeof(int16_t), PERIOD * 2, out);
	frames += PERIOD;
}

static bool busy(sound_t *s)
{
	for (int k = 0; k < VOICES; k++)
		if (s->voice[k].left)
			return true;
	return false;
}

static void play(fw_worker_t *w, const char *name, const fw_tone_t *tones, uint8_t count)
{
	sound_t *s = w->mem;
	printf("%6.2f s  %s\n", frames / (double)RATE, name);
	fw_sound_play(w, tones, count);
	do {
		render(s);
		put_period(s->buf);
	} while (busy(s));
	for (int i = 0; i < RATE / 3 / PERIOD; i++) {
		render(s);
		put_period(s->buf);
	}
}

static void put_u32(uint32_t v) { fwrite(&v, 4, 1, out); }
static void put_u16(uint16_t v) { fwrite(&v, 2, 1, out); }

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: sfx_preview out.wav\n");
		return 1;
	}
	out = fopen(argv[1], "wb");
	if (!out)
		return 1;
	fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, out);
	put_u32(16);
	put_u16(1);
	put_u16(2);
	put_u32(RATE);
	put_u32(RATE * 4);
	put_u16(4);
	put_u16(16);
	fwrite("data\0\0\0\0", 1, 8, out);

	sound_t *s = calloc(1, sizeof(*s));
	s->lfsr = 0x2545F491u;
	fw_worker_t w = {.state = FW_WORKER_RUNNING, .mem = s};
#define X(name) play(&w, #name, FW_SFX(name));
	SFX_ALL
#undef X

	uint32_t bytes = frames * 4;
	fseek(out, 4, SEEK_SET);
	put_u32(36 + bytes);
	fseek(out, 40, SEEK_SET);
	put_u32(bytes);
	fclose(out);
	printf("wrote %s (%.2f s)\n", argv[1], frames / (double)RATE);
	return 0;
}
