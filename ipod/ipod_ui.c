/*
 * Blue iPod Classic UI + Polar-Angle Touch Click Wheel for Smartlink SL6801 (240x320).
 * Replaces Music Menu (page 2), Songs (page 3), Now Playing (page 4),
 * Artists (page 6), and Albums (page 7) while keeping CPU0 audio/BT/DB 100% intact.
 *
 * Rules: runs on CPU1 (LVGL UI thread); zero writable globals; all state in lv_mem_alloc.
 */
#include "fw_240x320.h"
#include "gen/clickwheel.h"
#include "gen/album_default.h"
#include "gen/click_pcm.h"

/* ---------------------------------------------------------------- Geometry */
#define SCR_W 240
#define SCR_H 320

#define LCD_X 6
#define LCD_Y 4
#define LCD_W 228
#define LCD_H 142
#define HDR_H 20

#define WHEEL_SIZE 156
#define WHEEL_X    ((SCR_W - WHEEL_SIZE) / 2) /* 42 */
#define WHEEL_Y    156
#define WHEEL_CX   (WHEEL_X + WHEEL_SIZE / 2) /* 120 */
#define WHEEL_CY   (WHEEL_Y + WHEEL_SIZE / 2) /* 234 */
#define WHEEL_R_IN  27
#define WHEEL_R_OUT 82

#define ROW_COUNT 6
#define ROW_H     20
#define BAR_W     208

#define PI_F      3.14159265f
#define TWO_PI_F  6.28318530f
#define STEP_RAD  0.46f /* ~26 degrees per Click Wheel tick */

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

/* ---------------------------------------------------------------- Style helpers */
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

/* ---------------------------------------------------------------- UI State (in LVGL heap) */
typedef struct {
    uint8_t page_id; /* 2=Menu, 3=Songs, 4=NowPlaying, 6=Artists, 7=Albums */
    bool is_play;

    lv_obj_t *root;      /* 240x320 blue metallic shell */
    lv_obj_t *lcd_body;  /* animated content pane inside LCD */
    lv_obj_t *wheel_hl;  /* subtle touch-feedback highlight dot on wheel */
    lv_timer_t *timer;

    /* Slide-in animation state */
    uint32_t anim_start_ms;
    bool anim_done;

    /* Click wheel polar touch state */
    bool wheel_down;
    bool wheel_in_center;
    bool wheel_scrolled;
    float prev_angle;
    float accum_angle;
    int16_t down_x;
    int16_t down_y;

    /* Now Playing widgets */
    lv_obj_t *lbl_count;
    lv_obj_t *lbl_title;
    lv_obj_t *lbl_artist;
    lv_obj_t *lbl_elapsed;
    lv_obj_t *lbl_remain;
    lv_obj_t *bar_fill;
    lv_obj_t *lbl_play_ind;
    float smooth_pct;
    char last_count[24];
    char last_title[80];
    char last_artist[80];
    char last_elap[16];
    char last_tot[16];

    /* List widgets (Music Menu / Songs / Artists / Albums) */
    lv_obj_t *sel_pill;          /* smooth-gliding Aqua blue highlight bar */
    lv_obj_t *row_lbl[ROW_COUNT];
    lv_obj_t *row_chev[ROW_COUNT];
    int8_t sel_idx;
    float sel_y_cur;
    char last_row[ROW_COUNT][64];
} ipod_ui_t;

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

static void update_list_colors(ipod_ui_t *ui)
{
    lv_color_t white = lv_rgb(0xFF, 0xFF, 0xFF);
    lv_color_t dark  = lv_rgb(0x14, 0x18, 0x1F);
    lv_color_t gray  = lv_rgb(0x9A, 0xA4, 0xB2);
    for (int i = 0; i < ROW_COUNT; i++) {
        bool sel = (i == ui->sel_idx);
        lv_obj_set_style_text_color(ui->row_lbl[i], sel ? white : dark, 0);
        lv_obj_set_style_text_color(ui->row_chev[i], sel ? white : gray, 0);
    }
}

