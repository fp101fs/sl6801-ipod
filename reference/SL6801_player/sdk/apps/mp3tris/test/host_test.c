/*
 * Host test for Mp3tris: firmware calls are stubbed and the game is driven by fake events and
 * ticks. Prints the board after each scenario and writes play.raw / clearing.raw (convert with
 * show.py). From this directory, with MSYS2 UCRT64 gcc:
 *   gcc -Wall -Wno-unused-function -I ../../../include -I .. host_test.c -o host_test.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk.h"

#undef lv_mem_alloc
#undef lv_mem_free
#undef lv_obj_invalidate
#undef lv_label_set_text
#undef lv_obj_align
#undef lv_obj_clear_flag
#undef lv_obj_add_flag
#undef lv_tick_get
#undef lv_img_create
#undef lv_img_set_src
#undef lv_obj_set_width
#undef lv_obj_set_style_text_align
#undef lv_obj_set_pos
#undef lv_disp_get_hor_res
#undef lv_disp_get_ver_res
#undef lv_obj_set_style_border_width
#undef lv_obj_set_style_border_color
#undef lv_obj_add_event_cb
#undef lv_timer_create
#undef lv_timer_del
#undef lv_event_get_code
#undef lv_event_get_param
#undef FW_SYSTICK_VAL

static uint32_t now_ms;
static int obj_count, invalidations;
static lv_timer_t timer;
static lv_event_cb_t input;
static fw_overlay_t ov;
static int closed;

#define lv_mem_alloc malloc
#define lv_mem_free free
static void lv_obj_invalidate(lv_obj_t *o) { (void)o; invalidations++; }
static lv_obj_t *new_obj(void) { obj_count++; return (lv_obj_t *)calloc(1, 64); }
static void lv_label_set_text(lv_obj_t *o, const char *t) { strncpy((char *)o, t, 63); }
static void lv_obj_align(lv_obj_t *o, uint8_t a, lv_coord_t x, lv_coord_t y) { (void)o; (void)a; (void)x; (void)y; }
static void lv_obj_clear_flag(lv_obj_t *o, uint32_t f) { if (f & 1) ((char *)o)[63] = 0; }
static void lv_obj_add_flag(lv_obj_t *o, uint32_t f) { if (f & 1) ((char *)o)[63] = 1; }
static uint32_t lv_tick_get(void) { return now_ms; }
static lv_obj_t *lv_img_create(lv_obj_t *p) { (void)p; return new_obj(); }
static void lv_img_set_src(lv_obj_t *o, const void *s) { (void)o; (void)s; }
static void lv_obj_set_width(lv_obj_t *o, lv_coord_t w) { (void)o; (void)w; }
static void lv_obj_set_style_text_align(lv_obj_t *o, uint8_t v, uint32_t s) { (void)o; (void)v; (void)s; }
static void lv_obj_set_pos(lv_obj_t *o, lv_coord_t x, lv_coord_t y) { (void)o; (void)x; (void)y; }
static lv_coord_t lv_disp_get_hor_res(void *d) { (void)d; return 128; }
static lv_coord_t lv_disp_get_ver_res(void *d) { (void)d; return 160; }
static void lv_obj_set_style_border_width(lv_obj_t *o, lv_coord_t v, uint32_t s) { (void)o; (void)v; (void)s; }
static void lv_obj_set_style_border_color(lv_obj_t *o, lv_color_t v, uint32_t s) { (void)o; (void)v; (void)s; }
static void *lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t cb, uint8_t f, void *u) { (void)o; (void)f; (void)u; input = cb; return NULL; }
static lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t p, void *u) { timer.timer_cb = cb; timer.period = p; timer.user_data = u; return &timer; }
static void lv_timer_del(lv_timer_t *t) { (void)t; }
static uint8_t lv_event_get_code(lv_event_t *e) { return e->code; }
static void *lv_event_get_param(lv_event_t *e) { return e->param; }
#define FW_SYSTICK_VAL 12345u

fw_overlay_t *fw_overlay_open(lv_obj_t *rf, lv_color_t bg, void (*on_delete)(void *), void *ctx)
{
	(void)rf; (void)bg;
	ov.root = new_obj(); ov.on_delete = on_delete; ov.ctx = ctx;
	return &ov;
}
void fw_overlay_close(fw_overlay_t *o) { closed = 1; o->on_delete(o->ctx); }
lv_obj_t *fw_label(lv_obj_t *p, const char *t, lv_color_t c) { (void)p; (void)c; lv_obj_t *o = new_obj(); lv_label_set_text(o, t); return o; }
lv_obj_t *fw_rect(lv_obj_t *p, lv_coord_t w, lv_coord_t h, lv_color_t c) { (void)p; (void)w; (void)h; (void)c; return new_obj(); }
static int sounds_played;
fw_sound_t *fw_sound_open(void) { return NULL; }
void fw_sound_play(fw_sound_t *s, const fw_tone_t *t, uint8_t n) { (void)s; (void)t; (void)n; sounds_played++; }
void fw_sound_close(fw_sound_t *s) { (void)s; }
char *fw_put_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
char *fw_put_dec(char *p, uint32_t v) { return p + sprintf(p, "%u", (unsigned)v); }

#include "main.c"

static game_t *G;

static void send(uint8_t code, uint32_t key)
{
	lv_event_t e = {0};
	e.code = code;
	e.user_data = G;
	e.param = &key;
	input(&e);
}
static void key(uint32_t k) { send(LV_EVENT_KEY, k); }
static void advance(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 16) {
		now_ms += 16;
		timer.timer_cb(&timer);
	}
}

static void print_board(const char *title)
{
	printf("--- %s: state %d score %u lines %u level %u type %d next %d pos (%d,%d) rot %d\n", title, G->state,
	       (unsigned)G->score, G->lines, G->level, G->type, G->next, G->px, G->py, G->rot);
	cell_t c[4];
	piece_cells(G->type, G->rot, c);
	for (int y = 0; y < ROWS; y++) {
		putchar('|');
		for (int x = 0; x < COLS; x++) {
			int act = 0;
			for (int i = 0; i < 4; i++)
				if (G->state == PLAYING && G->px + c[i].x == x && G->py + c[i].y == y)
					act = 1;
			putchar(act ? '@' : G->board[y][x] ? "IOTSZJL"[G->board[y][x] - 1] : '.');
		}
		puts("|");
	}
}

static void dump(const char *path)
{
	FILE *f = fopen(path, "wb");
	fwrite(G->board_px, 1, sizeof(G->board_px), f);
	fwrite(G->next_px, 1, sizeof(G->next_px), f);
	fclose(f);
}

int main(void)
{
	open(NULL);
	G = timer.user_data;
	printf("objects %d, game_t %u bytes, msg hidden %d: %s\n", obj_count, (unsigned)sizeof(game_t),
	       ((char *)G->msg)[63], (char *)G->msg_title);
	advance(500);
	send(LV_EVENT_PRESSED, 0); /* start */
	send(LV_EVENT_RELEASED, 0);
	key(LV_KEY_ENTER);
	print_board("start");
	/* All 4 rotations of the first piece in place, and walls. */
	for (int i = 0; i < 4; i++) {
		key(FW_KEY_UP);
		printf("rot %d px %d py %d\n", G->rot, G->px, G->py);
	}
	for (int i = 0; i < 8; i++)
		key(FW_KEY_LEFT);
	printf("after 8 left: px %d\n", G->px);
	for (int i = 0; i < 12; i++)
		key(FW_KEY_RIGHT);
	printf("after 12 right: px %d\n", G->px);
	/* Natural fall: level 1 = 800 ms per row. */
	int y0 = G->py;
	advance(1700);
	printf("fell %d rows in 1.7 s\n", G->py - y0);
	/* Each Center press (PRESSED, then KEY on release) drops one row. */
	y0 = G->py;
	send(LV_EVENT_PRESSED, 0);
	key(LV_KEY_ENTER);
	printf("one press: %d row(s)\n", G->py - y0);
	/* Holding Center (PRESSED, then the long code) drops to the bottom and locks. */
	uint8_t first = G->type;
	uint32_t score0 = G->score;
	send(LV_EVENT_PRESSED, 0);
	key(FW_KEY_CENTER_LONG);
	printf("hold: locked %d, score +%u\n", G->type != first, (unsigned)(G->score - score0));
	print_board("after hard drop");
	/* Press until it locks: the press after landing locks it. */
	int presses = 0;
	first = G->type;
	while (G->type == first && presses < 40) {
		send(LV_EVENT_PRESSED, 0);
		key(LV_KEY_ENTER);
		presses++;
	}
	printf("locked after %d presses, score %u\n", presses, (unsigned)G->score);
	print_board("after first lock");

	/* Line clear: fill the bottom 3 rows except column 0, then drop an I vertically there. */
	for (int y = ROWS - 3; y < ROWS; y++)
		for (int x = 1; x < COLS; x++)
			G->board[y][x] = 1 + P_O;
	memset(G->board[ROWS - 4], 0, COLS);
	G->board[ROWS - 4][5] = 1 + P_T;
	G->next = P_I;
	G->state = CLEARING; G->full_rows = 0; G->state_ms = CLEAR_MS; /* force a spawn of the I */
	advance(16);
	key(FW_KEY_UP);
	for (int i = 0; i < 6; i++)
		key(FW_KEY_LEFT);
	print_board("I vertical at left");
	send(LV_EVENT_PRESSED, 0);
	while (G->state == PLAYING)
		advance(16);
	print_board("clearing");
	advance(100);
	dump("clearing.raw");
	advance(300);
	send(LV_EVENT_RELEASED, 0);
	print_board("after clear (expect 3 lines, T kept)");
	advance(400);
	dump("play.raw");

	/* Top out. */
	while (G->state == PLAYING) {
		send(LV_EVENT_PRESSED, 0);
		advance(16);
		while (G->state == PLAYING && !grounded(G))
			advance(16);
		advance(200);
		send(LV_EVENT_RELEASED, 0);
		if (G->state == CLEARING)
			advance(400);
	}
	print_board("game over");
	printf("msg: %s / %s hidden %d\n", (char *)G->msg_title, (char *)G->msg_text, ((char *)G->msg)[63]);
	send(LV_EVENT_PRESSED, 0); /* too soon */
	printf("state right after over + press: %d\n", G->state);
	advance(800);
	send(LV_EVENT_PRESSED, 0);
	printf("state after delay + press: %d, score %u best %u\n", G->state, (unsigned)G->score, (unsigned)G->best);
	key(FW_KEY_BACK);
	printf("short Back: closed %d\n", closed);
	key(FW_KEY_BACK_LONG);
	printf("long Back: closed %d, invalidations %d\n", closed, invalidations);
	return 0;
}
