/*
 * Firmware API for code injected into yp3_2.0.43 (SL6801). Every function here is an address in
 * the vendor firmware, called through a constant function pointer (ldr + blx). LVGL is v8.3 with
 * LV_COLOR_16_SWAP=1, LV_USE_USER_DATA=0 and 16-bit lv_coord_t.
 *
 * Injected code runs from flash and must not have writable globals (.data/.bss are rejected by the
 * linker script); keep state in memory from lv_mem_alloc.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_FN(addr, ret, args) ((ret(*) args)((uintptr_t)(addr) | 1u))

/* ---------------------------------------------------------------- LVGL types */

typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_font_t lv_font_t;
typedef int16_t lv_coord_t;
typedef uint32_t lv_style_selector_t;

typedef union {
	uint16_t full;
} lv_color_t;

typedef struct {
	uint32_t header; /* cf:5, always_zero:3, reserved:2, w:11, h:11 */
	uint32_t data_size;
	const uint8_t *data;
} lv_img_dsc_t;

#define LV_IMG_CF_TRUE_COLOR 4
#define LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED 6
/* Palette of 16 lv_color32_t (b, g, r, a) followed by rows of 4-bit indices, first pixel in the
 * high nibble. Drawn with per-entry alpha, so opaque entries need a = 0xFF. */
#define LV_IMG_CF_INDEXED_4BIT 9
/* The same with 4 palette entries and 2-bit indices, first pixel in the top bits. */
#define LV_IMG_CF_INDEXED_2BIT 8
/* The same with 256 palette entries and one byte per pixel. */
#define LV_IMG_CF_INDEXED_8BIT 10
#define LV_IMG_HEADER(cf, w, h) ((uint32_t)(cf) | ((uint32_t)(w) << 10) | ((uint32_t)(h) << 21))

/* lv_group_t: frozen is bit 0 and editing bit 1 of the byte at offset 0x18. */
typedef struct {
	uint8_t _ll[12];
	lv_obj_t **obj_focus;
	void *focus_cb;
	void *edge_cb;
	uint8_t flags;
} lv_group_t;

/* lv_event_t layout (v8.3 without user data on objects). */
typedef struct {
	lv_obj_t *target;
	lv_obj_t *current_target;
	uint8_t code;
	void *user_data;
	void *param;
} lv_event_t;

typedef void (*lv_event_cb_t)(lv_event_t *e);

#define LV_EVENT_ALL 0
#define LV_EVENT_PRESSED 1
#define LV_EVENT_RELEASED 8
#define LV_EVENT_KEY 13
#define LV_EVENT_DELETE 33

#define LV_KEY_NEXT 9
#define LV_KEY_ENTER 10
#define LV_KEY_PREV 11
#define LV_KEY_RIGHT 19
#define LV_KEY_LEFT 20
#define LV_KEY_ESC 27
/* Physical buttons on this player, as seen in LV_EVENT_KEY. ENTER's KEY event arrives on release;
 * LV_EVENT_PRESSED arrives on press. */
#define FW_KEY_BACK 0x81 /* bottom button; the firmware's pages treat it as "leave page" */
#define FW_KEY_UP 0xA0   /* top button */
#define FW_KEY_CENTER LV_KEY_ENTER
#define FW_KEY_LEFT LV_KEY_LEFT
#define FW_KEY_RIGHT LV_KEY_RIGHT
/* Holding a button sends _LONG once, then _REPEAT a few times per second, then _LONG_UP on
 * release (from the keypad driver at 0xD02018). Center only sends FW_KEY_CENTER_LONG, and only
 * while the focused object has FW_OBJ_FLAG_CENTER_LONG (see sdk/src/keys.c); otherwise holding it
 * powers the player off. FW_KEY_BACK_LONG needs FW_OBJ_FLAG_BACK_LONG; otherwise holding Back
 * opens the volume control. */
#define FW_KEY_BACK_LONG 0x82
#define FW_KEY_BACK_LONG_UP 0x83
#define FW_KEY_BACK_REPEAT 0x84
#define FW_KEY_LEFT_LONG 0x85
#define FW_KEY_LEFT_LONG_UP 0x86
#define FW_KEY_LEFT_REPEAT 0x87
#define FW_KEY_RIGHT_LONG 0x88
#define FW_KEY_RIGHT_LONG_UP 0x89
#define FW_KEY_RIGHT_REPEAT 0x8A
#define FW_KEY_CENTER_LONG 0x8B
#define FW_KEY_CENTER_LONG_UP 0x8C /* not sent on this player */
#define FW_KEY_CENTER_REPEAT 0x8D  /* not sent on this player */
#define FW_KEY_UP_LONG 0xA1
#define FW_KEY_UP_LONG_UP 0xA2
#define FW_KEY_UP_REPEAT 0xA3