static void list_step(ipod_ui_t *ui, int dir)
{
    int n = count_valid_rows(ui->page_id);
    int next = ui->sel_idx + dir;
    if (next >= n) {
        if (ui->page_id != 2) {
            /* Next page in Songs / Artists / Albums */
            view_send_msg(ui->page_id, 6, 0, 0x16);
            ui->sel_idx = 0;
        } else {
            ui->sel_idx = (int8_t)(n - 1);
        }
    } else if (next < 0) {
        if (ui->page_id != 2) {
            /* Prev page in Songs / Artists / Albums */
            view_send_msg(ui->page_id, 6, 0, 0x17);
            ui->sel_idx = (int8_t)(ROW_COUNT - 1);
        } else {
            ui->sel_idx = 0;
        }
    } else {
        ui->sel_idx = (int8_t)next;
    }
    CLICK_SEQ++;
    update_list_colors(ui);
}

/* ---------------------------------------------------------------- Click Wheel Touch Handler */
static void show_wheel_feedback(ipod_ui_t *ui, int16_t cx, int16_t cy, int16_t sz)
{
    lv_obj_set_size(ui->wheel_hl, sz, sz);
    lv_obj_set_style_radius(ui->wheel_hl, sz / 2, 0);
    lv_obj_set_pos(ui->wheel_hl, cx - sz / 2, cy - sz / 2);
    lv_obj_clear_flag(ui->wheel_hl, LV_OBJ_FLAG_HIDDEN);
}

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
        ui->down_x = px;
        ui->down_y = py;
        ui->wheel_in_center = (r2 <= (float)(WHEEL_R_IN * WHEEL_R_IN));
        if (ui->wheel_in_center) {
            show_wheel_feedback(ui, WHEEL_CX, WHEEL_CY, 46);
        } else {
            ui->prev_angle = fast_atan2f(dy, dx);
            show_wheel_feedback(ui, px, py, 28);
        }
    } else if (code == LV_EVENT_PRESSING && ui->wheel_down && !ui->wheel_in_center) {
        if (r2 > (float)((WHEEL_R_IN - 4) * (WHEEL_R_IN - 4))) {
            float ang = fast_atan2f(dy, dx);
            float d = ang - ui->prev_angle;
            if (d > PI_F) d -= TWO_PI_F;
            else if (d < -PI_F) d += TWO_PI_F;
            ui->prev_angle = ang;
            ui->accum_angle += d;
            show_wheel_feedback(ui, px, py, 28);

            if (!ui->is_play) {
                while (ui->accum_angle >= STEP_RAD) {
                    ui->accum_angle -= STEP_RAD;
                    ui->wheel_scrolled = true;
                    list_step(ui, +1);
                }
                while (ui->accum_angle <= -STEP_RAD) {
                    ui->accum_angle += STEP_RAD;
                    ui->wheel_scrolled = true;
                    list_step(ui, -1);
                }
            } else {
                /* On Now Playing: 65-degree wheel twist skips Next/Prev track */
                if (ui->accum_angle >= 1.15f) {
                    ui->accum_angle = 0.0f;
                    ui->wheel_scrolled = true;
                    CLICK_SEQ++;
                    view_send_msg(4, 5, 2, 4); /* Next */
                } else if (ui->accum_angle <= -1.15f) {
                    ui->accum_angle = 0.0f;
                    ui->wheel_scrolled = true;
                    CLICK_SEQ++;
                    view_send_msg(4, 5, 0, 4); /* Prev */
                }
            }
        }
    } else if (code == LV_EVENT_RELEASED && ui->wheel_down) {
        ui->wheel_down = false;
        lv_obj_add_flag(ui->wheel_hl, LV_OBJ_FLAG_HIDDEN);

        if (ui->wheel_in_center) {
            if (ui->is_play) {
                view_send_msg(4, 1, 0, 0x14); /* Music Settings */
            } else {
                view_send_msg(ui->page_id, 4, (uint32_t)ui->sel_idx, 4); /* Select item */
            }
        } else if (!ui->wheel_scrolled) {
            /* Classify tap into the 4 Click Wheel quadrants */
            float tdx = (float)(ui->down_x - WHEEL_CX);
            float tdy = (float)(ui->down_y - WHEEL_CY);
            float ax = tdx < 0.0f ? -tdx : tdx;
            float ay = tdy < 0.0f ? -tdy : tdy;
            if (ay >= ax) {
                if (tdy < 0.0f) {
                    /* TOP: MENU (Back) */
                    view_send_msg(ui->page_id, 1, 0, 0x15);
                } else {
                    /* BOTTOM: Play / Pause */
                    view_send_msg(4, 5, 1, 4);
                }
            } else {
                if (tdx < 0.0f) {
                    /* LEFT: Prev Track */
                    view_send_msg(4, 5, 0, 4);
                } else {
                    /* RIGHT: Next Track */
                    view_send_msg(4, 5, 2, 4);
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
            update_list_colors(ui);
            view_send_msg(ui->page_id, 4, (uint32_t)i, 4);
            break;
        }
    }
}

/* ---------------------------------------------------------------- 30ms Animation & Live Sync Timer */
static void ipod_timer_cb(lv_timer_t *t)
{
    ipod_ui_t *ui = (ipod_ui_t *)t->user_data;
    if (!ui) return;

    /* 1. Smooth cubic ease-out slide-in on page open (160ms) */
    if (!ui->anim_done) {
        uint32_t dt = lv_tick_get() - ui->anim_start_ms;
        if (dt >= 160u) {
            lv_obj_set_pos(ui->lcd_body, 0, HDR_H);
            ui->anim_done = true;
        } else {
            float u = 1.0f - ((float)dt / 160.0f);
            int16_t ox = (int16_t)(44.0f * u * u * u);
            lv_obj_set_pos(ui->lcd_body, ox, HDR_H);
        }
    }

    if (ui->is_play) {
        stock_music_play_t *sp = fw_music_play();
        if (!sp) return;

        /* Track counter ("1/253" -> "1 of 253") */
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

        /* Artist / Lyrics */
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

        /* Smoothly interpolated glossy Aqua progress bar */
        int32_t bv = lv_bar_get_val_fast(sp->bar);
        int32_t bm = lv_bar_get_max_fast(sp->bar);
        float target_pct = (bm > 0) ? ((float)bv / (float)bm) : 0.0f;
        if (target_pct < 0.0f) target_pct = 0.0f;
        if (target_pct > 1.0f) target_pct = 1.0f;
        ui->smooth_pct += (target_pct - ui->smooth_pct) * 0.35f;
        int16_t fw = (int16_t)(ui->smooth_pct * (float)(BAR_W - 2));
        if (fw < 0) fw = 0;
        lv_obj_set_width(ui->bar_fill, fw);
    } else {
        /* Smooth gliding blue selection pill */
        float target_y = (float)(ui->sel_idx * ROW_H);
        ui->sel_y_cur += (target_y - ui->sel_y_cur) * 0.42f;
        lv_obj_set_pos(ui->sel_pill, 0, (lv_coord_t)(ui->sel_y_cur + 0.5f));

        /* Sync 6 list row labels from hidden stock page */
        for (int i = 0; i < ROW_COUNT; i++) {
            const char *txt = lv_label_get_text_fast(get_stock_row_label(ui->page_id, i));
            if (rom_strcmp(txt, ui->last_row[i]) != 0) {
                lv_label_set_text(ui->row_lbl[i], txt);
                size_t n = rom_strlen(txt);
                if (n >= sizeof(ui->last_row[i])) n = sizeof(ui->last_row[i]) - 1;
                rom_memcpy(ui->last_row[i], txt, n);
                ui->last_row[i][n] = '\0';
            }
        }
    }
}

static void ipod_delete_cb(lv_event_t *e)
{
    ipod_ui_t *ui = (ipod_ui_t *)e->user_data;
    if (!ui) return;
    if (ui->timer) {
        lv_timer_del(ui->timer);
        ui->timer = NULL;
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
    ui->anim_start_ms = lv_tick_get();

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
    lv_obj_set_size(hdr, LCD_W - 4, HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_rgb(0xF3, 0xF5, 0xF8), 0);
    lv_obj_set_style_bg_grad_color(hdr, lv_rgb(0xC6, 0xCC, 0xD6), 0);
    lv_obj_set_style_bg_grad_dir(hdr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_rgb(0x92, 0x9A, 0xA6), 0);

    /* Left play triangle indicator */
    ui->lbl_play_ind = make_label(hdr, ">", lv_rgb(0x28, 0x74, 0xC8), LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(ui->lbl_play_ind, 6, 3);

    /* Centered header title */
    lv_obj_t *ht = make_label(hdr, hdr_title, lv_rgb(0x14, 0x18, 0x1E), LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(ht, 140);
    lv_obj_align(ht, LV_ALIGN_CENTER, 0, 0);

    /* Right classic green glossy iPod battery icon */
    lv_obj_t *bat = lv_obj_create(hdr);
    style_clean_box(bat);
    lv_obj_set_pos(bat, LCD_W - 30, 5);
    lv_obj_set_size(bat, 20, 10);
    lv_obj_set_style_radius(bat, 2, 0);
    lv_obj_set_style_bg_color(bat, lv_rgb(0xE8, 0xEC, 0xF0), 0);
    lv_obj_set_style_border_width(bat, 1, 0);
    lv_obj_set_style_border_color(bat, lv_rgb(0x48, 0x52, 0x5E), 0);

    lv_obj_t *bat_fill = lv_obj_create(bat);
    style_clean_box(bat_fill);
    lv_obj_set_pos(bat_fill, 1, 1);
    lv_obj_set_size(bat_fill, 16, 6);
    lv_obj_set_style_bg_color(bat_fill, lv_rgb(0x64, 0xE0, 0x72), 0);
    lv_obj_set_style_bg_grad_color(bat_fill, lv_rgb(0x28, 0x9C, 0x38), 0);
    lv_obj_set_style_bg_grad_dir(bat_fill, LV_GRAD_DIR_VER, 0);

    lv_obj_t *bat_nub = lv_obj_create(hdr);
    style_clean_box(bat_nub);
    lv_obj_set_pos(bat_nub, LCD_W - 10, 7);
    lv_obj_set_size(bat_nub, 2, 6);
    lv_obj_set_style_bg_color(bat_nub, lv_rgb(0x48, 0x52, 0x5E), 0);

    /* 4. Animated inner LCD body (slides in smoothly from right) */
    lv_obj_t *body = lv_obj_create(lcd);
    ui->lcd_body = body;
    style_clean_box(body);
    lv_obj_set_pos(body, 44, HDR_H);
    lv_obj_set_size(body, LCD_W - 4, LCD_H - HDR_H - 4);
    lv_obj_set_style_bg_color(body, lv_rgb(0xFF, 0xFF, 0xFF), 0);

    if (is_play) {
        /* ---- NOW PLAYING SCREEN (matches blue iPod Mini/Nano photo!) ---- */
        ui->lbl_count = make_label(body, "1 of 1", lv_rgb(0x20, 0x24, 0x2A), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(ui->lbl_count, 8, 3);

        /* Album art (64x64) */
        lv_obj_t *art = lv_img_create(body);
        lv_img_set_src(art, &img_album_default);
        lv_obj_set_pos(art, 8, 21);

        /* Song Title & Artist on right of album art */
        ui->lbl_title = make_label(body, "Now Playing", lv_rgb(0x10, 0x12, 0x16), LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(ui->lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(ui->lbl_title, 136);
        lv_obj_set_pos(ui->lbl_title, 80, 26);

        ui->lbl_artist = make_label(body, "", lv_rgb(0x38, 0x40, 0x4A), LV_TEXT_ALIGN_LEFT);
        lv_label_set_long_mode(ui->lbl_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(ui->lbl_artist, 136);
        lv_obj_set_pos(ui->lbl_artist, 80, 50);

        /* Glossy Blue Aqua Progress Bar */
        lv_obj_t *bar_bg = lv_obj_create(body);
        style_clean_box(bar_bg);
        lv_obj_set_pos(bar_bg, 8, 90);
        lv_obj_set_size(bar_bg, BAR_W, 11);
        lv_obj_set_style_radius(bar_bg, 3, 0);
        lv_obj_set_style_bg_color(bar_bg, lv_rgb(0xE4, 0xE9, 0xEF), 0);
        lv_obj_set_style_bg_grad_color(bar_bg, lv_rgb(0xFA, 0xFC, 0xFF), 0);
        lv_obj_set_style_bg_grad_dir(bar_bg, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(bar_bg, 1, 0);
        lv_obj_set_style_border_color(bar_bg, lv_rgb(0xB0, 0xB8, 0xC4), 0);

        ui->bar_fill = lv_obj_create(bar_bg);
        style_clean_box(ui->bar_fill);
        lv_obj_set_pos(ui->bar_fill, 0, 0);
        lv_obj_set_size(ui->bar_fill, 0, 9);
        lv_obj_set_style_bg_color(ui->bar_fill, lv_rgb(0x78, 0xC4, 0xFC), 0);
        lv_obj_set_style_bg_grad_color(ui->bar_fill, lv_rgb(0x26, 0x76, 0xE2), 0);
        lv_obj_set_style_bg_grad_dir(ui->bar_fill, LV_GRAD_DIR_VER, 0);

        /* Elapsed (left) & Remaining (right) */
        ui->lbl_elapsed = make_label(body, "0:00", lv_rgb(0x18, 0x1C, 0x22), LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(ui->lbl_elapsed, 8, 104);

        ui->lbl_remain = make_label(body, "-0:00", lv_rgb(0x18, 0x1C, 0x22), LV_TEXT_ALIGN_RIGHT);
        lv_obj_set_width(ui->lbl_remain, 80);
        lv_obj_set_pos(ui->lbl_remain, LCD_W - 92, 104);
    } else {
        /* ---- CLASSIC iPOD BROWSE LIST (Music / Songs / Artists / Albums) ---- */
        ui->sel_pill = lv_obj_create(body);
        style_clean_box(ui->sel_pill);
        lv_obj_set_pos(ui->sel_pill, 0, 0);
        lv_obj_set_size(ui->sel_pill, LCD_W - 4, ROW_H);
        lv_obj_set_style_bg_color(ui->sel_pill, lv_rgb(0x5C, 0xA9, 0xF8), 0);
        lv_obj_set_style_bg_grad_color(ui->sel_pill, lv_rgb(0x1E, 0x6A, 0xD4), 0);
        lv_obj_set_style_bg_grad_dir(ui->sel_pill, LV_GRAD_DIR_VER, 0);

        for (int i = 0; i < ROW_COUNT; i++) {
            lv_obj_t *lbl = make_label(body, "", lv_rgb(0x14, 0x18, 0x1F), LV_TEXT_ALIGN_LEFT);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
            lv_obj_set_size(lbl, LCD_W - 24, ROW_H);
            lv_obj_set_pos(lbl, 8, (lv_coord_t)(i * ROW_H + 3));
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(lbl, row_tap_cb, LV_EVENT_CLICKED, ui);
            ui->row_lbl[i] = lbl;

            lv_obj_t *ch = make_label(body, ">", lv_rgb(0x9A, 0xA4, 0xB2), LV_TEXT_ALIGN_RIGHT);
            lv_obj_set_pos(ch, LCD_W - 18, (lv_coord_t)(i * ROW_H + 3));
            ui->row_chev[i] = ch;
        }
        update_list_colors(ui);
    }

    /* 5. Iconic White & Blue Click Wheel (156x156 pre-rendered anti-aliased) */
    lv_obj_t *wheel = lv_img_create(root);
    lv_img_set_src(wheel, &img_clickwheel);
    lv_obj_set_pos(wheel, WHEEL_X, WHEEL_Y);

    /* Subtle translucent touch feedback ring */
    ui->wheel_hl = lv_obj_create(root);
    style_clean_box(ui->wheel_hl);
    lv_obj_set_style_bg_color(ui->wheel_hl, lv_rgb(0x38, 0x90, 0xF0), 0);
    lv_obj_set_style_bg_opa(ui->wheel_hl, 85, 0);
    lv_obj_add_flag(ui->wheel_hl, LV_OBJ_FLAG_HIDDEN);

    /* Full-bottom invisible touch catcher over Click Wheel */
    lv_obj_t *touch = lv_obj_create(root);
    style_clean_box(touch);
    lv_obj_set_pos(touch, 0, LCD_Y + LCD_H);
    lv_obj_set_size(touch, SCR_W, SCR_H - (LCD_Y + LCD_H));
    lv_obj_set_style_bg_opa(touch, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(touch, wheel_event_cb, LV_EVENT_ALL, ui);

    /* 6. Start 30ms (~33 fps) animation & live data sync timer */
    ui->timer = lv_timer_create(ipod_timer_cb, 30, ui);
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
    static const uint32_t params[10] = {
        2, 2, 22050, 256, 4, 0, 0, 0, FW_AUDIO_PA_CB, 0
    };

    for (;;) {
        uint32_t seq = CLICK_SEQ;
        if (seq == last_seq) {
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

        /* Stream click sample; restart immediately if wheel keeps spinning! */
        int pos = 0;
        int idle_periods = 0;
        while (idle_periods < 6) {
            if (CLICK_SEQ != last_seq) {
                last_seq = CLICK_SEQ;
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

        /* Drain ring and close immediately so music playback always gets /dev/audio0 */
        fw_os_sleep(55);
        fw_dev_close(&dev);
    }
}

__attribute__((used)) void hook_cpu0_start(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    fw_watchdog_setup(a, b, c, d);
    CLICK_SEQ = 0;
    fw_task_create(click_audio_task, NULL, "ClickTask", 6, 0x400);
}
