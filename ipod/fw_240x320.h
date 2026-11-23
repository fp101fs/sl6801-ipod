/*
 * Verified firmware API for 240x320 Smartlink SL6801 (yp3_2.0.43, ST7789S + CST726).
 * Every address below was directly verified by disassembling firmware/sl6801_backup.bin.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FW_FN(addr, ret, args) ((ret(*) args)((uintptr_t)(addr) | 1u))

/* ---------------------------------------------------------------- LVGL 8.3 types */
typedef struct _lv_obj_t lv_obj_t;
typedef struct _lv_font_t lv_font_t;
typedef int16_t lv_coord_t;
typedef uint32_t lv_style_selector_t;

typedef union {
    uint16_t full;
} lv_color_t;

typedef struct {
    uint32_t header;
    uint32_t data_size;
    const uint8_t *data;
} lv_img_dsc_t;

#define LV_IMG_CF_TRUE_COLOR   4
#define LV_IMG_CF_INDEXED_2BIT 8
#define LV_IMG_CF_INDEXED_4BIT 9
#define LV_IMG_CF_INDEXED_8BIT 10
#define LV_IMG_HEADER(cf, w, h) ((uint32_t)(cf) | ((uint32_t)(w) << 10) | ((uint32_t)(h) << 21))

/* LVGL 8.3 lv_anim_t (verified 0x44 bytes from yp3_2.0.43 disassembly) */
typedef struct _lv_anim_t lv_anim_t;
typedef void (*lv_anim_exec_xcb_t)(void *, int32_t);
typedef int32_t (*lv_anim_path_cb_t)(const lv_anim_t *);
typedef void (*lv_anim_start_cb_t)(lv_anim_t *);
typedef void (*lv_anim_ready_cb_t)(lv_anim_t *);
typedef int32_t (*lv_anim_get_value_cb_t)(lv_anim_t *);

struct _lv_anim_t {
    void *var;                           /* 0x00 */
    lv_anim_exec_xcb_t exec_cb;          /* 0x04 */
    lv_anim_start_cb_t start_cb;         /* 0x08 */
    lv_anim_ready_cb_t ready_cb;         /* 0x0C */
    lv_anim_get_value_cb_t get_value_cb; /* 0x10 */
    void *user_data;                     /* 0x14 */
    lv_anim_path_cb_t path_cb;           /* 0x18 */
    int32_t start_value;                 /* 0x1C */
    int32_t current_value;               /* 0x20 */
    int32_t end_value;                   /* 0x24 */
    int32_t time;                        /* 0x28 */
    int32_t act_time;                    /* 0x2C */
    uint32_t playback_delay;             /* 0x30 */
    uint32_t playback_time;              /* 0x34 */
    uint32_t repeat_delay;               /* 0x38 */
    uint16_t repeat_cnt;                 /* 0x3C */
    uint8_t flags;                       /* 0x3E: early_apply:1, playback_now:1, run_round:1, start_cb_called:1 */
    uint8_t _pad3f;                      /* 0x3F */
    uint32_t time_orig;                  /* 0x40 */
};
_Static_assert(sizeof(lv_anim_t) == 0x44, "lv_anim_t must be 0x44 bytes");

typedef struct {
    lv_obj_t *target;
    lv_obj_t *current_target;
    uint8_t code;
    void *user_data;
    void *param;
} lv_event_t;

typedef void (*lv_event_cb_t)(lv_event_t *e);

typedef struct _lv_timer_t {
    uint32_t period;
    uint32_t last_run;
    void (*timer_cb)(struct _lv_timer_t *t);
    void *user_data;
    int32_t repeat_count;
    uint8_t paused;
} lv_timer_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *t);

/* Event codes in this LVGL 8.3 build */
#define LV_EVENT_ALL       0
#define LV_EVENT_PRESSED   1
#define LV_EVENT_PRESSING  2
#define LV_EVENT_SHORT_CLICKED 4
#define LV_EVENT_LONG_PRESSED  5
#define LV_EVENT_CLICKED   7
#define LV_EVENT_RELEASED  8
#define LV_EVENT_DELETE    33

/* Object flags */
#define LV_OBJ_FLAG_HIDDEN     (1u << 0)
#define LV_OBJ_FLAG_CLICKABLE  (1u << 1)
#define LV_OBJ_FLAG_SCROLLABLE (1u << 4)

