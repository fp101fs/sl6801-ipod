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
#define STEP_MIN_MS   22u
#define CLICK_STOP_MAGIC 0xFFFFFFFFu

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
    lv_obj_t *sel_pill;     /* 60 FPS spring-animated Aqua blue highlight bar */
    lv_obj_t *scroll_thumb; /* classic iPod right-edge scrollbar thumb */
    lv_obj_t *row_lbl[ROW_COUNT];
    lv_obj_t *row_chev[ROW_COUNT];
    int8_t sel_idx;
    int8_t vis_idx;         /* row currently covered >=60% by sel_pill (-1 while transitioning) */
    float pill_y;           /* spring-interpolated Y coordinate */
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

static void apply_row_colors(ipod_ui_t *ui, int8_t vis)
{
    if (vis == ui->vis_idx) return;
    ui->vis_idx = vis;
    lv_color_t white = lv_rgb(0xFF, 0xFF, 0xFF);
    lv_color_t dark  = lv_rgb(0x14, 0x18, 0x1F);
    lv_color_t gray  = lv_rgb(0x9A, 0xA4, 0xB2);
    for (int i = 0; i < ROW_COUNT; i++) {
        bool sel = (i == vis);
        lv_obj_set_style_text_color(ui->row_lbl[i], sel ? white : dark, 0);
        lv_obj_set_style_text_color(ui->row_chev[i], sel ? white : gray, 0);
        lv_label_set_long_mode(ui->row_lbl[i], sel ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
    }
}

static void update_list_colors(ipod_ui_t *ui, bool snap_pill)
{
    if (snap_pill) {
        int16_t ty = (int16_t)(ui->sel_idx * ROW_H + 2);
        ui->pill_y = (float)ty;
        ui->last_pill_y = ty;
        lv_obj_set_y(ui->sel_pill, ty);
        if (ui->scroll_thumb) {
            int16_t sy = (int16_t)((ui->sel_idx * (SCROLL_H - THUMB_H - 2)) / (ROW_COUNT - 1));
            ui->last_thumb_y = sy;
            lv_obj_set_y(ui->scroll_thumb, sy);
        }
        apply_row_colors(ui, ui->sel_idx);
    }
}

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
    if (CLICK_SEQ == CLICK_STOP_MAGIC) CLICK_SEQ = 1;
    else CLICK_SEQ++;
    update_list_colors(ui, flipped);
}

/* ---------------------------------------------------------------- Click Wheel Tactile Glow */
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
                    if (CLICK_SEQ == CLICK_STOP_MAGIC) CLICK_SEQ = 1;
                    else CLICK_SEQ++;
                    view_send_msg(4, 5, 2, 4); /* Next */
                } else if (ui->accum_angle <= -1.10f) {
                    ui->accum_angle = 0.0f;
                    ui->wheel_scrolled = true;
                    show_quadrant_glow(ui, 0);
                    if (CLICK_SEQ == CLICK_STOP_MAGIC) CLICK_SEQ = 1;
                    else CLICK_SEQ++;
                    view_send_msg(4, 5, 0, 4); /* Prev */
                }
            }
        }
    } else if (code == LV_EVENT_RELEASED && ui->wheel_down) {
        ui->wheel_down = false;
        show_quadrant_glow(ui, 0);

        if (ui->wheel_in_center) {
            CLICK_SEQ = CLICK_STOP_MAGIC; /* release /dev/audio0 immediately before playing */
            if (ui->is_play) {
                view_send_msg(4, 1, 0, 0x14); /* Music Settings */
            } else {
                view_send_msg(ui->page_id, 4, (uint32_t)ui->sel_idx, 4); /* Select item */
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
                    CLICK_SEQ = CLICK_STOP_MAGIC;
                    if (ui->is_play) {
                        view_send_msg(4, 5, 1, 4); /* Now Playing: Play / Pause */
                    } else {
                        /* Browse lists (Songs/Artists/Albums/Menu): play or enter selected item */
                        view_send_msg(ui->page_id, 4, (uint32_t)ui->sel_idx, 4);
                    }
                }
            } else {
                if (ui->is_play) {
                    CLICK_SEQ = CLICK_STOP_MAGIC;
                    if (tdx < 0.0f) view_send_msg(4, 5, 0, 4); /* Prev track */
                    else            view_send_msg(4, 5, 2, 4); /* Next track */
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
            CLICK_SEQ = CLICK_STOP_MAGIC;
            view_send_msg(ui->page_id, 4, (uint32_t)i, 4);
            break;
        }
    }
}

/* ---------------------------------------------------------------- 16ms (60 FPS) Animation & Sync Timer */
static void ipod_timer_cb(lv_timer_t *t)
{
    ipod_ui_t *ui = (ipod_ui_t *)t->user_data;
    if (!ui) return;

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
        /* 60 FPS critically damped spring glide for Aqua highlight pill & scrollbar thumb */
        float target_y = (float)(ui->sel_idx * ROW_H + 2);
        float diff = target_y - ui->pill_y;
        if (diff > 0.4f || diff < -0.4f) {
            ui->pill_y += diff * 0.44f;
        } else {
            ui->pill_y = target_y;
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

        /* Sync 6 list row labels cleanly from hidden stock page (strictly single-line for horizontal marquee) */
        ui->frame_div++;
        if ((ui->frame_div & 1u) == 1u) {
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

        /* 3D-tilted Album Art + Glossy Floor Reflection (62x72 direct RAM buffer w/ sheen) */
        ui->img_art = lv_img_create(body);
        lv_obj_set_pos(ui->img_art, 6, 17);
        ui->art_buf = (uint8_t *)lv_mem_alloc(ALBUM_ART_BYTES);
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

        /* 60 FPS Direct Pixel-Buffer Glossy Aqua Progress Bar (208x11) */
        ui->img_bar = lv_img_create(body);
        lv_obj_set_pos(ui->img_bar, 8, 91);
        ui->bar_buf = (uint8_t *)lv_mem_alloc(BAR_BUF_BYTES);
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

    /* 6. Start 16ms (60 FPS) animation & live data sync timer */
    ui->timer = lv_timer_create(ipod_timer_cb, 16, ui);
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
        if (seq == last_seq || seq == CLICK_STOP_MAGIC) {
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
        while (idle_periods < 260) {
            uint32_t cur = CLICK_SEQ;
            if (cur == CLICK_STOP_MAGIC) {
                last_seq = cur;
                break;
            }
            if (cur != last_seq) {
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

        /* Drain ring and close cleanly */
        rom_memset(buf, 0, sizeof(buf));
        for (int d = 0; d < 8; d++) fw_dev_write(dev, buf, 256, 0);
        fw_os_sleep(50);
        fw_dev_close(&dev);
    }
}

__attribute__((used)) void hook_cpu0_start(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    fw_watchdog_setup(a, b, c, d);
    CLICK_SEQ = 0;
    fw_task_create(click_audio_task, NULL, "ClickTask", 6, 0x400);
}