/* Key manager (FUN_00CDD5C0): readiness check (0x5A once keys are being handled), its state
 * (1 while the screen is on), and the call it uses to hand a (key id, event) pair to the keypad
 * driver, which holds one pending pair. */
#define fw_key_ready FW_FN(0x00CF09D4, uint8_t, (void))
#define FW_KEY_STATE (*(volatile const uint8_t *)0x0081BC50u)
#define fw_key_forward FW_FN(0x00D0227C, void, (uint8_t id, uint8_t event))

#define LV_ALIGN_TOP_MID 2
#define LV_ALIGN_CENTER 9
#define LV_ALIGN_BOTTOM_LEFT 4
#define LV_ALIGN_BOTTOM_MID 5

#define LV_LABEL_LONG_WRAP 0
#define LV_TEXT_ALIGN_CENTER 2
#define LV_TEXT_ALIGN_RIGHT 3

#define LV_OBJ_FLAG_HIDDEN (1u << 0)
#define LV_OBJ_FLAG_CLICKABLE (1u << 1)
#define LV_OBJ_FLAG_SCROLLABLE (1u << 4)
#define LV_OBJ_FLAG_USER_1 (1u << 27) /* bits 27-30 are free for applications */
#define LV_OBJ_FLAG_USER_2 (1u << 28)
#define lv_obj_flags(obj) (*(volatile const uint32_t *)((const uint8_t *)(obj) + 0x1C))

typedef struct _lv_timer_t {
	uint32_t period;
	uint32_t last_run;
	void (*timer_cb)(struct _lv_timer_t *t);
	void *user_data;
	int32_t repeat_count;
	uint8_t paused;
} lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *t);

static inline lv_color_t lv_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
	lv_color_t c = {(uint16_t)((v >> 8) | (v << 8))}; /* LV_COLOR_16_SWAP */
	return c;
}

/* ---------------------------------------------------------------- LVGL functions */

#define lv_mem_alloc FW_FN(0x00D3909C, void *, (size_t size))
#define lv_mem_free FW_FN(0x00D39030, void, (void *p))
/* The LVGL heap is a 40 KB TLSF pool shared with the whole UI. The image cache is disabled
 * (LV_IMG_CACHE_DEF_SIZE 0), so images in RAM can be changed in place and redrawn. When LVGL
 * itself can't allocate (e.g. in lv_label_set_text), it spins forever and the UI freezes. */
typedef struct {
	uint32_t total_size, free_cnt, free_size, free_biggest_size, used_cnt, max_used;
	uint8_t used_pct, frag_pct;
} lv_mem_monitor_t;
#define lv_mem_monitor FW_FN(0x00D3904C, void, (lv_mem_monitor_t * mon))

#define lv_disp_get_layer_top FW_FN(0x00D27EF0, lv_obj_t *, (void *disp))
#define lv_disp_get_scr_act FW_FN(0x00D27E88, lv_obj_t *, (void *disp))
#define lv_disp_get_hor_res FW_FN(0x00D377B8, lv_coord_t, (void *disp))
#define lv_disp_get_ver_res FW_FN(0x00D377E0, lv_coord_t, (void *disp))
#define lv_layer_top() lv_disp_get_layer_top(NULL)

#define lv_obj_create FW_FN(0x00D2A250, lv_obj_t *, (lv_obj_t * parent))
#define lv_obj_del FW_FN(0x00D2E3B4, void, (lv_obj_t * obj))
#define lv_obj_set_size FW_FN(0x00D2B27E, void, (lv_obj_t * obj, lv_coord_t w, lv_coord_t h))
#define lv_obj_set_width FW_FN(0x00D2B21E, void, (lv_obj_t * obj, lv_coord_t w))
#define lv_obj_set_height FW_FN(0x00D2B24E, void, (lv_obj_t * obj, lv_coord_t h))
#define lv_obj_set_pos FW_FN(0x00D2B338, void, (lv_obj_t * obj, lv_coord_t x, lv_coord_t y))
#define lv_obj_align FW_FN(0x00D2B406, void, (lv_obj_t * obj, uint8_t align, lv_coord_t x, lv_coord_t y))
#define lv_obj_add_flag FW_FN(0x00D2A268, void, (lv_obj_t * obj, uint32_t f))
#define lv_obj_clear_flag FW_FN(0x00D2A2AE, void, (lv_obj_t * obj, uint32_t f))
#define lv_obj_invalidate FW_FN(0x00D2B770, void, (lv_obj_t * obj))