/* Alignments & text modes */
#define LV_ALIGN_TOP_LEFT     1
#define LV_ALIGN_TOP_MID      2
#define LV_ALIGN_TOP_RIGHT    3
#define LV_ALIGN_BOTTOM_LEFT  4
#define LV_ALIGN_BOTTOM_MID   5
#define LV_ALIGN_BOTTOM_RIGHT 6
#define LV_ALIGN_LEFT_MID     7
#define LV_ALIGN_RIGHT_MID    8
#define LV_ALIGN_CENTER       9

#define LV_TEXT_ALIGN_LEFT   1
#define LV_TEXT_ALIGN_CENTER 2
#define LV_TEXT_ALIGN_RIGHT  3

#define LV_LABEL_LONG_WRAP            0
#define LV_LABEL_LONG_DOT             1
#define LV_LABEL_LONG_SCROLL_CIRCULAR 3
#define LV_LABEL_LONG_CLIP            4

#define LV_GRAD_DIR_NONE 0
#define LV_GRAD_DIR_VER  1
#define LV_GRAD_DIR_HOR  2

#define LV_BORDER_SIDE_BOTTOM 1
#define LV_BORDER_SIDE_TOP    2
#define LV_BORDER_SIDE_FULL   0x0F

static inline lv_color_t lv_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    lv_color_t c = {(uint16_t)((v >> 8) | (v << 8))}; /* LV_COLOR_16_SWAP = 1 */
    return c;
}

/* ---------------------------------------------------------------- Core LVGL functions */
typedef struct {
    uint32_t total_size, free_cnt, free_size, free_biggest_size, used_cnt, max_used;
    uint8_t used_pct, frag_pct;
} lv_mem_monitor_t;
#define lv_mem_alloc   FW_FN(0x00DAF4D0, void *, (size_t size))
#define lv_mem_free    FW_FN(0x00DAF464, void, (void *p))
#define lv_mem_monitor FW_FN(0x00DAF480, void, (lv_mem_monitor_t *mon))

#define lv_disp_get_scr_act   FW_FN(0x00D9E0A8, lv_obj_t *, (void *disp))
#define lv_disp_get_layer_top FW_FN(0x00D9E110, lv_obj_t *, (void *disp))
#define lv_indev_get_act      FW_FN(0x00D9EAB0, void *, (void))

typedef struct {
    lv_coord_t x1;
    lv_coord_t y1;
    lv_coord_t x2;
    lv_coord_t y2;
} lv_area_t;

#define stock_lv_timer_handler FW_FN(0x00DB0408, uint32_t, (void))
#define stock_disp_flush       FW_FN(0x00811754, void, (void *drv, const lv_area_t *area, const uint16_t *color_p))
#define stock_key_cb           FW_FN(0x00D53184, void, (uint32_t *key_val))

static inline void lv_indev_read_point(void *indev, int16_t *x, int16_t *y)
{
    const uint8_t *p = (const uint8_t *)indev;
    *x = *(const int16_t *)(p + 0x0C);
    *y = *(const int16_t *)(p + 0x0E);
}

#define lv_obj_create     FW_FN(0x00DA0474, lv_obj_t *, (lv_obj_t *parent))
#define lv_obj_del        FW_FN(0x00DA4610, void, (lv_obj_t *obj))
#define lv_obj_add_flag   FW_FN(0x00DA048C, void, (lv_obj_t *obj, uint32_t f))
#define lv_obj_clear_flag FW_FN(0x00DA04D2, void, (lv_obj_t *obj, uint32_t f))
#define lv_obj_set_width  FW_FN(0x00DA1442, void, (lv_obj_t *obj, lv_coord_t w))
#define lv_obj_set_size   FW_FN(0x00DA14A2, void, (lv_obj_t *obj, lv_coord_t w, lv_coord_t h))
#define lv_obj_set_x      FW_FN(0x00DA14EC, void, (lv_obj_t *obj, lv_coord_t x))
#define lv_obj_set_y      FW_FN(0x00DA1524, void, (lv_obj_t *obj, lv_coord_t y))
#define lv_obj_set_pos    FW_FN(0x00DA155C, void, (lv_obj_t *obj, lv_coord_t x, lv_coord_t y))
#define lv_obj_align      FW_FN(0x00DA162A, void, (lv_obj_t *obj, uint8_t align, lv_coord_t x, lv_coord_t y))
#define lv_obj_invalidate FW_FN(0x00DA1994, void, (lv_obj_t *obj))

