/*
 * 60 FPS Blue iPod Classic UI (v2) + Polar-Angle Touch Click Wheel for Smartlink SL6801 (240x320).
 * Replaces Music Menu (page 2), Songs (page 3), Now Playing (page 4),
 * Artists (page 6), and Albums (page 7) while keeping CPU0 audio/BT/DB 100% intact.
 *
 * Combines:
 *   1. LVGL built-in lv_anim (cubic ease-out horizontal slide transitions)
 *   2. Direct RAM pixel buffers driven by a 16ms (60 FPS) lv_timer + Cortex-M4F FPU + rom_sinf
 *      (3D-tilted Album Art + Glossy Floor Reflection + Animated Specular Sheen,
 *       and Glossy Aqua Progress Bar with sub-pixel interpolation & shimmer)
 *   3. Compact LV_IMG_CF_INDEXED_4BIT baked sprite sheet icons (Play, Pause, Battery,
 *      and 5 Click Wheel quadrant tactile press glows)
 *
 * Rules: runs on CPU1 (LVGL UI thread); zero writable globals; all state in lv_mem_alloc.
 */
#include "fw_240x320.h"
#include "gen/clickwheel.h"
#include "gen/album_default.h"
#include "gen/sprites.h"
#include "gen/click_pcm.h"

/* ---------------------------------------------------------------- Geometry */
#define SCR_W 240
#define SCR_H 320

#define LCD_X 6
#define LCD_Y 4
#define LCD_W 228
#define LCD_H 142
#define HDR_H 20
#define BODY_W (LCD_W - 4)        /* 224 */
#define BODY_H (LCD_H - HDR_H - 4) /* 118 */

#define WHEEL_SIZE 156
#define WHEEL_X    ((SCR_W - WHEEL_SIZE) / 2) /* 42 */
#define WHEEL_Y    156
#define WHEEL_CX   (WHEEL_X + WHEEL_SIZE / 2) /* 120 */
#define WHEEL_CY   (WHEEL_Y + WHEEL_SIZE / 2) /* 234 */
#define WHEEL_R_IN  27
#define WHEEL_R_OUT 82

#define ROW_COUNT 6
#define ROW_H     19
#define SCROLL_X  (BODY_W - 9)
#define SCROLL_W  7
#define SCROLL_H  (BODY_H - 4)
#define THUMB_H   20

#define BAR_W     208
#define BAR_H     11
#define BAR_BUF_BYTES (1024u + (uint32_t)(BAR_W * BAR_H))

#define PI_F          3.14159265f
#define TWO_PI_F      6.28318530f
/* Click Wheel detents (2x travel distance: ~16.5 ticks/rev slow -> ~22 med -> ~27 fast) */
#define STEP_SLOW_RAD 0.38f
#define STEP_MED_RAD  0.29f
#define STEP_FAST_RAD 0.23f
#define STEP_MIN_MS      22u
#define CLICK_STOP_MAGIC 0xFFFFFFFFu
#define CLICK_IDLE_MAGIC 0xFFFFFFFEu

/* ---------------------------------------------------------------- Fast FPU atan2f */
static float fast_atan2f(float y, float x)
{
    float abs_y = (y < 0.0f ? -y : y) + 1e-6f;
    float angle;
    if (x >= 0.0f) {
        float r = (x - abs_y) / (x + abs_y);
        angle = 0.78539816f - 0.78539816f * r;
    } else {
        float r = (x + abs_y) / (abs_y - x);
        angle = 2.35619449f - 0.78539816f * r;
    }
    return (y < 0.0f) ? -angle : angle;
}

/* ---------------------------------------------------------------- Tiny string helpers */
static void fmt_u32(char *dst, uint32_t v, int min_digits)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u);
    while (n < min_digits) tmp[n++] = '0';
    for (int i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    dst[n] = '\0';
}

static int parse_hms_seconds(const char *s)
{
    if (!s || !s[0]) return -1;
    int parts[3] = {0, 0, 0};
    int np = 0, cur = 0;
    bool has_digit = false;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            cur = cur * 10 + (*p - '0');
            has_digit = true;
        } else if (*p == ':' && np < 2) {
            parts[np++] = cur;
            cur = 0;
            has_digit = false;
        }
    }
    if (!has_digit && np == 0) return -1;
    parts[np++] = cur;
    if (np == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (np == 2) return parts[0] * 60 + parts[1];
    return parts[0];
}

static void fmt_time(char *out, int sec, bool minus)
{
    if (sec < 0) sec = 0;
    int pos = 0;
    if (minus) out[pos++] = '-';
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    if (h > 0) {
        fmt_u32(out + pos, (uint32_t)h, 1);
        pos += (int)rom_strlen(out + pos);
        out[pos++] = ':';
        fmt_u32(out + pos, (uint32_t)m, 2);
    } else {
        fmt_u32(out + pos, (uint32_t)m, 1);
    }
    pos += (int)rom_strlen(out + pos);
    out[pos++] = ':';
    fmt_u32(out + pos, (uint32_t)s, 2);
}

/* Convert "6/15" -> "6 of 15" */
static void fmt_track_of(char *out, size_t cap, const char *src)
{
    if (!src || !src[0]) {
        out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (const char *p = src; *p && j + 5 < cap; p++) {
        if (*p == '/') {
            out[j++] = ' ';
            out[j++] = 'o';
            out[j++] = 'f';
            out[j++] = ' ';
        } else {
            out[j++] = *p;
        }
    }
    out[j] = '\0';
}

/* ---------------------------------------------------------------- Style & Anim helpers */
static void style_clean_box(lv_obj_t *o)
{
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_top(o, 0, 0);
    lv_obj_set_style_pad_bottom(o, 0, 0);
    lv_obj_set_style_pad_left(o, 0, 0);
    lv_obj_set_style_pad_right(o, 0, 0);
    lv_obj_set_style_bg_opa(o, 255, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, lv_color_t col, uint8_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, FW_FONT_UI, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, txt);
    return l;
}

static void anim_set_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v);
}

/* ---------------------------------------------------------------- UI State (in LVGL heap) */
typedef struct {
    uint8_t page_id; /* 2=Menu, 3=Songs, 4=NowPlaying, 6=Artists, 7=Albums */
    bool is_play;

    lv_obj_t *root;       /* 240x320 anodized blue metallic shell */
    lv_obj_t *lcd_body;   /* lv_anim horizontal-sliding content pane */
    lv_obj_t *wheel_glow; /* baked INDEXED_4BIT Click Wheel quadrant press glow */
    lv_obj_t *ico_state;  /* baked INDEXED_4BIT header Play/Pause sprite */
    lv_timer_t *timer;    /* 16ms (60 FPS) animation & live sync timer */
    uint8_t last_glow;    /* 0=none, 1=top, 2=bot, 3=left, 4=right, 5=center */
    uint32_t open_ms;     /* timestamp for guaranteed slide-in completion */
    bool slide_done;

    /* Deferred playback message (waits for ClickTask to close /dev/audio0 before FLAC/MP3 starts) */
    bool pending_msg;
    uint32_t pending_ms;
    uint32_t pend_page;
    uint32_t pend_type;
    uint32_t pend_sub;
    uint32_t pend_ev;

    /* Click wheel polar touch state */
    bool wheel_down;
    bool wheel_in_center;
    bool wheel_scrolled;
    float prev_angle;
    float accum_angle;
    float ang_vel;
    uint32_t prev_tick_ms;
    uint32_t last_step_ms;
    int16_t down_x;
    int16_t down_y;

    /* Now Playing widgets & direct 60 FPS RAM pixel buffers */
    lv_obj_t *lbl_count;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_artist;
    lv_obj_t *lbl_elapsed;
    lv_obj_t *lbl_remain;
    lv_obj_t *img_art;
    lv_obj_t *img_bar;
    uint8_t *art_buf;       /* 62x72 INDEXED_8BIT RAM buffer (5.5 KB) */
    uint8_t *bar_buf;       /* 208x11 INDEXED_8BIT RAM buffer (3.3 KB) */
    lv_img_dsc_t art_dsc;
    lv_img_dsc_t bar_dsc;
    float smooth_pct;
    float sheen_phase;
    bool is_paused;
    bool last_paused_known;
    uint8_t frame_div;
    char last_count[24];
    char last_title[80];
    char last_artist[80];
    char last_elap[16];
    char last_tot[16];

    /* List widgets (Music Menu / Songs / Artists / Albums) */
    lv_obj_t *sel_pill;     /* 60 FPS cubic ease-out Aqua blue highlight bar */
    lv_obj_t *scroll_thumb; /* classic iPod right-edge scrollbar thumb */
    lv_obj_t *row_lbl[ROW_COUNT];
    lv_obj_t *row_chev[ROW_COUNT];
    int8_t sel_idx;
    int8_t vis_idx;         /* row currently covered >=60% by sel_pill (-1 while transitioning) */
    int8_t marquee_idx;     /* row currently running horizontal marquee (-1 while gliding) */
    float pill_y;           /* current animated Y coordinate */
    float pill_start_y;     /* glide start Y */
    float pill_target_y;    /* glide target Y */
    uint32_t pill_start_ms; /* glide start timestamp */
    bool pill_moving;
    int16_t last_pill_y;
    int16_t last_thumb_y;
    uint32_t page_flip_ms;
    char last_row[ROW_COUNT][64];
} ipod_ui_t;