#define lv_timer_create FW_FN(0x00D39F34, lv_timer_t *, (lv_timer_cb_t cb, uint32_t period, void *user_data))
#define lv_timer_del FW_FN(0x00D39FA4, void, (lv_timer_t * t)) /* allowed inside the timer's own callback */
#define lv_tick_get FW_FN(0x00D37980, uint32_t, (void))
static inline uint32_t lv_tick_elaps(uint32_t prev) { return lv_tick_get() - prev; }
#define lv_obj_add_event_cb \
	FW_FN(0x00D280C8, void *, (lv_obj_t * obj, lv_event_cb_t cb, uint8_t filter, void *user_data))
#define lv_obj_remove_event_cb FW_FN(0x00D28148, bool, (lv_obj_t * obj, lv_event_cb_t cb))

#define lv_obj_set_style_align FW_FN(0x00D2E074, void, (lv_obj_t * obj, uint32_t v, lv_style_selector_t s))
#define lv_obj_set_style_pad_top FW_FN(0x00D2E080, void, (lv_obj_t * obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_pad_bottom FW_FN(0x00D2E08C, void, (lv_obj_t * obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_pad_left FW_FN(0x00D2E098, void, (lv_obj_t * obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_pad_right FW_FN(0x00D2E0A4, void, (lv_obj_t * obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_radius FW_FN(0x00D2E0C8, void, (lv_obj_t * obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_bg_color FW_FN(0x00D2E0F6, void, (lv_obj_t * obj, lv_color_t v, lv_style_selector_t s))
#define lv_obj_set_style_bg_opa FW_FN(0x00D2E118, void, (lv_obj_t * obj, uint8_t v, lv_style_selector_t s))
#define lv_obj_set_style_border_color FW_FN(0x00D2E12C, void, (lv_obj_t * obj, lv_color_t v, lv_style_selector_t s))
#define lv_obj_set_style_border_width FW_FN(0x00D2E158, void, (lv_obj_t * obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_text_color FW_FN(0x00D2E16E, void, (lv_obj_t * obj, lv_color_t v, lv_style_selector_t s))
#define lv_obj_set_style_text_font \
	FW_FN(0x00D2E192, void, (lv_obj_t * obj, const lv_font_t *v, lv_style_selector_t s))
#define lv_obj_set_style_text_align FW_FN(0x00D2E19E, void, (lv_obj_t * obj, uint8_t v, lv_style_selector_t s))

#define lv_label_create FW_FN(0x00D3F1FC, lv_obj_t *, (lv_obj_t * parent))
#define lv_label_set_text FW_FN(0x00D3FCA8, void, (lv_obj_t * obj, const char *text))
#define lv_label_set_long_mode FW_FN(0x00D3FE18, void, (lv_obj_t * obj, uint8_t mode))
#define lv_img_create FW_FN(0x00D3E764, lv_obj_t *, (lv_obj_t * parent))
#define lv_img_set_src FW_FN(0x00D3E77C, void, (lv_obj_t * obj, const void *src))

#define lv_event_get_code FW_FN(0x00D28098, uint8_t, (lv_event_t * e))
#define lv_event_get_param FW_FN(0x00D2809C, void *, (lv_event_t * e))
static inline void *lv_event_get_user_data(lv_event_t *e) { return e->user_data; }

#define lv_group_get_default FW_FN(0x00D284D0, lv_group_t *, (void))
#define lv_group_add_obj FW_FN(0x00D28580, void, (lv_group_t * g, lv_obj_t * obj))
#define lv_group_focus_obj FW_FN(0x00D28714, void, (lv_obj_t * obj))
#define lv_group_get_focused FW_FN(0x00D286AC, lv_obj_t *, (lv_group_t * g))
static inline void lv_group_focus_freeze(lv_group_t *g, bool en)
{
	g->flags = (uint8_t)((g->flags & ~1u) | (en ? 1u : 0u));
}

