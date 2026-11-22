/* Helpers shared by all apps (sdk/src). */
#pragma once
#include "fw.h"

/* Implemented in sdk/src/libc.c (no C library is linked). */
void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);

/* ---------------------------------------------------------------- APP menu entries
 * FW_MENU_ITEM(symbol, text, open) adds a row to the firmware's "APP" menu; sdk/src/menu.c
 * appends all rows (hook_home_menu_create, patched over the call at 0xD04C04). The entries are
 * collected by the linker script, so several apps can be linked into one image. */
typedef struct {
	const char *symbol; /* UTF-8 of a FontAwesome glyph present in FW_FONT_ICONS */
	const char *text;
	void (*open)(lv_obj_t *return_focus);
} fw_menu_item_t;

#define FW_MENU_ITEM(symbol, text, open)                                                            \
	static const fw_menu_item_t fw_menu_item_##open __attribute__((used, section(".fw_menu"))) = { \
		symbol, text, open}

#define FW_SYMBOL_PLAY "\xEF\x81\x8B"  /* U+F04B */
#define FW_SYMBOL_OK "\xEF\x80\x8C"    /* U+F00C */
#define FW_SYMBOL_AUDIO "\xEF\x80\x81" /* U+F001 */
#define FW_SYMBOL_BARS "\xEF\x83\x89"  /* U+F0C9 */
#define FW_SYMBOL_SAVE "\xEF\x83\x87"  /* U+F0C7 */

/* ---------------------------------------------------------------- the two cores
 * The SoC has two Cortex-M4F cores. CPU0 runs FreeRTOS with everything that uses it: the firmware
 * heap, tasks, semaphores, FatFs and the device drivers. CPU1 runs the UI (LVGL, the firmware's
 * pages) and so all app UI code: menu rows, overlay key and delete callbacks, LVGL timers. Kernel
 * calls from CPU1 race with CPU0 and corrupt the heap or the scheduler, which freezes the player
 * until the watchdog resets it, more often the busier CPU0 is (e.g. playing music). So UI code
 * must not call fw_os_*, fw_task_*, fw_sem_*, rom_task_states, the ROM's critical sections,
 * f_* or fw_dev_* (or fw_sd_ioctl). It may use LVGL and lv_mem_*, ROM helpers that don't touch the
 * kernel (memcpy, qsort, rom_gpio_get, rom_timer_read64, math), registers and plain memory.
 * FW_CORE_ID is 0 on CPU0 and 1 on CPU1. */
#define FW_CORE_ID (*(volatile const uint32_t *)0x4000000Cu & 1)

/* ---------------------------------------------------------------- workers (sdk/src/service.c)
 * A worker gives the UI memory and optionally a task on CPU0. fw_worker_start claims a slot and
 * returns at once (NULL if all FW_WORKERS are in use or the service isn't running). Within a few
 * ms the SDK's service task on CPU0 allocates mem_size zeroed bytes on the firmware heap, starts
 * entry(w) in a new task if entry is set, and moves the state to RUNNING (or FAILED when the heap
 * is short). The task's state becomes DONE once entry returns. The UI polls the state from a timer
 * and shares data with the task through w->mem with plain reads and writes; tasks poll for
 * requests (fw_os_sleep), since the UI can't give semaphores. fw_worker_release sets w->stop and
 * gives the slot back: the service joins the task and frees the memory later, so the UI must not
 * touch w or w->mem after releasing it. Workers are started and released from the UI only. */
#define FW_WORKERS 8
enum { FW_WORKER_FREE, FW_WORKER_STARTING, FW_WORKER_RUNNING, FW_WORKER_DONE, FW_WORKER_FAILED };

typedef struct fw_worker fw_worker_t;
struct fw_worker {
	volatile uint8_t state; /* FW_WORKER_* */
	volatile bool stop;     /* set by fw_worker_release; entry should return soon after */
	void *volatile mem;     /* valid in RUNNING and DONE */
	/* the SDK's */
	void (*entry)(fw_worker_t *w);
	uint32_t mem_size, stack_words;
	uint8_t priority;
	const char *name;
	volatile bool released, exited;
	void *task;
};

fw_worker_t *fw_worker_start(void (*entry)(fw_worker_t *w), uint32_t mem_size, const char *name, int priority,
			     uint32_t stack_words);
void fw_worker_release(fw_worker_t *w);
/* Shared pointer slots for modules that keep state across pages and cores (app code has no
 * writable globals). NULL before the service has started. Slot 0: album art. */
#define FW_MODULE_SLOTS 4
#define FW_MODULE_ALBUM_ART 0
void *volatile *fw_module_slot(unsigned i);

/* w->mem once the worker is RUNNING or DONE, else NULL (also for a NULL w). */
void *fw_worker_mem(const fw_worker_t *w);
/* True if the worker could not start (also for a NULL w). */
bool fw_worker_failed(const fw_worker_t *w);

/* ---------------------------------------------------------------- black box (sdk/src/blackbox.c)
 * Writes 0:/crashlog.txt when code on either core faults or the UI loop stops for 3 s, before the
 * watchdog resets the player (RAM does not survive that reset), and, while status_file is set,
 * rewrites 0:/bbstatus.txt every second. It runs in the service task; fw_blackbox() returns its
 * state, or NULL when it isn't running. */
#define BB_STACK_WORDS 32
#define BB_NAME_LEN 12
#define BB_MAX_TASKS 40
enum { BB_REPORT_NONE = -1, BB_REPORT_OK = 0, BB_REPORT_NO_CARD = 100, BB_REPORT_NO_MEMORY = 101 };