/* ---------------------------------------------------------------- Direct Pixel Buffer Renderers */
static void set_pal(uint8_t *pal, uint8_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = pal + (uint32_t)idx * 4u;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 0xFF;
}

static void init_bar_palette(uint8_t *buf)
{
    rom_memset(buf, 0, 1024u);
    set_pal(buf, 0, 0xFF, 0xFF, 0xFF); /* white corner */
    set_pal(buf, 1, 0xA4, 0xAE, 0xBC); /* recessed metallic border */

    /* Indices 2..12: Glossy silver-white empty track gradient (11 rows) */
    static const uint8_t trk_r[BAR_H] = {0xD4, 0xDC, 0xE6, 0xEE, 0xF4, 0xF8, 0xFA, 0xFC, 0xFC, 0xFA, 0xF2};
    static const uint8_t trk_g[BAR_H] = {0xDC, 0xE4, 0xEC, 0xF2, 0xF6, 0xFA, 0xFC, 0xFD, 0xFD, 0xFC, 0xF5};
    static const uint8_t trk_b[BAR_H] = {0xE6, 0xEC, 0xF2, 0xF7, 0xFA, 0xFD, 0xFF, 0xFF, 0xFF, 0xFE, 0xFA};

    /* Indices 16..26: Authentic Apple Aqua glossy blue bar gradient (11 rows) */
    static const uint8_t aq_r[BAR_H]  = {0x9C, 0x82, 0x6A, 0x54, 0x3E, 0x2A, 0x26, 0x28, 0x2E, 0x3A, 0x4C};
    static const uint8_t aq_g[BAR_H]  = {0xD8, 0xC6, 0xB4, 0xA2, 0x90, 0x7C, 0x76, 0x7A, 0x86, 0x98, 0xAE};
    static const uint8_t aq_b[BAR_H]  = {0xFF, 0xFC, 0xF8, 0xF2, 0xEC, 0xE2, 0xDC, 0xE0, 0xE6, 0xEE, 0xF6};

    for (int y = 0; y < BAR_H; y++) {
        set_pal(buf, (uint8_t)(2 + y), trk_r[y], trk_g[y], trk_b[y]);
        uint8_t r = aq_r[y], g = aq_g[y], b = aq_b[y];
        set_pal(buf, (uint8_t)(16 + y), r, g, b);
        /* Indices 32..42: Bright specular glass sheen (+42% toward white) */
        set_pal(buf, (uint8_t)(32 + y),
                (uint8_t)(r + ((255 - r) * 42) / 100),
                (uint8_t)(g + ((255 - g) * 42) / 100),
                (uint8_t)(b + ((255 - b) * 42) / 100));
        /* Indices 48..58: Soft sheen halo / sub-pixel leading edge (+18% toward white) */
        set_pal(buf, (uint8_t)(48 + y),
                (uint8_t)(r + ((255 - r) * 18) / 100),
                (uint8_t)(g + ((255 - g) * 18) / 100),
                (uint8_t)(b + ((255 - b) * 18) / 100));
    }
}

static void render_aqua_bar(ipod_ui_t *ui)
{
    if (!ui->bar_buf) return;
    uint8_t *px = ui->bar_buf + 1024u;
    float fill_f = ui->smooth_pct * (float)(BAR_W - 2);
    int fill_px = (int)fill_f;
    if (fill_px < 0) fill_px = 0;
    if (fill_px > BAR_W - 2) fill_px = BAR_W - 2;
    int sub_edge = ((fill_f - (float)fill_px) >= 0.35f && fill_px < BAR_W - 2) ? 1 : 0;

    /* Smooth animated specular sheen sweeping across the Aqua bar */
    float s = rom_sinf(ui->sheen_phase * 1.4f);
    int sheen_cx = (int)((s * 0.5f + 0.5f) * (float)(fill_px + 24)) - 12;

    for (int y = 0; y < BAR_H; y++) {
        uint8_t *row = px + y * BAR_W;
        bool top_bot = (y == 0 || y == BAR_H - 1);
        uint8_t trk_c = (uint8_t)(2 + y);
        uint8_t aq_c  = (uint8_t)(16 + y);
        uint8_t hi_c  = (uint8_t)(32 + y);
        uint8_t mid_c = (uint8_t)(48 + y);

        if (top_bot) {
            row[0] = 0;
            row[BAR_W - 1] = 0;
            row[1] = 1;
            row[BAR_W - 2] = 1;
            rom_memset(row + 2, 1, BAR_W - 4);
            continue;
        }

        row[0] = 1;
        row[BAR_W - 1] = 1;
        int end_x = 1 + fill_px;
        for (int x = 1; x < BAR_W - 1; x++) {
            if (x < end_x) {
                if (!ui->is_paused) {
                    int dist = (x - (y >> 1)) - sheen_cx;
                    if (dist < 0) dist = -dist;
                    if (dist <= 5)      row[x] = hi_c;
                    else if (dist <= 11) row[x] = mid_c;
                    else                 row[x] = aq_c;
                } else {
                    row[x] = aq_c;
                }
            } else if (x == end_x && sub_edge) {
                row[x] = mid_c;
            } else {
                row[x] = trk_c;
            }
        }
    }
    lv_obj_invalidate(ui->img_bar);
}

static void render_album_sheen(ipod_ui_t *ui)
{
    if (!ui->art_buf) return;
    const uint8_t *src = img_album_default_map + 1024u;
    uint8_t *dst = ui->art_buf + 1024u;
    rom_memcpy(dst, src, (size_t)(ALBUM_ART_W * ALBUM_ART_H));

    /* Subtle periodic diagonal glass sheen across the 3D-tilted cover art */
    if (!ui->is_paused) {
        float wave = rom_sinf(ui->sheen_phase * 0.75f);
        /* Sweep only during positive half-cycle so it feels like an authentic gentle gleam */
        if (wave > -0.2f) {
            int band = (int)((wave + 0.2f) * 82.0f) - 10;
            for (int y = 2; y < 56; y++) {
                uint8_t *row = dst + y * ALBUM_ART_W;
                int x_start = band - (y >> 1);
                int x_end = x_start + 7;
                if (x_start < 2) x_start = 2;
                if (x_end > ALBUM_ART_W - 3) x_end = ALBUM_ART_W - 3;
                for (int x = x_start; x < x_end; x++) {
                    uint8_t c = row[x];
                    if (c != 0u) row[x] = (uint8_t)(c | 0x80u);
                }
            }
        }
    }
    lv_obj_invalidate(ui->img_art);
}

/* ---------------------------------------------------------------- Row helpers */
static lv_obj_t *get_stock_row_label(uint8_t page_id, int idx)
{
    if (idx < 0 || idx >= ROW_COUNT) return NULL;
    if (page_id == 2) {
        stock_music_menu_t *m = fw_music_menu();
        return m ? m->lbl[idx] : NULL;
    }
    stock_music_list_t *l = NULL;
    if (page_id == 3) l = fw_music_song();
    else if (page_id == 6) l = fw_music_artist();
    else if (page_id == 7) l = fw_music_album();
    return l ? l->lbl[idx] : NULL;
}

static int count_valid_rows(uint8_t page_id)
{
    int cnt = 0;
    for (int i = 0; i < ROW_COUNT; i++) {
        const char *t = lv_label_get_text_fast(get_stock_row_label(page_id, i));
        if (t && t[0] && !(t[0] == ' ' && t[1] == '\0')) cnt = i + 1;
    }
    return cnt > 0 ? cnt : ROW_COUNT;
}