/* ---------------------------------------------------------------- devices and audio
 * OAL device layer. Devices are nodes in a circular list headed at FW_DEV_LIST: the link to the
 * next node is at node+0x14, the name pointer at node+0, the driver's private data at node+4, and
 * bit 0 of the byte at node+0x10 is set while the device is open. fw_dev_open returns the node;
 * if the device is already open it returns the same node without opening it again. */
#define FW_DEV_LIST ((void *)0x0081D40C)
#define fw_dev_open FW_FN(0x00D44604, void *, (const char *name, int flags, const void *params))
#define fw_dev_close FW_FN(0x00D44674, int, (void **node)) /* also clears *node */
#define fw_dev_write FW_FN(0x00D446BE, int, (void *node, const void *buf, uint32_t count, const void *arg))
#define fw_dev_read FW_FN(0x00D4469C, int, (void *node, void *buf, uint32_t count, const void *arg))

/* "/dev/audio0" open parameters (10 words): channels, bytes per sample, sample rate (above 16000;
 * lower rates fail), period in frames (every write must be exactly one period), number of
 * periods in the ring buffer, frames buffered before output starts (0: the whole ring), 0, 0,
 * event callback (may be NULL), callback argument. A write blocks while the ring is full.
 * The callback gets (arg, 1) after open and (arg, 2) on close; FW_AUDIO_PA_CB is the video
 * player's, which switches the speaker amplifier on (if the Speaker setting is on) and off. */
#define FW_AUDIO_PA_CB 0x00CF34C5u

/* "/dev/audio1" (record) open parameters: the same layout, except that word 7 selects the input:
 * byte 0 is the AMIC gain for 2 channels (0: default), byte 1 the input source. The firmware's
 * record test uses 2 channels, 16000 Hz, period 128, 2 periods and source 7. A read (count in
 * frames, any number) starts capture, blocks until the frames are in and returns count; reading
 * too slowly drops frames. */

/* "/dev/sd", as opened by FatFs for drive 0 when the card is mounted (NULL before). A read takes
 * the first sector's number by pointer: fw_dev_read(node, buf, sectors, &sector) returns sectors,
 * or 0 on error. The driver serializes access with its own mutex. */
#define FW_SD_NODE (*(void *const volatile *)(uintptr_t)0x0081A7CCu)
/* The SD media's FatFs-style ioctl, in RAM: FW_SD_SECTOR_COUNT stores the card's size in sectors. */
#define fw_sd_ioctl FW_FN(0x00816778, int, (int cmd, uint32_t *out))
#define FW_SD_SECTOR_COUNT 1
#define FW_SD_SECTOR_SIZE 512

/* ---------------------------------------------------------------- files (FatFs R0.14b)
 * Long names as UTF-8, exFAT. "0:" is the SD card ("0:/dir/file"; '\' works too, and the drive
 * is optional). Each call locks the volume with a mutex, so calls are safe from any task on CPU0,
 * never from UI code on CPU1 (see sdk.h). The firmware mounts the card on insertion and unmounts it,
 * freeing its state without waiting for other users, on removal, USB card-reader mode and sleep.
 * So check fw_sd_mounted() before a batch of calls, keep files open only briefly, and treat
 * FR_NOT_ENABLED and FR_INVALID_OBJECT as "card gone". Up to 20 files and directories can be
 * open at once, shared with the firmware. Path calls and f_readdir take 0x460 bytes of firmware
 * heap through oal_malloc, which on failure returns printf's result instead of NULL, so FatFs
 * then writes through a bad pointer: make sure the heap has room (FW_HEAP_FREE) first. */
