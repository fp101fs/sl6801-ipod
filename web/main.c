/*
 * Emscripten + LVGL 8.3 WebAssembly host for the real SL6801 iPod Classic UI (ipod/ipod_ui.c).
 */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>
#include "fw_240x320.h"

/* Hooked page constructors defined in ipod/ipod_ui.c */
extern int hook_music_menu_create(void);
extern int hook_music_song_create(void);
extern int hook_music_play_create(void);
extern int hook_music_artist_create(void);
extern int hook_music_album_create(void);

volatile uint32_t g_web_click_seq = 0xFFFFFFFEu;
volatile uint32_t g_web_shot_ctx  = 0u;
static uint32_t   g_last_click_seq = 0xFFFFFFFEu;

/* ---------------------------------------------------------------- Display & Input state */
#define W_SCR 240
#define H_SCR 320

static uint32_t      g_fb_rgba[W_SCR * H_SCR];
static lv_color_t    g_draw_buf1[W_SCR * 40];
static lv_disp_draw_buf_t g_disp_buf;
static lv_disp_drv_t g_disp_drv;
static lv_indev_drv_t g_indev_drv;

static int16_t g_ptr_x = 120;
static int16_t g_ptr_y = 234;
static bool    g_ptr_down = false;

static void web_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    for (int y = area->y1; y <= area->y2; y++) {
        uint32_t *dst = g_fb_rgba + y * W_SCR + area->x1;
        for (int x = area->x1; x <= area->x2; x++) {
            uint16_t raw = color_p->full;
            color_p++;
            uint16_t v = (uint16_t)((raw >> 8) | (raw << 8)); /* LV_COLOR_16_SWAP = 1 */
            uint8_t r = (uint8_t)((v >> 8) & 0xF8u); r |= (uint8_t)(r >> 5);
            uint8_t g = (uint8_t)((v >> 3) & 0xFCu); g |= (uint8_t)(g >> 6);
            uint8_t b = (uint8_t)((v << 3) & 0xF8u); b |= (uint8_t)(b >> 5);
            /* Little-endian RGBA bytes in memory: R, G, B, A */
            *dst++ = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
        }
    }
    lv_disp_flush_ready(drv);
}

static void web_indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = g_ptr_x;
    data->point.y = g_ptr_y;
    data->state = g_ptr_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---------------------------------------------------------------- Mock SL6801 Music Library */
typedef struct {
    const char *title;
    const char *artist;
    const char *album;
    int duration_s;
} demo_track_t;

static const demo_track_t g_tracks[] = {
    {"Digital Love",               "Daft Punk",      "Discovery",              298},
    {"Weird Fishes / Arpeggi",     "Radiohead",      "In Rainbows",            318},
    {"Time",                       "Pink Floyd",     "Dark Side of the Moon",  413},
    {"Feel Good Inc.",             "Gorillaz",       "Demon Days",             222},
    {"Let It Happen",              "Tame Impala",    "Currents",               467},
    {"Nights",                     "Frank Ocean",    "Blonde",                 307},
    {"Midnight City",              "M83",            "Hurry Up, We're Dreaming", 243},
    {"Everything In Its Right Place", "Radiohead",   "Kid A",                  251},
    {"Veridis Quo",                "Daft Punk",      "Discovery",              345},
    {"Breathe (In the Air)",       "Pink Floyd",     "Dark Side of the Moon",  169},
    {"Kids With Guns",             "Gorillaz",       "Demon Days",             225},
    {"The Less I Know The Better", "Tame Impala",    "Currents",               216},
};
#define NUM_TRACKS ((int)(sizeof(g_tracks) / sizeof(g_tracks[0])))

static const char *g_menu_items[6] = {
    "Songs", "Artists", "Albums", "Now Playing", "Playlists", "Shuffle Songs"
};
static const char *g_artists[] = {
    "Daft Punk", "Frank Ocean", "Gorillaz", "M83", "Pink Floyd", "Radiohead", "Tame Impala"
};
#define NUM_ARTISTS ((int)(sizeof(g_artists) / sizeof(g_artists[0])))

static const char *g_albums[] = {
    "Blonde", "Currents", "Dark Side of the Moon", "Demon Days", "Discovery", "In Rainbows", "Kid A"
};
#define NUM_ALBUMS ((int)(sizeof(g_albums) / sizeof(g_albums[0])))

/* Active mock stock page structs */
static stock_music_menu_t g_menu;
static stock_music_list_t g_song;
static stock_music_list_t g_artist;
static stock_music_list_t g_album;
static stock_music_play_t g_play;

static uint8_t g_cur_page = 2;
static int     g_list_offset = 0;
static int     g_track_idx = 0;
static int     g_elapsed_s = 72;
static bool    g_paused = false;
static uint32_t g_last_sec_ms = 0;

stock_music_menu_t *web_fw_music_menu(void)   { return &g_menu; }
stock_music_list_t *web_fw_music_song(void)   { return &g_song; }
stock_music_play_t *web_fw_music_play(void)   { return &g_play; }
stock_music_list_t *web_fw_music_artist(void) { return &g_artist; }
stock_music_list_t *web_fw_music_album(void)  { return &g_album; }