static void apply_row_marquee(ipod_ui_t *ui, int8_t idx)
{
    if (idx == ui->marquee_idx) return;
    if (ui->marquee_idx >= 0 && ui->marquee_idx < ROW_COUNT) {
        lv_label_set_long_mode(ui->row_lbl[ui->marquee_idx], LV_LABEL_LONG_DOT);
    }
    ui->marquee_idx = idx;
    if (idx >= 0 && idx < ROW_COUNT) {
        lv_label_set_long_mode(ui->row_lbl[idx], LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
}

static void apply_row_colors(ipod_ui_t *ui, int8_t vis)
{
    if (vis == ui->vis_idx) return;
    lv_color_t white = lv_rgb(0xFF, 0xFF, 0xFF);
    lv_color_t dark  = lv_rgb(0x14, 0x18, 0x1F);
    lv_color_t gray  = lv_rgb(0x9A, 0xA4, 0xB2);
    /* Only update the previously highlighted row and newly highlighted row (leaner redraws!) */
    if (ui->vis_idx >= 0 && ui->vis_idx < ROW_COUNT) {
        lv_obj_set_style_text_color(ui->row_lbl[ui->vis_idx], dark, 0);
        lv_obj_set_style_text_color(ui->row_chev[ui->vis_idx], gray, 0);
    }
    ui->vis_idx = vis;
    if (vis >= 0 && vis < ROW_COUNT) {
        lv_obj_set_style_text_color(ui->row_lbl[vis], white, 0);
        lv_obj_set_style_text_color(ui->row_chev[vis], white, 0);
    }
}

static void update_list_colors(ipod_ui_t *ui, bool snap_pill)
{
    float ty = (float)(ui->sel_idx * ROW_H + 2);
    if (snap_pill) {
        int16_t ity = (int16_t)(ui->sel_idx * ROW_H + 2);
        ui->pill_y = ty;
        ui->pill_start_y = ty;
        ui->pill_target_y = ty;
        ui->pill_moving = false;
        ui->last_pill_y = ity;
        lv_obj_set_y(ui->sel_pill, ity);
        if (ui->scroll_thumb) {
            int16_t sy = (int16_t)((ui->sel_idx * (SCROLL_H - THUMB_H - 2)) / (ROW_COUNT - 1));
            ui->last_thumb_y = sy;
            lv_obj_set_y(ui->scroll_thumb, sy);
        }
        apply_row_colors(ui, ui->sel_idx);
        apply_row_marquee(ui, ui->sel_idx);
    } else {
        /* Start silky time-based 60 FPS cubic ease-out glide from current pill_y */
        ui->pill_start_y = ui->pill_y;
        ui->pill_target_y = ty;
        ui->pill_start_ms = lv_tick_get();
        ui->pill_moving = true;
        /* Pause marquee during the brief 115ms glide so 100% SPI bandwidth goes to the pill */
        apply_row_marquee(ui, -1);
    }
}

static void click_trigger(void);

static void list_step(ipod_ui_t *ui, int dir, bool fast)
{
    int n = count_valid_rows(ui->page_id);
    if (ui->page_id != 2) n = ROW_COUNT;
    int next = ui->sel_idx + dir;
    uint32_t now = lv_tick_get();
    uint32_t cooldown = fast ? 130u : 200u;
    bool flipped = false;

    if (next >= n) {
        if (ui->page_id != 2) {
            if (now - ui->page_flip_ms < cooldown) return;
            ui->page_flip_ms = now;
            view_send_msg(ui->page_id, 6, 0, 0x16); /* Next page */
            ui->sel_idx = 0;
            flipped = true;
        } else {
            ui->sel_idx = (int8_t)(n - 1);
            return;
        }
    } else if (next < 0) {
        if (ui->page_id != 2) {
            if (now - ui->page_flip_ms < cooldown) return;
            ui->page_flip_ms = now;
            view_send_msg(ui->page_id, 6, 0, 0x17); /* Prev page */
            ui->sel_idx = (int8_t)(ROW_COUNT - 1);
            flipped = true;
        } else {
            ui->sel_idx = 0;
            return;
        }
    } else {
        ui->sel_idx = (int8_t)next;
    }
    click_trigger();
    update_list_colors(ui, flipped);
}

/* Signal ClickTask to close /dev/audio0 and defer view_send_msg until it is 100% closed */
static void send_msg_after_click_close(ipod_ui_t *ui, uint32_t page, uint32_t type, uint32_t sub, uint32_t ev)
{
    uint32_t seq = CLICK_SEQ;
    if (seq == 0u || seq == CLICK_IDLE_MAGIC) {
        view_send_msg(page, type, sub, ev);
        return;
    }
    CLICK_SEQ = CLICK_STOP_MAGIC;
    ui->pending_msg = true;
    ui->pending_ms = lv_tick_get();
    ui->pend_page = page;
    ui->pend_type = type;
    ui->pend_sub = sub;
    ui->pend_ev = ev;
}

static void click_trigger(void)
{
    uint32_t s = CLICK_SEQ;
    if (s == CLICK_STOP_MAGIC || s == CLICK_IDLE_MAGIC || s >= 0xFFFFFFF0u) CLICK_SEQ = 1u;
    else CLICK_SEQ = s + 1u;
}
static void show_quadrant_glow(ipod_ui_t *ui, uint8_t q)
{
    if (q == ui->last_glow) return;
    ui->last_glow = q;
    if (q == 0) {
        lv_obj_add_flag(ui->wheel_glow, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const lv_img_dsc_t *src = &img_glow_center;
    lv_coord_t gx = WHEEL_CX - 25, gy = WHEEL_CY - 25;
    if (q == 1) { /* Top: MENU */
        src = &img_glow_top;
        gx = WHEEL_CX - 26;
        gy = WHEEL_CY - 69;
    } else if (q == 2) { /* Bottom: >|| */
        src = &img_glow_bot;
        gx = WHEEL_CX - 26;
        gy = WHEEL_CY + 41;
    } else if (q == 3) { /* Left: |<< */
        src = &img_glow_left;
        gx = WHEEL_CX - 69;
        gy = WHEEL_CY - 26;
    } else if (q == 4) { /* Right: >>| */
        src = &img_glow_right;
        gx = WHEEL_CX + 41;
        gy = WHEEL_CY - 26;
    }
    lv_img_set_src(ui->wheel_glow, src);
    lv_obj_set_pos(ui->wheel_glow, gx, gy);
    lv_obj_clear_flag(ui->wheel_glow, LV_OBJ_FLAG_HIDDEN);
}

static uint8_t point_to_quadrant(float dx, float dy, bool in_center)
{
    if (in_center) return 5;
    float ax = dx < 0.0f ? -dx : dx;
    float ay = dy < 0.0f ? -dy : dy;
    if (ay >= ax) return (dy < 0.0f) ? 1 : 2;
    return (dx < 0.0f) ? 3 : 4;
}

/* ---------------------------------------------------------------- Click Wheel Touch Handler */
static void wheel_event_cb(lv_event_t *e)
{
    ipod_ui_t *ui = (ipod_ui_t *)e->user_data;
    if (!ui) return;
    uint8_t code = lv_event_get_code(e);

    void *indev = lv_indev_get_act();
    int16_t px = WHEEL_CX, py = WHEEL_CY;
    if (indev) lv_indev_read_point(indev, &px, &py);

    float dx = (float)(px - WHEEL_CX);
    float dy = (float)(py - WHEEL_CY);
    float r2 = dx * dx + dy * dy;

    if (code == LV_EVENT_PRESSED) {
        ui->wheel_down = true;
        ui->wheel_scrolled = false;
        ui->accum_angle = 0.0f;
        ui->ang_vel = 0.0f;
        ui->prev_tick_ms = lv_tick_get();
        ui->last_step_ms = 0;
        ui->down_x = px;
        ui->down_y = py;
        ui->wheel_in_center = (r2 <= (float)(WHEEL_R_IN * WHEEL_R_IN));
        if (!ui->wheel_in_center) {
            ui->prev_angle = fast_atan2f(dy, dx);
        }
        show_quadrant_glow(ui, point_to_quadrant(dx, dy, ui->wheel_in_center));
    } else if (code == LV_EVENT_PRESSING && ui->wheel_down && !ui->wheel_in_center) {
        if (r2 > (float)((WHEEL_R_IN - 4) * (WHEEL_R_IN - 4))) {
            uint32_t now = lv_tick_get();
            uint32_t dt_ms = now - ui->prev_tick_ms;
            if (dt_ms < 5u) dt_ms = 5u;
            ui->prev_tick_ms = now;

            float ang = fast_atan2f(dy, dx);
            float d = ang - ui->prev_angle;
            if (d > PI_F) d -= TWO_PI_F;
            else if (d < -PI_F) d += TWO_PI_F;
            ui->prev_angle = ang;
            ui->accum_angle += d;

            /* Smooth angular velocity (rad/s) for light, responsive iPod acceleration */
            float inst_vel = (d < 0.0f ? -d : d) * 1000.0f / (float)dt_ms;
            ui->ang_vel = ui->ang_vel * 0.55f + inst_vel * 0.45f;
            float step_rad = STEP_SLOW_RAD;
            bool fast = false;
            if (ui->ang_vel > 5.8f) {
                step_rad = STEP_FAST_RAD;
                fast = true;
            } else if (ui->ang_vel > 2.8f) {
                step_rad = STEP_MED_RAD;
            }

            if (!ui->is_play) {
                /* Pace to at most 1 smooth step per touch tick (>= 22ms) so sliding 3-4 items
                 * glides cleanly and fires 2-4 crisp individual clicks with zero skipping. */
                if ((now - ui->last_step_ms) >= STEP_MIN_MS) {
                    if (ui->accum_angle >= step_rad) {
                        ui->accum_angle -= step_rad;
                        if (ui->accum_angle > step_rad * 1.5f) ui->accum_angle = step_rad * 1.5f;
                        ui->wheel_scrolled = true;
                        ui->last_step_ms = now;
                        show_quadrant_glow(ui, 0);
                        list_step(ui, +1, fast);
                    } else if (ui->accum_angle <= -step_rad) {
                        ui->accum_angle += step_rad;
                        if (ui->accum_angle < -step_rad * 1.5f) ui->accum_angle = -step_rad * 1.5f;
                        ui->wheel_scrolled = true;
                        ui->last_step_ms = now;
                        show_quadrant_glow(ui, 0);
                        list_step(ui, -1, fast);
                    }
                }
            } else {
                if (ui->accum_angle >= 1.10f) {
                    ui->accum_angle = 0.0f;
                    ui->wheel_scrolled = true;
                    show_quadrant_glow(ui, 0);
                    click_trigger();
                    view_send_msg(4, 5, 2, 4); /* Next */
                } else if (ui->accum_angle <= -1.10f) {
                    ui->accum_angle = 0.0f;
                    ui->wheel_scrolled = true;
                    show_quadrant_glow(ui, 0);
                    click_trigger();
                    view_send_msg(4, 5, 0, 4); /* Prev */
                }
            }
        }
    } else if (code == LV_EVENT_RELEASED && ui->wheel_down) {
        ui->wheel_down = false;
        show_quadrant_glow(ui, 0);

        if (ui->wheel_in_center) {
            if (ui->is_play) {
                view_send_msg(4, 1, 0, 0x14); /* Music Settings */
            } else {
                send_msg_after_click_close(ui, ui->page_id, 4, (uint32_t)ui->sel_idx, 4); /* Select item */
            }
        } else if (!ui->wheel_scrolled) {
            float tdx = (float)(ui->down_x - WHEEL_CX);
            float tdy = (float)(ui->down_y - WHEEL_CY);
            float ax = tdx < 0.0f ? -tdx : tdx;
            float ay = tdy < 0.0f ? -tdy : tdy;
            if (ay >= ax) {
                if (tdy < 0.0f) {
                    view_send_msg(ui->page_id, 1, 0, 0x15); /* MENU (Back) */
                } else {
                    if (ui->is_play) {
                        send_msg_after_click_close(ui, 4, 5, 1, 4); /* Now Playing: Play / Pause */
                    } else {
                        /* Browse lists (Songs/Artists/Albums/Menu): play or enter selected item */
                        send_msg_after_click_close(ui, ui->page_id, 4, (uint32_t)ui->sel_idx, 4);
                    }
                }
            } else {
                if (ui->is_play) {
                    if (tdx < 0.0f) send_msg_after_click_close(ui, 4, 5, 0, 4); /* Prev track */
                    else            send_msg_after_click_close(ui, 4, 5, 2, 4); /* Next track */
                } else {
                    /* Browse lists: |<< moves selection UP, >>| moves selection DOWN */
                    list_step(ui, (tdx < 0.0f) ? -1 : +1, false);
                }
            }
        }
    }
}

static void row_tap_cb(lv_event_t *e)
{
    ipod_ui_t *ui = (ipod_ui_t *)e->user_data;
    if (!ui || ui->is_play) return;
    lv_obj_t *t = lv_event_get_target(e);
    for (int i = 0; i < ROW_COUNT; i++) {
        if (t == ui->row_lbl[i]) {
            ui->sel_idx = (int8_t)i;
            update_list_colors(ui, false);
            send_msg_after_click_close(ui, ui->page_id, 4, (uint32_t)i, 4);
            break;
        }
    }
}

/* ---------------------------------------------------------------- 16ms (60 FPS) Animation & Sync Timer */
static void ipod_timer_cb(lv_timer_t *t)
{
    ipod_ui_t *ui = (ipod_ui_t *)t->user_data;
    if (!ui) return;

    /* Fire deferred playback message once ClickTask has closed /dev/audio0 (prevents FLAC race!) */
    if (ui->pending_msg) {
        if (CLICK_SEQ == CLICK_IDLE_MAGIC || (lv_tick_get() - ui->pending_ms) >= 150u) {
            ui->pending_msg = false;
            view_send_msg(ui->pend_page, ui->pend_type, ui->pend_sub, ui->pend_ev);
            return;
        }
    }

    /* Guaranteed 180ms cubic ease-out slide-in completion (protects against stock page-pop lv_anim clears) */
    if (!ui->slide_done) {
        uint32_t dt = lv_tick_get() - ui->open_ms;
        if (dt >= 180u) {
            lv_obj_set_x(ui->lcd_body, 0);
            ui->slide_done = true;
        } else {
            float u = 1.0f - ((float)dt / 180.0f);
            lv_obj_set_x(ui->lcd_body, (lv_coord_t)(120.0f * u * u * u));
        }
    }

    if (ui->is_play) {
        stock_music_play_t *sp = fw_music_play();
        if (!sp) return;

        /* Detect live Play vs Pause state from stock btn_play child label ("\xee\x9a\x8c" = pause) */
        bool paused = ui->is_paused;
        if (sp->btn_play) {
            lv_obj_t *plbl = lv_obj_get_child(sp->btn_play, 0);
            const char *pt = lv_label_get_text_fast(plbl);
            if (pt[0] == (char)0xEE) {
                paused = ((uint8_t)pt[1] == 0x9Au);
            }
        }
        if (!ui->last_paused_known || paused != ui->is_paused) {
            ui->is_paused = paused;
            ui->last_paused_known = true;
            lv_img_set_src(ui->ico_state, paused ? &img_ico_pause : &img_ico_play);
            render_album_sheen(ui);
        }

        /* Advance 60 FPS specular sheen phase while playing */
        if (!ui->is_paused) {
            ui->sheen_phase += 0.045f;
            if (ui->sheen_phase > TWO_PI_F * 4.0f) ui->sheen_phase -= TWO_PI_F * 4.0f;
        }

        /* Sync text metadata every 3rd frame (~20 Hz) to keep 60 FPS frame budget ultra-lean */
        ui->frame_div++;
        if ((ui->frame_div & 3u) == 1u) {
            /* Track counter ("6/15" -> "6 of 15") */
            const char *cnt = lv_label_get_text_fast(sp->track_cnt);
            if (rom_strcmp(cnt, ui->last_count) != 0) {
                char buf[32];
                fmt_track_of(buf, sizeof(buf), cnt);
                lv_label_set_text(ui->lbl_count, buf);
                size_t n = rom_strlen(cnt);
                if (n >= sizeof(ui->last_count)) n = sizeof(ui->last_count) - 1;
                rom_memcpy(ui->last_count, cnt, n);
                ui->last_count[n] = '\0';
            }

            /* Song Title */
            const char *ttl = lv_label_get_text_fast(sp->title);
            if (rom_strcmp(ttl, ui->last_title) != 0) {
                lv_label_set_text(ui->lbl_title, ttl[0] ? ttl : "Now Playing");
                size_t n = rom_strlen(ttl);
                if (n >= sizeof(ui->last_title)) n = sizeof(ui->last_title) - 1;
                rom_memcpy(ui->last_title, ttl, n);
                ui->last_title[n] = '\0';
            }

            /* Artist / Album */
            const char *art = lv_label_get_text_fast(sp->artist);
            if (rom_strcmp(art, ui->last_artist) != 0) {
                lv_label_set_text(ui->lbl_artist, art);
                size_t n = rom_strlen(art);
                if (n >= sizeof(ui->last_artist)) n = sizeof(ui->last_artist) - 1;
                rom_memcpy(ui->last_artist, art, n);
                ui->last_artist[n] = '\0';
            }

            /* Elapsed & Remaining Time ("3:41" & "-1:20") */
            const char *elap = lv_label_get_text_fast(sp->elapsed);
            const char *tot  = lv_label_get_text_fast(sp->total);
            if (rom_strcmp(elap, ui->last_elap) != 0 || rom_strcmp(tot, ui->last_tot) != 0) {
                int es = parse_hms_seconds(elap);
                int ts = parse_hms_seconds(tot);
                char eb[16], rb[16];
                if (es >= 0) {
                    fmt_time(eb, es, false);
                    lv_label_set_text(ui->lbl_elapsed, eb);
                }
                if (es >= 0 && ts >= es) {
                    fmt_time(rb, ts - es, true);
                    lv_label_set_text(ui->lbl_remain, rb);
                } else if (ts >= 0) {
                    fmt_time(rb, ts, true);
                    lv_label_set_text(ui->lbl_remain, rb);
                }
                size_t n1 = rom_strlen(elap);
                if (n1 >= sizeof(ui->last_elap)) n1 = sizeof(ui->last_elap) - 1;
                rom_memcpy(ui->last_elap, elap, n1);
                ui->last_elap[n1] = '\0';
                size_t n2 = rom_strlen(tot);
                if (n2 >= sizeof(ui->last_tot)) n2 = sizeof(ui->last_tot) - 1;
                rom_memcpy(ui->last_tot, tot, n2);
                ui->last_tot[n2] = '\0';
            }
        }

        /* 60 FPS sub-pixel Glossy Aqua Progress Bar */
        int32_t bv = lv_bar_get_val_fast(sp->bar);
        int32_t bm = lv_bar_get_max_fast(sp->bar);
        float target_pct = (bm > 0) ? ((float)bv / (float)bm) : 0.0f;
        if (target_pct < 0.0f) target_pct = 0.0f;
        if (target_pct > 1.0f) target_pct = 1.0f;
        ui->smooth_pct += (target_pct - ui->smooth_pct) * 0.25f;
        render_aqua_bar(ui);

        /* 30 FPS glass sheen pass on 3D-tilted album art when playing */
        if (!ui->is_paused && (ui->frame_div & 1u) == 0u) {
            render_album_sheen(ui);
        }
    } else {
        /* Time-based 60 FPS cubic ease-out glide (115ms) for Aqua highlight pill & scrollbar thumb */
        if (ui->pill_moving) {
            uint32_t dt = lv_tick_get() - ui->pill_start_ms;
            if (dt >= 115u) {
                ui->pill_y = ui->pill_target_y;
                ui->pill_moving = false;
                apply_row_marquee(ui, ui->sel_idx);
            } else {
                float u = 1.0f - ((float)dt / 115.0f);
                float ease = 1.0f - (u * u * u);
                ui->pill_y = ui->pill_start_y + (ui->pill_target_y - ui->pill_start_y) * ease;
            }

            int16_t iy = (int16_t)(ui->pill_y + 0.5f);
            if (iy != ui->last_pill_y) {
                ui->last_pill_y = iy;
                lv_obj_set_y(ui->sel_pill, iy);
            }

            /* Only flip a row's text to white once the blue highlight pill covers >=60% of it */
            int8_t vis = -1;
            float py_rel = ui->pill_y - 2.0f;
            for (int i = 0; i < ROW_COUNT; i++) {
                float d = py_rel - (float)(i * ROW_H);
                if (d < 0.0f) d = -d;
                if (d <= (float)ROW_H * 0.40f) {
                    vis = (int8_t)i;
                    break;
                }
            }
            apply_row_colors(ui, vis);

            if (ui->scroll_thumb) {
                float max_pill = (float)((ROW_COUNT - 1) * ROW_H);
                float frac = (ui->pill_y - 2.0f) / (max_pill > 1.0f ? max_pill : 1.0f);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                int16_t sy = (int16_t)(frac * (float)(SCROLL_H - THUMB_H - 2) + 0.5f);
                if (sy != ui->last_thumb_y) {
                    ui->last_thumb_y = sy;
                    lv_obj_set_y(ui->scroll_thumb, sy);
                }
            }
        }

        /* Sync 6 list row labels cleanly from hidden stock page when not mid-glide */
        ui->frame_div++;
        if (!ui->pill_moving && (ui->frame_div & 3u) == 1u) {
            for (int i = 0; i < ROW_COUNT; i++) {
                const char *txt = lv_label_get_text_fast(get_stock_row_label(ui->page_id, i));
                if (rom_strcmp(txt, ui->last_row[i]) != 0) {
                    char clean[64];
                    size_t j = 0;
                    for (size_t k = 0; txt[k] && j + 1 < sizeof(clean); k++) {
                        char c = txt[k];
                        if (c == '\r' || c == '\n') c = ' ';
                        clean[j++] = c;
                    }
                    clean[j] = '\0';
                    lv_label_set_text(ui->row_lbl[i], clean);
                    bool has_txt = (clean[0] != '\0' && !(clean[0] == ' ' && clean[1] == '\0'));
                    if (has_txt) lv_obj_clear_flag(ui->row_chev[i], LV_OBJ_FLAG_HIDDEN);
                    else         lv_obj_add_flag(ui->row_chev[i], LV_OBJ_FLAG_HIDDEN);
                    size_t n = rom_strlen(txt);
                    if (n >= sizeof(ui->last_row[i])) n = sizeof(ui->last_row[i]) - 1;
                    rom_memcpy(ui->last_row[i], txt, n);
                    ui->last_row[i][n] = '\0';
                }
            }
        }
    }
}

static void ipod_delete_cb(lv_event_t *e)
{
    ipod_ui_t *ui = (ipod_ui_t *)e->user_data;
    if (!ui) return;
    if (ui->lcd_body) lv_anim_del(ui->lcd_body, NULL);
    if (ui->timer) {
        lv_timer_del(ui->timer);
        ui->timer = NULL;
    }
    if (ui->art_buf) {
        lv_mem_free(ui->art_buf);
        ui->art_buf = NULL;
    }
    if (ui->bar_buf) {
        lv_mem_free(ui->bar_buf);
        ui->bar_buf = NULL;
    }
    lv_mem_free(ui);
}

/* ---------------------------------------------------------------- Build Shared iPod Shell */
static void hide_all_children(lv_obj_t *parent)
{
    uint32_t cnt = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *ch = lv_obj_get_child(parent, (int32_t)i);
        if (ch) lv_obj_add_flag(ch, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_ipod_shell(lv_obj_t *stock_container, uint8_t page_id, const char *hdr_title, bool is_play)
{
    if (!stock_container) return;

    ipod_ui_t *ui = (ipod_ui_t *)lv_mem_alloc(sizeof(ipod_ui_t));
    if (!ui) return;
    rom_memset(ui, 0, sizeof(ipod_ui_t));
    ui->page_id = page_id;
    ui->is_play = is_play;
    ui->open_ms = lv_tick_get();
    ui->vis_idx = -1;
    ui->marquee_idx = -1;
    ui->last_pill_y = -1;
    ui->last_thumb_y = -1;

    /* Keep stock widgets alive (so CPU0 updates them) but hidden */
    hide_all_children(stock_container);

    /* 1. Full-screen Anodized Metallic Blue iPod Body (240x320) */
    lv_obj_t *root = lv_obj_create(stock_container);
    ui->root = root;
    style_clean_box(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(root, lv_rgb(0x4A, 0x92, 0xD4), 0);
    lv_obj_set_style_bg_grad_color(root, lv_rgb(0x28, 0x64, 0xA4), 0);
    lv_obj_set_style_bg_grad_dir(root, LV_GRAD_DIR_VER, 0);
    lv_obj_add_event_cb(root, ipod_delete_cb, LV_EVENT_DELETE, ui);

    /* 2. Framed White iPod LCD (228x142 with crisp 2px black bezel) */
    lv_obj_t *lcd = lv_obj_create(root);
    style_clean_box(lcd);
    lv_obj_set_pos(lcd, LCD_X, LCD_Y);
    lv_obj_set_size(lcd, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(lcd, lv_rgb(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_radius(lcd, 6, 0);
    lv_obj_set_style_border_width(lcd, 2, 0);
    lv_obj_set_style_border_color(lcd, lv_rgb(0x10, 0x14, 0x18), 0);

    /* 3. Classic iPod Brushed-Silver Top Header Bar */
    lv_obj_t *hdr = lv_obj_create(lcd);
    style_clean_box(hdr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, BODY_W, HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_rgb(0xF3, 0xF5, 0xF8), 0);
    lv_obj_set_style_bg_grad_color(hdr, lv_rgb(0xC6, 0xCC, 0xD6), 0);
    lv_obj_set_style_bg_grad_dir(hdr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_rgb(0x92, 0x9A, 0xA6), 0);

    /* Left baked INDEXED_4BIT Play/Pause sprite */
    ui->ico_state = lv_img_create(hdr);
    lv_img_set_src(ui->ico_state, &img_ico_play);
    lv_obj_set_pos(ui->ico_state, 6, 4);

    /* Centered header title */
    lv_obj_t *ht = make_label(hdr, hdr_title, lv_rgb(0x14, 0x18, 0x1E), LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(ht, 140);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    /* Right baked INDEXED_4BIT glossy green iPod Battery sprite */
    lv_obj_t *bat = lv_img_create(hdr);
    lv_img_set_src(bat, &img_ico_battery);
    lv_obj_set_pos(bat, BODY_W - 28, 4);

    /* 4. Inner LCD body — animated via LVGL built-in lv_anim horizontal slide-in! */
    lv_obj_t *body = lv_obj_create(lcd);
    ui->lcd_body = body;
    style_clean_box(body);
    lv_obj_set_pos(body, 120, HDR_H);
    lv_obj_set_size(body, BODY_W, BODY_H);
    lv_obj_set_style_bg_color(body, lv_rgb(0xFF, 0xFF, 0xFF), 0);

    lv_anim_t slide;
    lv_anim_init(&slide);
    slide.var = body;
    slide.exec_cb = anim_set_x_cb;
    slide.path_cb = LV_ANIM_PATH_EASE_OUT;
    slide.start_value = 120;
    slide.end_value = 0;
    slide.time = 180;
    lv_anim_start(&slide);

    if (is_play) {
        /* ---- 60 FPS NOW PLAYING SCREEN ---- */
        ui->lbl_count = make_label(body, "1 of 1", lv_rgb(0x28, 0x2E, 0x36), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(ui->lbl_count, 8, 2);

        /* 3D-tilted Album Art + Glossy Floor Reflection (only allocate 5.5KB RAM buffer if LVGL heap has ample headroom) */
        ui->img_art = lv_img_create(body);
        lv_obj_set_pos(ui->img_art, 6, 17);
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        if (mon.free_biggest_size >= 14336u) {
            ui->art_buf = (uint8_t *)lv_mem_alloc(ALBUM_ART_BYTES);
        }
        if (ui->art_buf) {
            rom_memcpy(ui->art_buf, img_album_default_map, ALBUM_ART_BYTES);
            ui->art_dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_8BIT, ALBUM_ART_W, ALBUM_ART_H);
            ui->art_dsc.data_size = ALBUM_ART_BYTES;
            ui->art_dsc.data = ui->art_buf;
            lv_img_set_src(ui->img_art, &ui->art_dsc);
        } else {
            lv_img_set_src(ui->img_art, &img_album_default);
        }

        /* Song Title & Artist on right of 3D album art */
        ui->lbl_title = make_label(body, "Now Playing", lv_rgb(0x10, 0x12, 0x16), LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(ui->lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(ui->lbl_title, 140);
        lv_obj_set_pos(ui->lbl_title, 76, 24);

        ui->lbl_artist = make_label(body, "", lv_rgb(0x40, 0x48, 0x54), LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(ui->lbl_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(ui->lbl_artist, 140);
        lv_obj_set_pos(ui->lbl_artist, 76, 48);

        /* 60 FPS Direct Pixel-Buffer Glossy Aqua Progress Bar (208x11, 3.3KB RAM) */
        ui->img_bar = lv_img_create(body);
        lv_obj_set_pos(ui->img_bar, 8, 91);
        lv_mem_monitor(&mon);
        if (mon.free_biggest_size >= 8192u) {
            ui->bar_buf = (uint8_t *)lv_mem_alloc(BAR_BUF_BYTES);
        }
        if (ui->bar_buf) {
            init_bar_palette(ui->bar_buf);
            ui->bar_dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_8BIT, BAR_W, BAR_H);
            ui->bar_dsc.data_size = BAR_BUF_BYTES;
            ui->bar_dsc.data = ui->bar_buf;
            lv_img_set_src(ui->img_bar, &ui->bar_dsc);
            render_aqua_bar(ui);
        }

        /* Elapsed (left) & Remaining (right) */
        ui->lbl_elapsed = make_label(body, "0:00", lv_rgb(0x18, 0x1C, 0x22), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(ui->lbl_elapsed, 8, 103);

        ui->lbl_remain = make_label(body, "-0:00", lv_rgb(0x18, 0x1C, 0x22), LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(ui->lbl_remain, 80);
        lv_obj_set_pos(ui->lbl_remain, BODY_W - 88, 103);
    } else {
        /* ---- 60 FPS CLASSIC iPOD BROWSE LIST (Music / Songs / Artists / Albums) ---- */
        ui->sel_pill = lv_obj_create(body);
        style_clean_box(ui->sel_pill);
        lv_obj_set_pos(ui->sel_pill, 0, 2);
        lv_obj_set_size(ui->sel_pill, SCROLL_X - 2, ROW_H);
        lv_obj_set_style_bg_color(ui->sel_pill, lv_rgb(0x5C, 0xA9, 0xF8), 0);
        lv_obj_set_style_bg_grad_color(ui->sel_pill, lv_rgb(0x1E, 0x6A, 0xD4), 0);
        lv_obj_set_style_bg_grad_dir(ui->sel_pill, LV_GRAD_DIR_VER, 0);

        /* Classic iPod right-edge recessed scrollbar track & smooth Aqua thumb */
        lv_obj_t *sb_trk = lv_obj_create(body);
        style_clean_box(sb_trk);
        lv_obj_set_pos(sb_trk, SCROLL_X, 2);
        lv_obj_set_size(sb_trk, SCROLL_W, SCROLL_H);
        lv_obj_set_style_bg_color(sb_trk, lv_rgb(0xE8, 0xEC, 0xF2), 0);
        lv_obj_set_style_border_width(sb_trk, 1, 0);
        lv_obj_set_style_border_color(sb_trk, lv_rgb(0xB8, 0xC0, 0xCC), 0);
        lv_obj_set_style_radius(sb_trk, 2, 0);

        ui->scroll_thumb = lv_obj_create(sb_trk);
        style_clean_box(ui->scroll_thumb);
        lv_obj_set_pos(ui->scroll_thumb, 0, 0);
        lv_obj_set_size(ui->scroll_thumb, SCROLL_W - 2, THUMB_H);
        lv_obj_set_style_radius(ui->scroll_thumb, 2, 0);
        lv_obj_set_style_bg_color(ui->scroll_thumb, lv_rgb(0x5A, 0xA6, 0xF4), 0);
        lv_obj_set_style_bg_grad_color(ui->scroll_thumb, lv_rgb(0x28, 0x72, 0xD6), 0);
        lv_obj_set_style_bg_grad_dir(ui->scroll_thumb, LV_GRAD_DIR_HOR, 0);

        for (int i = 0; i < ROW_COUNT; i++) {
            lv_obj_t *lbl = make_label(body, "", lv_rgb(0x14, 0x18, 0x1F), LV_TEXT_ALIGN_LEFT);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            /* Width-only (LV_SIZE_CONTENT height) prevents early truncation AND guarantees
             * LV_LABEL_LONG_SCROLL_CIRCULAR always scrolls horizontally (never vertically). */
            lv_obj_set_width(lbl, SCROLL_X - 20);
            lv_obj_set_pos(lbl, 6, (lv_coord_t)(i * ROW_H + 3));
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(lbl, row_tap_cb, LV_EVENT_CLICKED, ui);
            ui->row_lbl[i] = lbl;

            lv_obj_t *ch = make_label(body, ">", lv_rgb(0x9A, 0xA4, 0xB2), LV_TEXT_ALIGN_RIGHT);
            lv_obj_set_pos(ch, SCROLL_X - 13, (lv_coord_t)(i * ROW_H + 3));
            ui->row_chev[i] = ch;
        }
        update_list_colors(ui, true);
    }

    /* 5. Iconic White & Blue Click Wheel (156x156 pre-rendered anti-aliased) */
    lv_obj_t *wheel = lv_img_create(root);
    lv_img_set_src(wheel, &img_clickwheel);
    lv_obj_set_pos(wheel, WHEEL_X, WHEEL_Y);

    /* Baked INDEXED_4BIT Click Wheel quadrant tactile press glow (55% opacity) */
    ui->wheel_glow = lv_img_create(root);
    lv_img_set_src(ui->wheel_glow, &img_glow_center);
    lv_obj_set_style_opa(ui->wheel_glow, 140, 0);
    lv_obj_add_flag(ui->wheel_glow, LV_OBJ_FLAG_HIDDEN);

    /* Full-bottom invisible touch catcher over Click Wheel */
    lv_obj_t *touch = lv_obj_create(root);
    style_clean_box(touch);
    lv_obj_set_pos(touch, 0, LCD_Y + LCD_H);
    lv_obj_set_size(touch, SCR_W, SCR_H - (LCD_Y + LCD_H));
    lv_obj_set_style_bg_opa(touch, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch, wheel_event_cb, LV_EVENT_ALL, ui);

    /* 6. Start 10ms (~100 Hz) browse timer / 16ms (60 FPS) Now Playing timer */
    ui->timer = lv_timer_create(ipod_timer_cb, is_play ? 16 : 10, ui);
}

/* ---------------------------------------------------------------- Hooked Page Constructors */
__attribute__((used)) int hook_music_menu_create(void)
{
    int ret = stock_page_music_menu_create();
    if (ret == 0 && fw_music_menu()) {
        build_ipod_shell(lv_disp_get_scr_act(NULL), 2, "Music", false);
    }
    return ret;
}

__attribute__((used)) int hook_music_song_create(void)
{
    int ret = stock_page_music_song_create();
    if (ret == 0 && fw_music_song()) {
        build_ipod_shell(lv_disp_get_scr_act(NULL), 3, "Songs", false);
    }
    return ret;
}

__attribute__((used)) int hook_music_play_create(void)
{
    int ret = stock_page_music_play_create();
    if (ret == 0 && fw_music_play()) {
        build_ipod_shell(lv_disp_get_scr_act(NULL), 4, "Now Playing", true);
    }
    return ret;
}

__attribute__((used)) int hook_music_artist_create(void)
{
    int ret = stock_page_music_artist_create();
    if (ret == 0 && fw_music_artist()) {
        build_ipod_shell(lv_disp_get_scr_act(NULL), 6, "Artists", false);
    }
    return ret;
}

__attribute__((used)) int hook_music_album_create(void)
{
    int ret = stock_page_music_album_create();
    if (ret == 0 && fw_music_album()) {
        build_ipod_shell(lv_disp_get_scr_act(NULL), 7, "Albums", false);
    }
    return ret;
}

/* ---------------------------------------------------------------- CPU0 Click Wheel Sound Task */
static bool dev_is_busy(const char *name)
{
    uint8_t *head = (uint8_t *)FW_DEV_LIST;
    uint8_t *cur = *(uint8_t **)head;
    int guard = 0;
    while (cur && cur != head && guard++ < 64) {
        uint8_t *node = cur - 0x14;
        const char *n = *(const char **)node;
        if (n && rom_strcmp(n, name) == 0) {
            return (node[0x10] & 1u) != 0;
        }
        cur = *(uint8_t **)cur;
    }
    return false;
}

static void click_audio_task(void *arg)
{
    (void)arg;
    int16_t buf[256 * 2] __attribute__((aligned(4)));
    uint32_t last_seq = CLICK_SEQ;
    /* 44100 Hz, 2ch, 16-bit, 256 frames/period, 8 periods (~46ms ring), speaker amp callback */
    static const uint32_t params[10] = {
        2, 2, 44100, 256, 8, 0, 0, 0, FW_AUDIO_PA_CB, 0
    };

    for (;;) {
        uint32_t seq = CLICK_SEQ;
        if (seq == CLICK_STOP_MAGIC) {
            CLICK_SEQ = CLICK_IDLE_MAGIC;
            last_seq = CLICK_IDLE_MAGIC;
            /* Brief cooldown so FLAC/MP3 init can open /dev/audio0 without contention */
            fw_os_sleep(350);
            continue;
        }
        if (seq == last_seq || seq == CLICK_IDLE_MAGIC || seq == 0u) {
            last_seq = seq;
            fw_os_sleep(8);
            continue;
        }
        last_seq = seq;

        /* Never interrupt active music playback */
        if (dev_is_busy("/dev/audio0")) {
            continue;
        }

        void *dev = fw_dev_open("/dev/audio0", 0, params);
        if (!dev) continue;

        /* 4 silent pre-roll periods (~23ms) so Class-D speaker amp wakes cleanly & responsively */
        rom_memset(buf, 0, sizeof(buf));
        for (int pre = 0; pre < 4; pre++) {
            fw_dev_write(dev, buf, 256, 0);
        }

        /* Keep /dev/audio0 open ~1.5s (260 periods) while spinning to prevent mid-spin amp pops!
         * Exits immediately if CLICK_STOP_MAGIC is signaled (user pressed Center/Play). */
        int pos = 0;
        int idle_periods = 0;
        bool urgent_stop = false;
        while (idle_periods < 260) {
            uint32_t cur = CLICK_SEQ;
            if (cur == CLICK_STOP_MAGIC) {
                urgent_stop = true;
                break;
            }
            if (cur != last_seq && cur != CLICK_IDLE_MAGIC) {
                last_seq = cur;
                pos = 0;
                idle_periods = 0;
            }
            if (pos < CLICK_PCM_LEN) {
                rom_memcpy(buf, &click_pcm[pos], 256 * sizeof(int16_t));
                rom_pcm_mono_to_stereo(buf, buf, 256);
                pos += 256;
            } else {
                rom_memset(buf, 0, sizeof(buf));
                idle_periods++;
            }
            fw_dev_write(dev, buf, 256, 0);
        }

        /* Drain ring and close cleanly; on urgent_stop close ASAP so FLAC never races /dev/audio0 */
        if (!urgent_stop) {
            rom_memset(buf, 0, sizeof(buf));
            for (int d = 0; d < 6; d++) fw_dev_write(dev, buf, 256, 0);
            fw_os_sleep(30);
        } else {
            fw_os_sleep(24);
        }
        fw_dev_close(&dev);
        CLICK_SEQ = CLICK_IDLE_MAGIC;
        last_seq = CLICK_IDLE_MAGIC;
        if (urgent_stop) {
            /* Give CPU0 FLAC/MP3 decoder time to parse headers & claim /dev/audio0 */
            fw_os_sleep(350);
        }
    }
}

/* ---------------------------------------------------------------- On-Device Screenshot & Recorder */
enum {
    SHOT_IDLE = 0,
    SHOT_ARMED = 1,
    SHOT_CAPTURING = 2,
    SHOT_FINISHING = 3,
};

#define SHOT_BUF_ROWS 8
#define SHOT_ROW_BYTES (SCR_W * 3u) /* 720 bytes (multiple of 4, no BMP padding) */
#define SHOT_REC_MAX_FRAMES 200u

typedef struct {
    volatile uint32_t state;
    volatile uint32_t strip_ready;
    volatile uint32_t req_shot;
    volatile uint32_t recording;
    volatile int16_t  x1, y1, x2, y2;
    const uint16_t *volatile pixels;

    /* Key combo tracking (CPU0 key callback) */
    uint8_t  last_kid;
    uint32_t last_key_ms;
    uint32_t last_pair_ms;

    /* Capture bookkeeping (CPU0 ShotTask) */
    uint32_t next_idx;
    uint32_t rec_frames;
    bool     dir_ready;

    fw_file_t     file;
    fw_fileinfo_t fno;
    uint8_t       bgr_buf[SHOT_BUF_ROWS * SHOT_ROW_BYTES];
} shot_ctx_t;

static inline shot_ctx_t *get_shot_ctx(void)
{
    uint32_t raw = SHOT_CTX_RAW;
    return fw_is_ram((void *)(uintptr_t)raw) ? (shot_ctx_t *)(uintptr_t)raw : NULL;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void build_bmp_header(uint8_t hdr[54])
{
    const uint32_t img_bytes = (uint32_t)SCR_W * (uint32_t)SCR_H * 3u;
    const uint32_t file_bytes = 54u + img_bytes;
    rom_memset(hdr, 0, 54u);
    hdr[0] = 'B';
    hdr[1] = 'M';
    put_le32(hdr + 2, file_bytes);
    put_le32(hdr + 10, 54u);               /* pixel data offset */
    put_le32(hdr + 14, 40u);               /* BITMAPINFOHEADER size */
    put_le32(hdr + 18, (uint32_t)SCR_W);   /* width = 240 */
    put_le32(hdr + 22, (uint32_t)(-SCR_H));/* top-down height = -320 */
    put_le16(hdr + 26, 1u);                /* planes */
    put_le16(hdr + 28, 24u);               /* 24-bit BGR */
    put_le32(hdr + 30, 0u);                /* BI_RGB (uncompressed) */
    put_le32(hdr + 34, img_bytes);
    put_le32(hdr + 38, 2835u);             /* 72 DPI */
    put_le32(hdr + 42, 2835u);
}

static void fmt_shot_path(char *dst, const char *prefix, uint32_t idx)
{
    /* "0:/shots/" + prefix + 4-digit index + ".bmp" */
    static const char dir[] = "0:/shots/";
    size_t p = 0;
    for (size_t i = 0; dir[i]; i++) dst[p++] = dir[i];
    for (size_t i = 0; prefix[i]; i++) dst[p++] = prefix[i];
    fmt_u32(dst + p, idx, 4);
    p += rom_strlen(dst + p);
    dst[p++] = '.';
    dst[p++] = 'b';
    dst[p++] = 'm';
    dst[p++] = 'p';
    dst[p] = '\0';
}

static void rgb565_swap_to_bgr24(uint8_t *dst, const uint16_t *src, int count)
{
    for (int i = 0; i < count; i++) {
        uint16_t raw = src[i];
        uint16_t v = (uint16_t)((raw >> 8) | (raw << 8));
        uint8_t r = (uint8_t)((v >> 8) & 0xF8u);
        uint8_t g = (uint8_t)((v >> 3) & 0xFCu);
        uint8_t b = (uint8_t)((v << 3) & 0xF8u);
        r |= (uint8_t)(r >> 5);
        g |= (uint8_t)(g >> 6);
        b |= (uint8_t)(b >> 5);
        dst[i * 3 + 0] = b;
        dst[i * 3 + 1] = g;
        dst[i * 3 + 2] = r;
    }
}

static void shot_write_strip(shot_ctx_t *ctx)
{
    int x1 = ctx->x1, y1 = ctx->y1, x2 = ctx->x2, y2 = ctx->y2;
    const uint16_t *px = ctx->pixels;
    if (!px || x1 > x2 || y1 > y2) return;
    int src_w = x2 - x1 + 1;
    if (x2 < 0 || y2 < 0 || x1 >= SCR_W || y1 >= SCR_H) return;

    int cx1 = x1 < 0 ? 0 : x1;
    int cy1 = y1 < 0 ? 0 : y1;
    int cx2 = x2 >= SCR_W ? SCR_W - 1 : x2;
    int cy2 = y2 >= SCR_H ? SCR_H - 1 : y2;
    int cw = cx2 - cx1 + 1;
    uint32_t bw = 0;

    if (cx1 == 0 && cx2 == SCR_W - 1 && src_w == SCR_W) {
        /* Fast path: full-width horizontal band */
        if (f_lseek(&ctx->file, 54u + (uint64_t)cy1 * SHOT_ROW_BYTES) != 0) return;
        int y = cy1;
        while (y <= cy2) {
            int batch = cy2 - y + 1;
            if (batch > SHOT_BUF_ROWS) batch = SHOT_BUF_ROWS;
            const uint16_t *src = px + (y - y1) * src_w;
            rgb565_swap_to_bgr24(ctx->bgr_buf, src, batch * SCR_W);
            if (f_write(&ctx->file, ctx->bgr_buf, (uint32_t)batch * SHOT_ROW_BYTES, &bw) != 0) return;
            y += batch;
        }
    } else {
        /* Partial rect fallback: write clipped row slices at exact BMP offsets */
        for (int y = cy1; y <= cy2; y++) {
            uint64_t ofs = 54u + (uint64_t)y * SHOT_ROW_BYTES + (uint64_t)cx1 * 3u;
            if (f_lseek(&ctx->file, ofs) != 0) return;
            const uint16_t *src = px + (y - y1) * src_w + (cx1 - x1);
            rgb565_swap_to_bgr24(ctx->bgr_buf, src, cw);
            if (f_write(&ctx->file, ctx->bgr_buf, (uint32_t)cw * 3u, &bw) != 0) return;
        }
    }
}

static void shot_task(void *arg)
{
    shot_ctx_t *ctx = (shot_ctx_t *)arg;
    uint8_t bmp_hdr[54];
    char path[36];
    build_bmp_header(bmp_hdr);

    for (;;) {
        if (!ctx->req_shot && !ctx->recording) {
            fw_os_sleep(20);
            continue;
        }
        bool is_rec = (ctx->recording != 0u);
        ctx->req_shot = 0u;

        if (!fw_sd_mounted() || FW_HEAP_FREE < 0x4000u) {
            ctx->recording = 0u;
            fw_os_sleep(200);
            continue;
        }

        if (!ctx->dir_ready) {
            f_mkdir("0:/shots");
            ctx->dir_ready = true;
            if (ctx->next_idx == 0u) ctx->next_idx = 1u;
        }

        /* Find next unused filename so previous captures are never overwritten */
        const char *pfx = is_rec ? "rec_" : "shot_";
        while (ctx->next_idx < 9999u) {
            fmt_shot_path(path, pfx, ctx->next_idx);
            if (f_stat(path, &ctx->fno) != 0) break;
            ctx->next_idx++;
        }

        if (f_open(&ctx->file, path, FA_WRITE | FA_CREATE_ALWAYS) != 0) {
            ctx->recording = 0u;
            ctx->dir_ready = false;
            fw_os_sleep(200);
            continue;
        }

        uint32_t bw = 0;
        if (f_write(&ctx->file, bmp_hdr, sizeof(bmp_hdr), &bw) != 0 || bw != sizeof(bmp_hdr)) {
            f_close(&ctx->file);
            ctx->recording = 0u;
            continue;
        }

        /* Arm CPU1 to force a full-screen redraw and stream every flushed strip */
        ctx->strip_ready = 0u;
        __sync_synchronize();
        ctx->state = SHOT_ARMED;

        uint32_t t0 = rom_tick_count();
        while ((rom_tick_count() - t0) < 2500u) {
            if (ctx->strip_ready) {
                __sync_synchronize();
                shot_write_strip(ctx);
                __sync_synchronize();
                ctx->strip_ready = 0u;
            } else if (ctx->state == SHOT_FINISHING) {
                break;
            } else {
                fw_os_sleep(1);
            }
        }

        ctx->state = SHOT_IDLE;
        ctx->strip_ready = 0u;
        __sync_synchronize();
        f_close(&ctx->file);
        ctx->next_idx++;

        if (is_rec) {
            ctx->rec_frames++;
            if (ctx->rec_frames >= SHOT_REC_MAX_FRAMES) {
                ctx->recording = 0u;
                ctx->rec_frames = 0u;
            } else {
                /* Small breather so UI touch & audio stay responsive (~3-5 FPS) */
                fw_os_sleep(40);
            }
        } else {
            /* Subtle shutter-click audio confirmation (only if music isn't playing) */
            if (!dev_is_busy("/dev/audio0")) {
                uint32_t s = CLICK_SEQ;
                CLICK_SEQ = (s >= 0xFFFFFFF0u) ? 1u : (s + 1u);
            }
            fw_os_sleep(100);
        }
    }
}

/* CPU0 key callback hook:
 *   - Quick Vol+ then Vol- (or Vol- then Vol+) within 450ms -> single screenshot (or stop recording)
 *   - 3rd quick alternating tap within 600ms -> start continuous recording (~3-5 FPS) */
__attribute__((used)) void hook_key_cb(uint32_t *key_val)
{
    shot_ctx_t *ctx = get_shot_ctx();
    if (ctx && key_val) {
        uint32_t raw = *key_val & 0x7FFFFFFFu;
        if (raw > 4u) {
            uint8_t kid = (uint8_t)(raw & 0xFFu);
            uint8_t kev = (uint8_t)((raw >> 16) & 0xFFu);
            if (kev == 0x10u) {
                uint32_t now = rom_tick_count();
                if (ctx->last_pair_ms != 0u && (now - ctx->last_pair_ms) <= 600u && kid != ctx->last_kid) {
                    /* 3rd alternating tap -> start continuous recording */
                    ctx->recording = 1u;
                    ctx->rec_frames = 0u;
                    ctx->last_pair_ms = 0u;
                    ctx->last_kid = 0u;
                } else if (ctx->last_kid != 0u && kid != ctx->last_kid && (now - ctx->last_key_ms) <= 450u) {
                    /* 2nd alternating tap -> stop recording if active, else take 1 screenshot */
                    if (ctx->recording) {
                        ctx->recording = 0u;
                        ctx->last_pair_ms = 0u;
                    } else {
                        ctx->req_shot = 1u;
                        ctx->last_pair_ms = now;
                    }
                    ctx->last_kid = kid;
                    ctx->last_key_ms = now;
                } else {
                    ctx->last_kid = kid;
                    ctx->last_key_ms = now;
                }
            }
        }
    }
    stock_key_cb(key_val);
}

/* CPU1 LVGL timer handler hook: triggers full-screen redraw when armed */
__attribute__((used)) uint32_t hook_lv_timer_handler(void)
{
    shot_ctx_t *ctx = get_shot_ctx();
    if (ctx && ctx->state == SHOT_ARMED) {
        ctx->state = SHOT_CAPTURING;
        __sync_synchronize();
        lv_obj_t *scr = lv_disp_get_scr_act(NULL);
        if (scr) lv_obj_invalidate(scr);
        lv_obj_t *top = lv_disp_get_layer_top(NULL);
        if (top) lv_obj_invalidate(top);
        uint32_t ret = stock_lv_timer_handler();
        __sync_synchronize();
        ctx->state = SHOT_FINISHING;
        return ret;
    }
    return stock_lv_timer_handler();
}

/* CPU1 LVGL flush_cb hook: hands each RGB565 strip to CPU0 ShotTask during capture */
__attribute__((used)) void hook_disp_flush(void *drv, const lv_area_t *area, const uint16_t *color_p)
{
    shot_ctx_t *ctx = get_shot_ctx();
    if (ctx && ctx->state == SHOT_CAPTURING && area && color_p) {
        ctx->x1 = area->x1;
        ctx->y1 = area->y1;
        ctx->x2 = area->x2;
        ctx->y2 = area->y2;
        ctx->pixels = color_p;
        __sync_synchronize();
        ctx->strip_ready = 1u;
        uint32_t t0 = lv_tick_get();
        while (ctx->strip_ready && (lv_tick_get() - t0) < 600u) {
            __asm volatile("nop");
        }
    }
    stock_disp_flush(drv, area, color_p);
}

__attribute__((used)) void hook_cpu0_start(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    fw_watchdog_setup(a, b, c, d);
    CLICK_SEQ = CLICK_IDLE_MAGIC;
    SHOT_CTX_RAW = 0u;
    /* 0x280 words (2.5 KB) stack leaves extra CPU0 FreeRTOS heap headroom for FLAC decoding */
    fw_task_create(click_audio_task, NULL, "ClickTask", 6, 0x280);
    shot_ctx_t *sctx = (shot_ctx_t *)fw_os_calloc(1u, sizeof(shot_ctx_t));
    if (fw_is_ram(sctx)) {
        SHOT_CTX_RAW = (uint32_t)(uintptr_t)sctx;
        fw_task_create(shot_task, sctx, "ShotTask", 5, 0x300);
    }
}