enum {
	FR_OK = 0,
	FR_DISK_ERR = 1,
	FR_NO_FILE = 4,
	FR_NO_PATH = 5,
	FR_INVALID_OBJECT = 9,
	FR_NOT_ENABLED = 12,
	FR_NO_FILESYSTEM = 13,
	FR_NOT_ENOUGH_CORE = 17,
};
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10
#define AM_RDO 0x01
#define AM_HID 0x02
#define AM_SYS 0x04
#define AM_DIR 0x10
#define AM_ARC 0x20
typedef struct {
	uint64_t opaque[600 / 8];
} fw_file_t; /* FIL */
typedef struct {
	uint64_t opaque[88 / 8];
} fw_dir_t; /* DIR */
typedef struct {
	uint64_t size;
	uint16_t date, time; /* FAT format */
	uint8_t attrib;      /* AM_* */
	char altname[13];    /* 8.3 name; empty on exFAT */
	char name[256];      /* UTF-8; empty at the end of a directory */
} fw_fileinfo_t;         /* FILINFO, 0x120 bytes */
#define fw_file_size(fp) (*(const uint64_t *)((const uint8_t *)(fp) + 0x10))
/* The card is mounted: the drive table's flag and FatFs[0] are both set. */
#define fw_sd_mounted() (*(volatile const uint8_t *)0x0081A7D4u && *(void *volatile const *)0x0081DC80u)
#define f_open FW_FN(0x00D58890, int, (fw_file_t * fp, const char *path, uint8_t mode))
#define f_close FW_FN(0x00D58D60, int, (fw_file_t * fp))
#define f_read FW_FN(0x00D57918, int, (fw_file_t * fp, void *buf, uint32_t n, uint32_t *done))
#define f_write FW_FN(0x00D57AD6, int, (fw_file_t * fp, const void *buf, uint32_t n, uint32_t *done))
#define f_lseek FW_FN(0x00D57CCE, int, (fw_file_t * fp, uint64_t ofs))
#define f_sync FW_FN(0x00D58BB4, int, (fw_file_t * fp))
#define f_opendir FW_FN(0x00D58086, int, (fw_dir_t * dp, const char *path))
#define f_readdir FW_FN(0x00D58190, int, (fw_dir_t * dp, fw_fileinfo_t *fno))
#define f_closedir FW_FN(0x00D58162, int, (fw_dir_t * dp))
#define f_stat FW_FN(0x00D5825E, int, (const char *path, fw_fileinfo_t *fno))
#define f_getfree FW_FN(0x00D582CC, int, (const char *path, uint32_t *free_clusters, void **fs))
#define f_unlink FW_FN(0x00D583EA, int, (const char *path))
#define f_rename FW_FN(0x00D584F4, int, (const char *old_path, const char *new_path))
#define f_mkdir FW_FN(0x00D58D92, int, (const char *path))

/* Playback DMA state of the audio hardware, read by its interrupt (0x8167E8). In state PLAYING the
 * interrupt starts the next period from the ring, even after the device has been closed and the
 * ring freed, so close /dev/audio0 only once the ring has run dry (state STARVED). */
#define FW_AUDIO_HW (*(const uint8_t *volatile *)(uintptr_t)0x0081E320u)
#define FW_AUDIO_HW_STATE 0x21
enum { FW_AUDIO_IDLE, FW_AUDIO_PRIMING, FW_AUDIO_PLAYING, FW_AUDIO_STARVED };

/* ---------------------------------------------------------------- OS layer (FreeRTOS wrappers)
 * Pitfall: fw_os_calloc and fw_task_create return printf's return value (a small number), not
 * NULL, on failure; check results with fw_is_ram(). */
#define fw_is_ram(p) ((uintptr_t)(p) >= 0x00800000u && (uintptr_t)(p) < 0x00900000u)
#define fw_os_calloc FW_FN(0x00CD8610, void *, (uint32_t n, uint32_t size))
#define fw_os_free FW_FN(0x00CD8654, void, (void *p))
#define fw_os_heap_free FW_FN(0x00CD8660, uint32_t, (void))
#define fw_os_sleep FW_FN(0x00CD86B4, void, (uint32_t ms))
/* entry(arg) runs in a new task; fw_task_join waits for entry to return, then deletes the task.
 * The stack size is in 4-byte words and comes from the firmware heap. */
#define fw_task_create \
	FW_FN(0x00CD88A4, void *, (void (*entry)(void *), void *arg, const char *name, int priority, uint32_t stack))
#define fw_task_join FW_FN(0x00CD8960, void, (void *task))
#define fw_sem_create FW_FN(0x00CD86DE, void *, (void)) /* binary semaphore, initially empty */
#define fw_sem_take FW_FN(0x00CD874C, int, (void **sem, int timeout_ms)) /* -1 on timeout; -1 ms waits forever */
#define fw_sem_give FW_FN(0x00CD876E, void, (void **sem))
#define fw_sem_delete FW_FN(0x00CD86E8, int, (void **sem))
/* Software watchdog (0xD27B78): ViewTask (watch task 3) checks in once per loop and watch task 9
 * once per message. When a watched task has not checked in since the last refresh, it logs
 * "task %d (%s)is dead" and stops feeding /dev/wdog, which resets the player about 15 s later
 * through the power management chip (RAM is lost). */
