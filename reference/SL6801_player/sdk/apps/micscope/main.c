/*
 * "Mic scope" entry in the APP menu: an oscilloscope view of the microphone input, the left
 * channel on top and the right one below, triggered on a rising zero crossing.
 * Up changes the gain, Left/Right the time scale, Center freezes the view, Back exits.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define SCOPE_W 128
#define SCOPE_H 120
#define HALF_H (SCOPE_H / 2)
#define STRIDE (SCOPE_W / 4)
#define PALETTE_SIZE (4 * 4)
#define FRAMES (FW_MIC_RING / 2)
#define MAX_ZOOM 3 /* frames per column; leaves FRAMES - SCOPE_W * zoom frames to find a trigger */
#define MAX_GAIN 32
#define TICK_MS 50
#define STATS_TICKS 5

enum { PAL_BG, PAL_GRID, PAL_LEFT, PAL_RIGHT };

static const uint8_t palette_rgb[4][3] = {{0x08, 0x10, 0x18}, {0x30, 0x48, 0x58}, {0x40, 0xF0, 0x60}, {0xF0, 0xD0, 0x40}};

typedef struct {
	fw_overlay_t *ov;
	fw_mic_t *mic;
	lv_timer_t *timer;
	lv_obj_t *img, *stats, *info;
	lv_img_dsc_t dsc;
	uint8_t zoom, gain;
	bool frozen;
	uint8_t ticks;
	int16_t peak[2];
	int16_t samples[FRAMES * 2];
	uint8_t px[PALETTE_SIZE + STRIDE * SCOPE_H];
} app_t;

static void plot(app_t *app, int x, int y, uint8_t c)
{
	uint8_t *p = &app->px[PALETTE_SIZE + y * STRIDE + x / 4];
	int shift = (3 - x % 4) * 2;
	*p = (uint8_t)((*p & ~(3 << shift)) | (c << shift));
}

static void draw_grid(app_t *app)
{
	memset(app->px + PALETTE_SIZE, 0, STRIDE * SCOPE_H);
	for (int x = 0; x < SCOPE_W; x++) {
		plot(app, x, HALF_H, PAL_GRID);
		if (x % 4 == 0) {
			plot(app, x, HALF_H / 2, PAL_GRID);
			plot(app, x, HALF_H + HALF_H / 2, PAL_GRID);
		}
	}
	for (int y = 0; y < SCOPE_H; y += 4)
		for (int x = 16; x < SCOPE_W; x += 16)
			plot(app, x, y, PAL_GRID);
}

/* Row of a sample in the half that starts at top. */
static int sample_y(app_t *app, int32_t s, int top)
{
	int y = HALF_H / 2 - (int)(s * app->gain * (HALF_H / 2 - 1) / 32768);
	y = y < 1 ? 1 : y > HALF_H - 2 ? HALF_H - 2 : y;
	return top + y;
}

static void draw_trace(app_t *app, const int16_t *s, int ch, int top, uint8_t color)
{
	int prev = -1;
	for (int x = 0; x < SCOPE_W; x++) {
		int lo = SCOPE_H, hi = -1;
		for (int k = 0; k < app->zoom; k++) {
			int y = sample_y(app, s[2 * (x * app->zoom + k) + ch], top);
			lo = y < lo ? y : lo;
			hi = y > hi ? y : hi;
		}
		if (prev >= 0) {
			lo = prev < lo ? prev : lo;
			hi = prev > hi ? prev : hi;
		}
		for (int y = lo; y <= hi; y++)
			plot(app, x, y, color);
		prev = sample_y(app, s[2 * (x * app->zoom + app->zoom - 1) + ch], top);
	}
}

/* Index of the first rising zero crossing of channel ch in the first `range` frames, else 0. */
static int find_trigger(const int16_t *s, int ch, int range)
{
	for (int i = 1; i < range; i++)
		if (s[2 * (i - 1) + ch] < 0 && s[2 * i + ch] >= 0)
			return i;
	return 0;
}