static void clear_screen(void)
{
    lv_obj_clean(lv_disp_get_scr_act(NULL));
    memset(&g_menu, 0, sizeof(g_menu));
    memset(&g_song, 0, sizeof(g_song));
    memset(&g_artist, 0, sizeof(g_artist));
    memset(&g_album, 0, sizeof(g_album));
    memset(&g_play, 0, sizeof(g_play));
}

static void populate_list_labels(stock_music_list_t *lst, const char *const *items, int total, int offset)
{
    for (int i = 0; i < 6; i++) {
        int idx = offset + i;
        if (lst->lbl[i]) {
            lv_label_set_text(lst->lbl[i], (idx >= 0 && idx < total) ? items[idx] : "");
        }
    }
}

static void populate_song_labels(int offset)
{
    for (int i = 0; i < 6; i++) {
        int idx = offset + i;
        if (g_song.lbl[i]) {
            lv_label_set_text(g_song.lbl[i], (idx >= 0 && idx < NUM_TRACKS) ? g_tracks[idx].title : "");
        }
    }
}

static void update_play_widgets(void)
{
    if (!g_play.title) return;
    const demo_track_t *tr = &g_tracks[g_track_idx];
    char buf[32];
    snprintf(buf, sizeof(buf), "%d/%d", g_track_idx + 1, NUM_TRACKS);
    lv_label_set_text(g_play.track_cnt, buf);
    lv_label_set_text(g_play.title, tr->title);
    lv_label_set_text(g_play.artist, tr->artist);

    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", g_elapsed_s / 3600, (g_elapsed_s % 3600) / 60, g_elapsed_s % 60);
    lv_label_set_text(g_play.elapsed, buf);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tr->duration_s / 3600, (tr->duration_s % 3600) / 60, tr->duration_s % 60);
    lv_label_set_text(g_play.total, buf);

    lv_bar_set_range(g_play.bar, 0, tr->duration_s);
    lv_bar_set_value(g_play.bar, g_elapsed_s, LV_ANIM_OFF);

    lv_obj_t *plbl = lv_obj_get_child(g_play.btn_play, 0);
    if (plbl) {
        /* Matches ipod_ui.c detection: "\xEE\x9A\x8C" = paused, "\xEE\x99\x8C" = playing */
        lv_label_set_text(plbl, g_paused ? "\xEE\x9A\x8C" : "\xEE\x99\x8C");
    }
}

int stock_page_music_menu_create(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    for (int i = 0; i < 6; i++) {
        g_menu.lbl[i] = lv_label_create(scr);
        lv_label_set_text(g_menu.lbl[i], g_menu_items[i]);
    }
    return 0;
}

int stock_page_music_song_create(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    for (int i = 0; i < 6; i++) g_song.lbl[i] = lv_label_create(scr);
    populate_song_labels(g_list_offset);
    return 0;
}

int stock_page_music_artist_create(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    for (int i = 0; i < 6; i++) g_artist.lbl[i] = lv_label_create(scr);
    populate_list_labels(&g_artist, g_artists, NUM_ARTISTS, g_list_offset);
    return 0;
}

int stock_page_music_album_create(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    for (int i = 0; i < 6; i++) g_album.lbl[i] = lv_label_create(scr);
    populate_list_labels(&g_album, g_albums, NUM_ALBUMS, g_list_offset);
    return 0;
}

int stock_page_music_play_create(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    g_play.track_cnt = lv_label_create(scr);
    g_play.title     = lv_label_create(scr);
    g_play.artist    = lv_label_create(scr);
    g_play.elapsed   = lv_label_create(scr);
    g_play.total     = lv_label_create(scr);
    g_play.bar       = lv_bar_create(scr);
    g_play.btn_play  = lv_obj_create(scr);
    lv_label_create(g_play.btn_play);
    update_play_widgets();
    return 0;
}

static void open_page(uint8_t page_id)
{
    g_cur_page = page_id;
    g_list_offset = 0;
    clear_screen();
    if (page_id == 2)      hook_music_menu_create();
    else if (page_id == 3) hook_music_song_create();
    else if (page_id == 4) hook_music_play_create();
    else if (page_id == 6) hook_music_artist_create();
    else if (page_id == 7) hook_music_album_create();
}