#define fw_watch_checkin FW_FN(0x00D27C20, int, (void *watch_task))
/* Opens /dev/wdog for the monitor; called once at startup by the firmware's first task (CPU0). */
#define fw_watchdog_setup FW_FN(0x00D27AF0, void, (uint32_t a, uint32_t b, uint32_t c, uint32_t d))
#define FW_CURRENT_TCB (*(const uint8_t *volatile *)0x00800A28u) /* CPU0's pxCurrentTCB */

/* Volume as set by the user (low byte), and the software gain stage the video player applies to
 * 16-bit samples before writing them. */
#define fw_audio_volume FW_FN(0x00CDF884, uint32_t, (void))
/* Writes the current volume settings to the settings store in flash (PSMP); 1 on success. */
#define fw_volume_save FW_FN(0x00CDF6DC, int, (void))
#define fw_gain_create FW_FN(0x00D47BDC, void *, (int type, uint32_t rate, int channels, int bytes))
#define fw_gain_apply FW_FN(0x00D47C10, void, (void *gain, uint32_t volume, void *buf, uint32_t frames))
#define fw_gain_free FW_FN(0x00D47CB8, void, (void *gain))

/* ---------------------------------------------------------------- boot ROM
 * The mask ROM at 0 holds the FreeRTOS kernel that the OS layer wraps, a C library, low-level
 * drivers, and audio and Bluetooth code (README, "Boot ROM"). */
#define fw_log FW_FN(0x000002B0, int, (const char *fmt, ...)) /* printf to the debug UART */
#define rom_memcpy FW_FN(0x00000B2A, void *, (void *dst, const void *src, size_t n))
#define rom_memset FW_FN(0x00000BAA, void *, (void *dst, int c, size_t n))
#define rom_memmove FW_FN(0x00000C1E, void *, (void *dst, const void *src, size_t n))
#define rom_memcmp FW_FN(0x00000C54, int, (const void *a, const void *b, size_t n))
#define rom_strlen FW_FN(0x00000AA2, size_t, (const char *s))
#define rom_strcmp FW_FN(0x00000AB2, int, (const char *a, const char *b))
#define rom_strcpy FW_FN(0x00000A32, char *, (char *dst, const char *src))
#define rom_atoi FW_FN(0x00000D1E, int, (const char *s))
#define rom_qsort \
	FW_FN(0x00000C90, void, (void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)))
/* Expands n mono samples to n stereo frames; dst may equal src. Copies words when both are 4-byte
 * aligned and n is a multiple of 8. */
#define rom_pcm_mono_to_stereo FW_FN(0x000013EA, void, (int16_t *dst, const int16_t *src, uint32_t n))
/* Table lookups with linear interpolation. Worst errors: sinf/cosf 2e-5, log2f 3e-6, exp2f 1e-6
 * relative. log2f needs a normal x > 0; exp2f gives 0 below -127 and FLT_MAX from 128. */
#define rom_sinf FW_FN(0x00048488, float, (float x))
#define rom_cosf FW_FN(0x0004840C, float, (float x))
#define rom_log2f FW_FN(0x00048228, float, (float x))
#define rom_exp2f FW_FN(0x00048268, float, (float x))
/* FreeRTOS tick count. The tick is 1 kHz, so this is milliseconds since boot. Not for interrupts. */
#define rom_tick_count FW_FN(0x0000A2B4, uint32_t, (void))
/* FreeRTOS TaskStatus_t as this kernel fills it. Run times count FW_TIMER_HZ ticks (32 bits). */
enum { FW_TASK_READY = 1, FW_TASK_BLOCKED, FW_TASK_SUSPENDED, FW_TASK_DELETED };
typedef struct {
	void *handle;
	const char *name; /* inside the task's control block */
	uint32_t number;
	uint8_t state;
	uint32_t priority, base_priority;
	uint32_t run_time;
	uint16_t stack_free; /* lowest free stack so far, in words */
} fw_task_status_t;
/* uxTaskGetSystemState: fills up to max entries (0 if there are more tasks than that) and the
 * total run time; the scheduler is suspended meanwhile. */