/* LVGL 8.3 animations (verified from yp3_2.0.43 disassembly around 0x00DAE000) */
#define lv_anim_init      FW_FN(0x00DADFD8, void, (lv_anim_t *a))
#define lv_anim_del       FW_FN(0x00DAE00C, bool, (void *var, lv_anim_exec_xcb_t exec_cb))
#define lv_anim_start     FW_FN(0x00DAE060, lv_anim_t *, (const lv_anim_t *a))
#define LV_ANIM_PATH_LINEAR      ((lv_anim_path_cb_t)(uintptr_t)0x00DADDDDu)
#define LV_ANIM_PATH_EASE_OUT    ((lv_anim_path_cb_t)(uintptr_t)0x00DAE15Fu)
#define LV_ANIM_PATH_EASE_IN_OUT ((lv_anim_path_cb_t)(uintptr_t)0x00DAE195u)
#define lv_obj_get_child     FW_FN(0x00DA44C2, lv_obj_t *, (const lv_obj_t *obj, int32_t id))
#define lv_obj_get_child_cnt FW_FN(0x00DA44E0, uint32_t, (const lv_obj_t *obj))
#define lv_obj_get_parent(obj) (*(lv_obj_t **)((uint8_t *)(obj) + 4))

#define lv_obj_add_event_cb FW_FN(0x00D9E2EC, void *, (lv_obj_t *obj, lv_event_cb_t cb, uint8_t filter, void *user_data))
#define lv_event_get_code   FW_FN(0x00D9E2BC, uint8_t, (lv_event_t *e))
#define lv_event_get_target FW_FN(0x00D9E2B4, lv_obj_t *, (lv_event_t *e))

/* Style setters */
#define lv_obj_set_style_pad_top       FW_FN(0x00DA42A4, void, (lv_obj_t *obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_pad_bottom    FW_FN(0x00DA42B0, void, (lv_obj_t *obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_pad_left      FW_FN(0x00DA42BC, void, (lv_obj_t *obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_pad_right     FW_FN(0x00DA42C8, void, (lv_obj_t *obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_radius        FW_FN(0x00DA42EC, void, (lv_obj_t *obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_opa           FW_FN(0x00DA42F6, void, (lv_obj_t *obj, uint8_t v, lv_style_selector_t s))
#define lv_obj_set_style_bg_color      FW_FN(0x00DA431A, void, (lv_obj_t *obj, lv_color_t v, lv_style_selector_t s))
#define lv_obj_set_style_bg_opa        FW_FN(0x00DA433C, void, (lv_obj_t *obj, uint8_t v, lv_style_selector_t s))
#define lv_obj_set_style_bg_grad_color FW_FN(0x00DA4346, void, (lv_obj_t *obj, lv_color_t v, lv_style_selector_t s))
#define lv_obj_set_style_bg_grad_dir   FW_FN(0x00DA4368, void, (lv_obj_t *obj, uint8_t v, lv_style_selector_t s))
#define lv_obj_set_style_border_color  FW_FN(0x00DA4390, void, (lv_obj_t *obj, lv_color_t v, lv_style_selector_t s))
#define lv_obj_set_style_border_width  FW_FN(0x00DA43B2, void, (lv_obj_t *obj, lv_coord_t v, lv_style_selector_t s))
#define lv_obj_set_style_border_side   FW_FN(0x00DA43BE, void, (lv_obj_t *obj, uint8_t v, lv_style_selector_t s))
#define lv_obj_set_style_text_color    FW_FN(0x00DA43C8, void, (lv_obj_t *obj, lv_color_t v, lv_style_selector_t s))
#define lv_obj_set_style_text_font     FW_FN(0x00DA43EC, void, (lv_obj_t *obj, const lv_font_t *v, lv_style_selector_t s))
#define lv_obj_set_style_text_align    FW_FN(0x00DA43F8, void, (lv_obj_t *obj, uint8_t v, lv_style_selector_t s))