typedef struct {
	uint32_t orig_fault;          /* the firmware's fault handler that bb_fault_entry replaces */
	uint32_t repatched;           /* times the fault vectors had been restored by something else */
	volatile uint32_t view_beat;  /* hardware timer at the UI loop's last pass */
	volatile bool status_file;    /* set by the UI to get 0:/bbstatus.txt */
	uint32_t status_writes;
	volatile int32_t status_result; /* last 0:/bbstatus.txt write, like report */
	volatile int32_t report;        /* BB_REPORT_*, or a FatFs FR_* error */
	void *work;                     /* buffers for writing the files */
	uint32_t prev_total, prev_count; /* run times at the last write, for CPU use */
	void *prev_handle[BB_MAX_TASKS];
	uint32_t prev_run[BB_MAX_TASKS];
	volatile uint32_t faulted;
	uint32_t fault_core, fault_time, exc_return, sp;
	uint32_t frame[8]; /* r0-r3, r12, lr, pc, xPSR */
	uint32_t cfsr, hfsr, mmfar, bfar;
	char task[BB_NAME_LEN]; /* CPU0's current task */
	uint32_t stack[BB_STACK_WORDS]; /* the words above the stacked frame */
} fw_blackbox_t;

fw_blackbox_t *fw_blackbox(void);

/* ---------------------------------------------------------------- full-screen overlays
 * A full-screen object on the top layer that takes all key input until it is deleted.
 * on_delete runs when the root object is deleted for any reason (fw_overlay_close or the firmware
 * tearing down the UI), which is the place to stop timers and free app state.
 * The root has both flags below set, so the app gets these long presses instead of the firmware;
 * clear a flag on the root to restore the firmware's action.
 *   FW_OBJ_FLAG_CENTER_LONG: holding Center sends FW_KEY_CENTER_LONG instead of powering off.
 *   FW_OBJ_FLAG_BACK_LONG: holding Back sends FW_KEY_BACK_LONG instead of opening the volume
 *   control. */
#define FW_OBJ_FLAG_CENTER_LONG LV_OBJ_FLAG_USER_1
#define FW_OBJ_FLAG_BACK_LONG LV_OBJ_FLAG_USER_2
typedef struct {
	lv_obj_t *root;
	lv_group_t *group;
	lv_obj_t *return_focus;
	void (*on_delete)(void *ctx);
	void *ctx;
} fw_overlay_t;

fw_overlay_t *fw_overlay_open(lv_obj_t *return_focus, lv_color_t bg, void (*on_delete)(void *ctx), void *ctx);
/* Deletes the overlay (running on_delete) and gives focus back. Do not touch app state afterwards. */
void fw_overlay_close(fw_overlay_t *ov);

/* ---------------------------------------------------------------- sound effects
 * A 44.1 kHz stream on /dev/audio0 that plays synthesized tones, fed by a worker task the way the
 * video player feeds its audio. fw_sound_open returns NULL only when no worker is free; while the
 * device is in use (e.g. by the music player), or until the worker has started, sounds are
 * dropped. The functions accept NULL. A sound starts within about 50 ms of fw_sound_play. */
enum { FW_WAVE_SQUARE, FW_WAVE_TRIANGLE, FW_WAVE_NOISE };

/* One tone: frequency slides linearly from f0 to f1 Hz (noise: the rate it changes at), volume
 * 0-255 decays to 0 over the tone's length. vol 0 gives a rest. */
typedef struct {
	uint16_t f0, f1;
	uint16_t ms;
	uint8_t wave, vol;
} fw_tone_t;

typedef fw_worker_t fw_sound_t;

fw_sound_t *fw_sound_open(void);
/* Plays tones[0..count-1] one after another; up to 4 sounds play at once. The tones must stay
 * valid while they play (static const tables). */
void fw_sound_play(fw_sound_t *s, const fw_tone_t *tones, uint8_t count);
/* Stops the sound task and closes the device. */
void fw_sound_close(fw_sound_t *s);
#define FW_SFX(tones) (tones), (uint8_t)(sizeof(tones) / sizeof((tones)[0]))

/* ---------------------------------------------------------------- microphone
 * 16 kHz, 16-bit stereo capture from /dev/audio1 (the firmware's record test settings), read by
 * a worker task into a ring of the last FW_MIC_RING frames. The microphone is on the right channel.
 * fw_mic_open returns NULL only when no worker is free; fw_mic_failed tells, after a moment,
 * whether the device was in use or memory was short. The functions accept NULL. */
#define FW_MIC_RATE 16000
#define FW_MIC_RING 1024
typedef fw_worker_t fw_mic_t;

fw_mic_t *fw_mic_open(void);
bool fw_mic_failed(const fw_mic_t *m);
/* Copies the newest frames (interleaved left/right samples) into dst, at most FW_MIC_RING / 2.
 * Returns the number copied: fewer than asked (or 0) right after opening. */
uint32_t fw_mic_latest(fw_mic_t *m, int16_t *dst, uint32_t frames);
/* Stops the capture task and closes the device. */
void fw_mic_close(fw_mic_t *m);

/* ---------------------------------------------------------------- small helpers */
/* True if the device is open, or missing. CPU0 only (it walks the driver list). */
bool fw_dev_busy(const char *name);
char *fw_put_str(char *p, const char *s);
char *fw_put_dec(char *p, uint32_t v);
char *fw_put_hex(char *p, uint32_t v, int digits);

/* Label in the UI font. */
lv_obj_t *fw_label(lv_obj_t *parent, const char *text, lv_color_t color);
/* Plain rectangle without the theme's padding, border or scrolling. */
lv_obj_t *fw_rect(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t color);