#define rom_task_states FW_FN(0x0000A564, uint32_t, (fw_task_status_t * out, uint32_t max, uint32_t *total))
#define rom_task_count FW_FN(0x0000A2CC, uint32_t, (void))
/* FreeRTOS heap (heap_4): its size, free bytes now and the lowest free bytes so far. */
#define FW_HEAP_SIZE (*(volatile const uint32_t *)0x00800C74u)
#define FW_HEAP_FREE (*(volatile const uint32_t *)0x00800C64u)
#define FW_HEAP_MIN_FREE (*(volatile const uint32_t *)0x00800C68u)
/* The free-running 64-bit hardware timer behind the delays, and its rate in Hz. */
#define rom_timer_read64 FW_FN(0x0000B6D8, uint64_t, (void))
#define FW_TIMER_HZ (*(volatile const uint32_t *)0x00801374u)
/* Busy waits on the hardware timer; other tasks don't run meanwhile. */
#define rom_delay_us FW_FN(0x0000B73E, void, (uint32_t us))
#define rom_delay_ms FW_FN(0x0000B764, void, (uint32_t ms))
/* FreeRTOS critical section (masks interrupts at BASEPRI 0x50 and up) and scheduler suspension;
 * both nest. */
#define rom_enter_critical FW_FN(0x0000B514, void, (void))
#define rom_exit_critical FW_FN(0x0000B534, void, (void))
#define rom_suspend_all FW_FN(0x0000A2A4, void, (void))
#define rom_resume_all FW_FN(0x0000A428, int, (void))
/* Peripheral clock gates, numbered 0-95 across three 32-bit registers. */
#define rom_clk_enable FW_FN(0x00002564, void, (uint32_t id))
#define rom_clk_disable FW_FN(0x00002570, void, (uint32_t id))
#define rom_clk_is_enabled FW_FN(0x00002518, bool, (uint32_t id))
/* Input level of a GPIO pin, given as port << 16 | pin << 11 (the ROM reads 0x30000 for the boot
 * key). */
#define rom_gpio_get FW_FN(0x000007EC, bool, (uint32_t pin))

/* ---------------------------------------------------------------- vendor firmware */

/* The 2 MB SPI flash, memory mapped: flash offset X is at FW_FLASH_BASE + X. */
#define FW_FLASH_BASE 0x00C00000u
#define FW_FLASH_SIZE 0x00200000u
#define FW_FLASH_SECTOR 4096 /* erase unit */

#define get_lang_str FW_FN(0x00D01F8C, const char *, (int id))

/* The UI's 12 px, 1 bpp multilingual font (lv_font_t in .data). */
#define FW_FONT_UI ((const lv_font_t *)0x00819C98)
/* 23 px anti-aliased clock font; only has "/0123456789:CELMOW". */
#define FW_FONT_DIGITS ((const lv_font_t *)0x00C610F8)

/* Cortex-M SysTick current value, handy as a random seed. */
#define FW_SYSTICK_VAL (*(volatile uint32_t *)0xE000E018)

/* View context: *(FW_VIEW_CTX) points to a struct holding the key group and per-page state. */
#define FW_VIEW_CTX (*(uint8_t **)0x0081CE5C)
#define fw_view_group() (*(lv_group_t **)(FW_VIEW_CTX + 0x30))

#define lv_obj_get_parent(obj) (*(lv_obj_t **)((uint8_t *)(obj) + 4))
#define lv_obj_get_width FW_FN(0x00D2B4C2, lv_coord_t, (lv_obj_t * obj))
#define lv_obj_set_style_border_side FW_FN(0x00D2E164, void, (lv_obj_t * obj, uint8_t v, lv_style_selector_t s))
#define LV_LABEL_LONG_SCROLL_CIRCULAR 3
#define LV_LABEL_LONG_CLIP 4
#define LV_BORDER_SIDE_BOTTOM 1
#define LV_BORDER_SIDE_TOP 2

/* Vendor list helpers used by the menu pages. */
#define fw_list_item_create FW_FN(0x00D0267C, lv_obj_t *, (lv_obj_t * list)) /* full-width bordered row */
#define fw_list_item_layout FW_FN(0x00D32618, void, (lv_obj_t * obj, int main, int cross, int track)) /* flex */
#define FW_FONT_ICONS ((const lv_font_t *)0x00C810C4) /* 24 FontAwesome symbols (U+F001...) */