/* Widgets */
#define lv_label_create        FW_FN(0x00DB5610, lv_obj_t *, (lv_obj_t *parent))
#define lv_label_set_text      FW_FN(0x00DB60BC, void, (lv_obj_t *obj, const char *text))
#define lv_label_set_long_mode FW_FN(0x00DB622C, void, (lv_obj_t *obj, uint8_t mode))
#define lv_img_create          FW_FN(0x00DB4B78, lv_obj_t *, (lv_obj_t *parent))
#define lv_img_set_src         FW_FN(0x00DB4B90, void, (lv_obj_t *obj, const void *src))

#define lv_timer_create FW_FN(0x00DB0374, lv_timer_t *, (lv_timer_cb_t cb, uint32_t period, void *user_data))
#define lv_timer_del    FW_FN(0x00DB03E4, void, (lv_timer_t *t))
#define lv_tick_get     FW_FN(0x00DADDB4, uint32_t, (void))

/* Direct struct accessors (verified from disassembly) */
static inline const char *lv_label_get_text_fast(lv_obj_t *lbl)
{
    if (!lbl) return "";
    const char *s = *(const char *const *)((const uint8_t *)lbl + 0x24);
    return s ? s : "";
}

static inline int32_t lv_bar_get_val_fast(lv_obj_t *bar)
{
    return bar ? *(const int32_t *)((const uint8_t *)bar + 0x24) : 0;
}

static inline int32_t lv_bar_get_max_fast(lv_obj_t *bar)
{
    return bar ? *(const int32_t *)((const uint8_t *)bar + 0x2C) : 100;
}

/* ---------------------------------------------------------------- ROM libc & audio */
#define rom_memcpy FW_FN(0x00000B2A, void *, (void *dst, const void *src, size_t n))
#define rom_memset FW_FN(0x00000BAA, void *, (void *dst, int c, size_t n))
#define rom_strcmp FW_FN(0x00000AB2, int, (const char *a, const char *b))
#define rom_strlen FW_FN(0x00000AA2, size_t, (const char *s))
#define rom_pcm_mono_to_stereo FW_FN(0x000013EA, void, (int16_t *dst, const int16_t *src, uint32_t n))
#define rom_sinf FW_FN(0x00048488, float, (float x))
#define rom_cosf FW_FN(0x0004840C, float, (float x))

/* ---------------------------------------------------------------- CPU0 OS & Audio Devices */
#define FW_DEV_LIST    ((void *)0x0081D5E4u)
#define fw_dev_open    FW_FN(0x00DBAA14, void *, (const char *name, int flags, const void *params))
#define fw_dev_close   FW_FN(0x00DBAA84, int, (void **node))
#define fw_dev_write   FW_FN(0x00DBAACE, int, (void *node, const void *buf, uint32_t count, const void *arg))
#define FW_AUDIO_PA_CB 0x00D6BE99u

#define fw_is_ram(p) ((uintptr_t)(p) >= 0x00800000u && (uintptr_t)(p) < 0x00900000u)
#define fw_os_calloc   FW_FN(0x00D4E200, void *, (uint32_t n, uint32_t size))
#define fw_os_free     FW_FN(0x00D4E244, void, (void *p))
#define fw_os_sleep    FW_FN(0x00D4E2A4, void, (uint32_t ms))
#define fw_task_create FW_FN(0x00D4E494, void *, (void (*entry)(void *), void *arg, const char *name, int prio, uint32_t stack))
#define fw_watchdog_setup FW_FN(0x00D9DD10, void, (uint32_t a, uint32_t b, uint32_t c, uint32_t d))

#define rom_tick_count FW_FN(0x0000A2B4, uint32_t, (void))
#define FW_HEAP_FREE   (*(volatile const uint32_t *)0x00800C64u)

/* FatFs R0.14b (CPU0 ONLY; verified in 240x320 yp3_2.0.43) */
#define FA_READ          0x01u
#define FA_WRITE         0x02u
#define FA_CREATE_NEW    0x04u
#define FA_CREATE_ALWAYS 0x08u
typedef struct { uint64_t opaque[600 / 8]; } fw_file_t;
typedef struct {
    uint64_t size;
    uint16_t date, time;
    uint8_t  attrib;
    char     altname[13];
    char     name[256];
} fw_fileinfo_t;
#define fw_sd_mounted() (*(void *volatile const *)0x0081DE54u != NULL)
#define f_open  FW_FN(0x00DCEEF8, int, (fw_file_t *fp, const char *path, uint8_t mode))
#define f_close FW_FN(0x00DCF3C8, int, (fw_file_t *fp))
#define f_write FW_FN(0x00DCE13E, int, (fw_file_t *fp, const void *buf, uint32_t n, uint32_t *done))
#define f_lseek FW_FN(0x00DCE336, int, (fw_file_t *fp, uint64_t ofs))
#define f_mkdir FW_FN(0x00DCF3FA, int, (const char *path))
#define f_stat  FW_FN(0x00DCE8C6, int, (const char *path, fw_fileinfo_t *fno))