void view_send_msg(uint32_t page_id, uint32_t type, uint32_t sub_id, uint32_t ev)
{
    /* Acknowledge CLICK_STOP_MAGIC immediately in WASM */
    if (g_web_click_seq == 0xFFFFFFFFu) g_web_click_seq = 0xFFFFFFFEu;

    if (type == 1 && ev == 0x15) {
        /* MENU (Back) */
        if (page_id != 2) open_page(2);
        return;
    }
    if (type == 6 && (ev == 0x16 || ev == 0x17)) {
        /* Next / Prev list page */
        int total = (page_id == 3) ? NUM_TRACKS : (page_id == 6 ? NUM_ARTISTS : NUM_ALBUMS);
        if (ev == 0x16) {
            g_list_offset = (g_list_offset + 6 < total) ? (g_list_offset + 6) : 0;
        } else {
            int last_pg = ((total - 1) / 6) * 6;
            g_list_offset = (g_list_offset >= 6) ? (g_list_offset - 6) : last_pg;
        }
        if (page_id == 3) populate_song_labels(g_list_offset);
        else if (page_id == 6) populate_list_labels(&g_artist, g_artists, NUM_ARTISTS, g_list_offset);
        else if (page_id == 7) populate_list_labels(&g_album, g_albums, NUM_ALBUMS, g_list_offset);
        return;
    }
    if (type == 4 && ev == 4) {
        /* Select row */
        if (page_id == 2) {
            if (sub_id == 0)      open_page(3);
            else if (sub_id == 1) open_page(6);
            else if (sub_id == 2) open_page(7);
            else if (sub_id == 3) open_page(4);
            else if (sub_id == 4) open_page(3);
            else if (sub_id == 5) { g_track_idx = (g_track_idx + 3) % NUM_TRACKS; g_elapsed_s = 0; g_paused = false; open_page(4); }
        } else if (page_id == 3) {
            int idx = g_list_offset + (int)sub_id;
            if (idx < NUM_TRACKS) {
                g_track_idx = idx;
                g_elapsed_s = 0;
                g_paused = false;
                open_page(4);
            }
        } else {
            open_page(3);
        }
        return;
    }
    if (page_id == 4 && type == 5 && ev == 4) {
        if (sub_id == 0) {
            g_track_idx = (g_track_idx + NUM_TRACKS - 1) % NUM_TRACKS;
            g_elapsed_s = 0;
        } else if (sub_id == 1) {
            g_paused = !g_paused;
        } else if (sub_id == 2) {
            g_track_idx = (g_track_idx + 1) % NUM_TRACKS;
            g_elapsed_s = 0;
        }
        update_play_widgets();
    }
}

/* ---------------------------------------------------------------- Main loop & JS Exports */
static double g_prev_time_ms = 0.0;

static void main_loop(void)
{
    double now = emscripten_get_now();
    uint32_t dt = (g_prev_time_ms > 0.0) ? (uint32_t)(now - g_prev_time_ms) : 16u;
    if (dt > 50u) dt = 50u;
    g_prev_time_ms = now;
    lv_tick_inc(dt);

    /* Trigger Web Audio click sound whenever ipod_ui.c increments CLICK_SEQ */
    uint32_t seq = g_web_click_seq;
    if (seq != g_last_click_seq) {
        if (seq == 0xFFFFFFFFu) {
            g_web_click_seq = 0xFFFFFFFEu;
            g_last_click_seq = 0xFFFFFFFEu;
        } else {
            g_last_click_seq = seq;
            if (seq != 0xFFFFFFFEu && seq != 0u) {
                EM_ASM({ if (window.playIpodClick) window.playIpodClick(); });
            }
        }
    }

    /* Advance Now Playing clock every 1s */
    uint32_t t = lv_tick_get();
    if (g_cur_page == 4 && !g_paused && (t - g_last_sec_ms) >= 1000u) {
        g_last_sec_ms = t;
        g_elapsed_s++;
        if (g_elapsed_s >= g_tracks[g_track_idx].duration_s) {
            g_elapsed_s = 0;
            g_track_idx = (g_track_idx + 1) % NUM_TRACKS;
        }
        update_play_widgets();
    }

    lv_timer_handler();
    EM_ASM({ if (window.presentIpodFrame) window.presentIpodFrame(); });
}

EMSCRIPTEN_KEEPALIVE void web_pointer_event(int x, int y, int pressed)
{
    if (x < 0) x = 0;
    if (x >= W_SCR) x = W_SCR - 1;
    if (y < 0) y = 0;
    if (y >= H_SCR) y = H_SCR - 1;
    g_ptr_x = (int16_t)x;
    g_ptr_y = (int16_t)y;
    g_ptr_down = (pressed != 0);
}

EMSCRIPTEN_KEEPALIVE uint32_t *web_get_fb(void)
{
    return g_fb_rgba;
}

EMSCRIPTEN_KEEPALIVE void web_nav_page(int page_id)
{
    if (page_id == 2 || page_id == 3 || page_id == 4 || page_id == 6 || page_id == 7) {
        open_page((uint8_t)page_id);
    }
}

int main(void)
{
    lv_init();
    lv_disp_draw_buf_init(&g_disp_buf, g_draw_buf1, NULL, W_SCR * 40);
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.hor_res = W_SCR;
    g_disp_drv.ver_res = H_SCR;
    g_disp_drv.flush_cb = web_disp_flush;
    g_disp_drv.draw_buf = &g_disp_buf;
    lv_disp_drv_register(&g_disp_drv);

    lv_indev_drv_init(&g_indev_drv);
    g_indev_drv.type = LV_INDEV_TYPE_POINTER;
    g_indev_drv.read_cb = web_indev_read;
    lv_indev_drv_register(&g_indev_drv);

    g_last_sec_ms = 0;
    open_page(2); /* Start on iPod Music Menu */

    emscripten_set_main_loop(main_loop, 0, 1);
    return 0;
}