/* "APP" page (page id 0x53), opened from the home grid: a list of Alarm and Pictures. */
typedef struct {
	lv_obj_t *item[2];
	lv_obj_t *text[2];
	lv_obj_t *icon[2];
	uint8_t sel;
} fw_home_menu_page_t;
#define fw_home_menu_page() (*(fw_home_menu_page_t **)(FW_VIEW_CTX + 0x1DC))
#define page_home_menu_create FW_FN(0x00D0FC64, int, (void))
#define page_home_menu_event_cb FW_FN(0x00D0FB2C, void, (lv_event_t * e))
#define FW_CALL_HOME_MENU_CREATE 0x00D04C04 /* "bl page_home_menu_create" in view_page_create */

/* Page 0x51 (page_expand_home_create, 2x3 icon grid) exists in the code but is not reachable
 * from the UI on this model. */

/* Music "now playing" page (page id 4). Its state holds the labels; [1] is a child of the page's
 * full-width container. Called from view_page_create at 0xD04ABA. */
#define page_music_play_create FW_FN(0x00D12E64, int, (void))
#define fw_music_play_page() (*(lv_obj_t ***)(FW_VIEW_CTX + 0xA0))

/* Starts playing a file (MediaTask, CPU0): path and a type (3 for music). Called from 16 places
 * in the media state machine (_media_*_process). */
#define fw_media_start FW_FN(0x00D21800, int, (const char *path, uint32_t type))

/* LVGL style properties, numbered as this build's lv_obj_init_draw_rect_dsc (0xD2ABA8) reads them
 * (the background image ones lack the flag bits later LVGL versions add). */
#define lv_obj_set_local_style_prop \
	FW_FN(0x00D2DB28, void, (lv_obj_t * obj, uint32_t prop, uint32_t value, lv_style_selector_t s))
#define lv_obj_get_style_prop FW_FN(0x00D2D950, uint32_t, (lv_obj_t * obj, uint32_t part, uint32_t prop))
#define LV_STYLE_BG_OPA 0x21
#define LV_STYLE_BG_IMG_SRC 0x26
#define LV_STYLE_BG_IMG_OPA 0x27
/* Background image, drawn centered on the object; NULL removes it. On the music page's container
 * it covered the container's children (cause not found), so album art uses an lv_img child
 * instead. */
#define lv_obj_set_style_bg_img_src FW_FN(0x00D2E122, void, (lv_obj_t * obj, const void *src, lv_style_selector_t s))

/* TJpgDec (R0.03 style, extended), used by the photo viewer. The state (FW_JD_SIZE bytes) is
 * cleared by fw_jd_prepare except two words at 0xB8 and 0xBC. Output is RGB565 in native byte
 * order (swap the bytes for LVGL), in blocks given as a rect of uint16 left, right, top, bottom.
 * A progressive JPEG decodes from its first (DC) scan only, i.e. at 1/8 detail. Results: 0 ok,
 * 1 stopped by the output callback, 2 input error, 3 pool too small, 6 bad data, 8 unsupported. */
#define FW_JD_SIZE 0x118
#define FW_JD_POOL 0xE00      /* work pool size the photo viewer gives */
#define FW_JD_WIDTH 0x1C      /* uint16 */
#define FW_JD_HEIGHT 0x1E     /* uint16 */
#define FW_JD_DEVICE 0x7C     /* the dev pointer given to fw_jd_prepare */
#define FW_JD_PROGRESSIVE 0x84 /* uint32, 1 for a progressive JPEG */
/* uint32 flags that change what the input callback does: with FW_JD_TELL set, return the current
 * position; with FW_JD_SEEK set, go to position n and return n. Both happen only while an APP1
 * (Exif) segment is searched for a thumbnail, which fw_jd_prepare's skip_exif = 1 avoids. */
#define FW_JD_TELL 0xA8
#define FW_JD_SEEK 0xAC
typedef uint32_t (*fw_jd_in_t)(void *jd, uint8_t *buf, uint32_t n); /* buf NULL: skip n bytes */
typedef int (*fw_jd_out_t)(void *jd, void *bitmap, const uint16_t *rect); /* 0 stops */
/* skip_exif 0: an Exif thumbnail of at least 96x120 is decoded instead of the main image, and a
 * smaller one gives result 8 (the photo viewer then retries with 1). 1: APP1 segments are
 * skipped. */
#define fw_jd_prepare \
	FW_FN(0x00CF1A20, int, (void *jd, fw_jd_in_t in, void *pool, uint32_t pool_size, void *dev, int skip_exif))
#define fw_jd_decomp FW_FN(0x00CF1F90, int, (void *jd, fw_jd_out_t out, uint8_t scale)) /* 1/2^scale, 0-3 */
