/*
 * Emscripten / LVGL 8.3 WebAssembly compatibility shim for ipod/ipod_ui.c.
 * Allows compiling the exact unmodified SL6801 firmware UI (ipod/ipod_ui.c)
 * and generated assets directly to WASM for 1:1 in-browser rendering.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "lvgl.h"

/* ---------------------------------------------------------------- Color & Image helpers */
#define LV_IMG_HEADER(cf, w, h) ((uint32_t)(cf) | ((uint32_t)(w) << 10) | ((uint32_t)(h) << 21))

static inline lv_color_t lv_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    lv_color_t c;
    c.full = (uint16_t)((v >> 8) | (v << 8)); /* LV_COLOR_16_SWAP = 1 */
    return c;
}

/* Override LV_ANIM_PATH_* macros to real LVGL 8.3 path callbacks */
#define LV_ANIM_PATH_LINEAR      lv_anim_path_linear
#define LV_ANIM_PATH_EASE_OUT    lv_anim_path_ease_out
#define LV_ANIM_PATH_EASE_IN_OUT lv_anim_path_ease_in_out

/* ---------------------------------------------------------------- Fast accessors -> real LVGL 8.3 */
static inline void lv_indev_read_point(void *indev, int16_t *x, int16_t *y)
{
    lv_point_t pt = {0, 0};
    if (indev) lv_indev_get_point((const lv_indev_t *)indev, &pt);
    *x = pt.x;
    *y = pt.y;
}

static inline const char *lv_label_get_text_fast(lv_obj_t *lbl)
{
    if (!lbl) return "";
    const char *s = lv_label_get_text(lbl);
    return s ? s : "";
}

static inline int32_t lv_bar_get_val_fast(lv_obj_t *bar)
{
    return bar ? lv_bar_get_value(bar) : 0;
}

static inline int32_t lv_bar_get_max_fast(lv_obj_t *bar)
{
    return bar ? lv_bar_get_max_value(bar) : 100;
}

/* ---------------------------------------------------------------- ROM libc & math shims */
#define rom_memcpy memcpy
#define rom_memset memset
#define rom_strcmp strcmp
#define rom_strlen strlen
#define rom_sinf   sinf
#define rom_cosf   cosf

static inline void rom_pcm_mono_to_stereo(int16_t *dst, const int16_t *src, uint32_t n)
{
    for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
        dst[i * 2] = src[i];
        dst[i * 2 + 1] = src[i];
    }
}

/* ---------------------------------------------------------------- CPU0 / OS / FatFs shims */
#define FW_DEV_LIST    ((void *)0)
#define FW_AUDIO_PA_CB 0u
#define fw_is_ram(p)   ((p) != NULL)
#define FW_HEAP_FREE   (0x20000u)

static inline void *fw_dev_open(const char *n, int f, const void *p) { (void)n; (void)f; (void)p; return NULL; }
static inline int   fw_dev_close(void **n) { (void)n; return 0; }
static inline int   fw_dev_write(void *n, const void *b, uint32_t c, const void *a) { (void)n; (void)b; (void)c; (void)a; return 0; }
static inline void *fw_os_calloc(uint32_t n, uint32_t sz) { return calloc(n, sz); }
static inline void  fw_os_free(void *p) { free(p); }
static inline void  fw_os_sleep(uint32_t ms) { (void)ms; }
static inline void *fw_task_create(void (*e)(void *), void *a, const char *n, int p, uint32_t s) { (void)e; (void)a; (void)n; (void)p; (void)s; return NULL; }
static inline void  fw_watchdog_setup(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; }
static inline uint32_t rom_tick_count(void) { return lv_tick_get(); }

#define FA_READ          0x01u
#define FA_WRITE         0x02u
#define FA_CREATE_NEW    0x04u
#define FA_CREATE_ALWAYS 0x08u
typedef struct { uint64_t opaque[1]; } fw_file_t;
typedef struct { uint64_t size; } fw_fileinfo_t;
#define fw_sd_mounted() (false)
static inline int f_open(fw_file_t *fp, const char *p, uint8_t m) { (void)fp; (void)p; (void)m; return -1; }
static inline int f_close(fw_file_t *fp) { (void)fp; return 0; }
static inline int f_write(fw_file_t *fp, const void *b, uint32_t n, uint32_t *d) { (void)fp; (void)b; (void)n; (void)d; return -1; }
static inline int f_lseek(fw_file_t *fp, uint64_t o) { (void)fp; (void)o; return -1; }
static inline int f_mkdir(const char *p) { (void)p; return -1; }
static inline int f_stat(const char *p, fw_fileinfo_t *f) { (void)p; (void)f; return -1; }

static inline uint32_t stock_lv_timer_handler(void) { return lv_timer_handler(); }
static inline void stock_disp_flush(void *d, const lv_area_t *a, const uint16_t *c) { (void)d; (void)a; (void)c; }
static inline void stock_key_cb(uint32_t *k) { (void)k; }

/* Shared click sequence & shot ctx globals for WASM */
extern volatile uint32_t g_web_click_seq;
extern volatile uint32_t g_web_shot_ctx;
#define CLICK_SEQ    g_web_click_seq
#define SHOT_CTX_RAW g_web_shot_ctx

/* ---------------------------------------------------------------- Font & Mock Stock Pages */
#define FW_FONT_UI (&lv_font_montserrat_12)

typedef struct {
    lv_obj_t *btn[6];
    lv_obj_t *lbl[6];
    lv_obj_t *icon[6];
} stock_music_menu_t;

typedef struct {
    lv_obj_t *hdr;
    lv_obj_t *btn[6];
    lv_obj_t *lbl[6];
    lv_obj_t *icon[6];
    uint32_t  _unk4c;
    lv_obj_t *prev_pg;
    lv_obj_t *next_pg;
    lv_obj_t *mbox;
    uint32_t  _unk5c;
    lv_obj_t *back_btn;
} stock_music_list_t;

typedef struct {
    lv_obj_t *track_cnt;
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *_unk0c;
    lv_obj_t *btnm;
    lv_obj_t *elapsed;
    lv_obj_t *total;
    lv_obj_t *bar;
    lv_obj_t *btn_prev;
    lv_obj_t *btn_play;
    lv_obj_t *btn_next;
    uint16_t  last_btn;
    uint8_t   flag;
    uint8_t   _pad2f;
    lv_obj_t *btn_back;
    lv_obj_t *btn_set;
} stock_music_play_t;

extern stock_music_menu_t *web_fw_music_menu(void);
extern stock_music_list_t *web_fw_music_song(void);
extern stock_music_play_t *web_fw_music_play(void);
extern stock_music_list_t *web_fw_music_artist(void);
extern stock_music_list_t *web_fw_music_album(void);

#define fw_music_menu()   web_fw_music_menu()
#define fw_music_song()   web_fw_music_song()
#define fw_music_play()   web_fw_music_play()
#define fw_music_artist() web_fw_music_artist()
#define fw_music_album()  web_fw_music_album()

extern int  stock_page_music_menu_create(void);
extern int  stock_page_music_song_create(void);
extern int  stock_page_music_play_create(void);
extern int  stock_page_music_artist_create(void);
extern int  stock_page_music_album_create(void);
extern void view_send_msg(uint32_t page_id, uint32_t type, uint32_t sub_id, uint32_t ev);