static void update_labels(app_t *app)
{
	char text[48];
	char *p = fw_put_str(text, "Peak L ");
	p = fw_put_dec(p, (uint32_t)app->peak[0]);
	p = fw_put_str(p, "  R ");
	fw_put_dec(p, (uint32_t)app->peak[1]);
	lv_label_set_text(app->stats, text);
	p = fw_put_str(text, "x");
	p = fw_put_dec(p, app->gain);
	p = fw_put_str(p, "  ");
	p = fw_put_dec(p, (uint32_t)SCOPE_W * app->zoom * 1000 / FW_MIC_RATE);
	p = fw_put_str(p, " ms");
	if (app->frozen)
		p = fw_put_str(p, "  HOLD");
	lv_label_set_text(app->info, text);
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	if (fw_mic_failed(app->mic)) {
		lv_label_set_text(app->stats, "Mic unavailable");
		lv_timer_del(t);
		app->timer = NULL;
		return;
	}
	if (app->frozen)
		return;
	if (fw_mic_latest(app->mic, app->samples, FRAMES) < FRAMES)
		return;
	int16_t peak[2] = {0, 0};
	for (int i = 0; i < FRAMES * 2; i++) {
		int32_t a = app->samples[i] < 0 ? -(int32_t)app->samples[i] : app->samples[i];
		if (a > peak[i & 1])
			peak[i & 1] = (int16_t)(a > 32767 ? 32767 : a);
	}
	if (peak[0] > app->peak[0])
		app->peak[0] = peak[0];
	if (peak[1] > app->peak[1])
		app->peak[1] = peak[1];
	int range = FRAMES - SCOPE_W * app->zoom;
	int start = find_trigger(app->samples, peak[1] > peak[0], range);
	draw_grid(app);
	draw_trace(app, app->samples + 2 * start, 0, 0, PAL_LEFT);
	draw_trace(app, app->samples + 2 * start, 1, HALF_H, PAL_RIGHT);
	lv_obj_invalidate(app->img);
	if (++app->ticks >= STATS_TICKS) {
		update_labels(app);
		app->ticks = 0;
		app->peak[0] = app->peak[1] = 0;
	}
}

static void on_delete(void *ctx)
{
	app_t *app = ctx;
	if (app->timer)
		lv_timer_del(app->timer);
	fw_mic_close(app->mic);
	lv_mem_free(app);
}

static void key_cb(lv_event_t *e)
{
	app_t *app = lv_event_get_user_data(e);
	uint32_t key = *(uint32_t *)lv_event_get_param(e);
	switch (key) {
	case FW_KEY_BACK:
	case LV_KEY_ESC:
		fw_overlay_close(app->ov); /* frees app via on_delete */
		return;
	case FW_KEY_UP:
		app->gain = app->gain >= MAX_GAIN ? 1 : (uint8_t)(app->gain * 2);
		break;
	case FW_KEY_LEFT:
		if (app->zoom > 1)
			app->zoom--;
		break;
	case FW_KEY_RIGHT:
		if (app->zoom < MAX_ZOOM)
			app->zoom++;
		break;
	case LV_KEY_ENTER:
		app->frozen = !app->frozen;
		break;
	default:
		return;
	}
	update_labels(app);
}

static void open(lv_obj_t *return_focus)
{
	app_t *app = lv_mem_alloc(sizeof(app_t));
	if (!app)
		return;
	memset(app, 0, sizeof(*app));
	app->zoom = 1;
	app->gain = 1;
	app->ov = fw_overlay_open(return_focus, lv_rgb(0, 0, 0), on_delete, app);
	if (!app->ov) {
		lv_mem_free(app);
		return;
	}
	lv_obj_t *root = app->ov->root;
	lv_coord_t h = lv_disp_get_ver_res(NULL);

	for (int i = 0; i < 4; i++) {
		app->px[i * 4 + 0] = palette_rgb[i][2];
		app->px[i * 4 + 1] = palette_rgb[i][1];
		app->px[i * 4 + 2] = palette_rgb[i][0];
		app->px[i * 4 + 3] = 0xFF;
	}
	draw_grid(app);
	app->dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_2BIT, SCOPE_W, SCOPE_H);
	app->dsc.data_size = sizeof(app->px);
	app->dsc.data = app->px;
	app->img = lv_img_create(root);
	lv_img_set_src(app->img, &app->dsc);
	lv_obj_set_pos(app->img, 0, (lv_coord_t)((h - SCOPE_H) / 2));

	app->stats = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_obj_set_pos(app->stats, 2, 1);
	app->info = fw_label(root, "", lv_rgb(0xA0, 0xB0, 0xC0));
	lv_obj_set_pos(app->info, 2, (lv_coord_t)(h - 18));

	app->mic = fw_mic_open();
	if (app->mic) {
		update_labels(app);
		app->timer = lv_timer_create(tick, TICK_MS, app);
	} else {
		lv_label_set_text(app->stats, "Mic unavailable");
	}
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);
}

FW_MENU_ITEM(FW_SYMBOL_AUDIO, "Mic scope", open);