/* Reserved Cortex-M vector slots in RAM used as lock-free CPU1 <-> CPU0 shared slots */
#define CLICK_SEQ    (*(volatile uint32_t *)0x0080001Cu)
#define SHOT_CTX_RAW (*(volatile uint32_t *)0x00800020u)

/* ---------------------------------------------------------------- Vendor UI & Music Pages */
#define FW_VIEW_CTX  (*(uint8_t **)0x0081D008u)
#define FW_FONT_UI   ((const lv_font_t *)0x00819D70u)

/* Presenter message sender (CPU1 -> CPU0) */
#define view_send_msg FW_FN(0x00D7CE28, void, (uint32_t page_id, uint32_t type, uint32_t sub_id, uint32_t ev))

/* Stock music page constructors (hooked in view_page_create) */
#define stock_page_music_menu_create   FW_FN(0x00D8B9D8, int, (void))
#define stock_page_music_song_create   FW_FN(0x00D8CC94, int, (void))
#define stock_page_music_play_create   FW_FN(0x00D8BEB0, int, (void))
#define stock_page_music_artist_create FW_FN(0x00D8A630, int, (void))
#define stock_page_music_album_create  FW_FN(0x00D8A188, int, (void))

/* Stock page struct layouts */
typedef struct {
    lv_obj_t *btn[6];   /* 0x00..0x14 */
    lv_obj_t *lbl[6];   /* 0x18..0x2C */
    lv_obj_t *icon[6];  /* 0x30..0x44 */
} stock_music_menu_t;

typedef struct {
    lv_obj_t *hdr;      /* 0x00 */
    lv_obj_t *btn[6];   /* 0x04..0x18 */
    lv_obj_t *lbl[6];   /* 0x1C..0x30 */
    lv_obj_t *icon[6];  /* 0x34..0x48 */
    uint32_t  _unk4c;   /* 0x4C */
    lv_obj_t *prev_pg;  /* 0x50 */
    lv_obj_t *next_pg;  /* 0x54 */
    lv_obj_t *mbox;     /* 0x58 */
    uint32_t  _unk5c;   /* 0x5C */
    lv_obj_t *back_btn; /* 0x60 */
} stock_music_list_t;

typedef struct {
    lv_obj_t *track_cnt; /* 0x00: e.g. "1/253" */
    lv_obj_t *title;     /* 0x04 */
    lv_obj_t *artist;    /* 0x08 */
    lv_obj_t *_unk0c;    /* 0x0C */
    lv_obj_t *btnm;      /* 0x10 */
    lv_obj_t *elapsed;   /* 0x14: e.g. "00:00:21" */
    lv_obj_t *total;     /* 0x18: e.g. "00:03:25" */
    lv_obj_t *bar;       /* 0x1C */
    lv_obj_t *btn_prev;  /* 0x20 */
    lv_obj_t *btn_play;  /* 0x24 */
    lv_obj_t *btn_next;  /* 0x28 */
    uint16_t  last_btn;  /* 0x2C */
    uint8_t   flag;      /* 0x2E */
    uint8_t   _pad2f;    /* 0x2F */
    lv_obj_t *btn_back;  /* 0x30 */
    lv_obj_t *btn_set;   /* 0x34 */
} stock_music_play_t;

#define fw_music_menu()   (*(stock_music_menu_t **)(FW_VIEW_CTX + 0x90))
#define fw_music_song()   (*(stock_music_list_t **)(FW_VIEW_CTX + 0x94))
#define fw_music_play()   (*(stock_music_play_t **)(FW_VIEW_CTX + 0x98))
#define fw_music_artist() (*(stock_music_list_t **)(FW_VIEW_CTX + 0xA0))
#define fw_music_album()  (*(stock_music_list_t **)(FW_VIEW_CTX + 0xA4))
