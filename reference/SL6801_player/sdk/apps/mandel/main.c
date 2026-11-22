/*
 * "Mandelbrot" entry in the APP menu: a Mandelbrot set explorer. Up, Left and Right move the view
 * (hold to repeat), a short Back moves it down (Back is the bottom button), Center zooms in,
 * holding Center zooms out, holding Back exits. Colors cycle blue, green, red, blue with the
 * escape iteration; points in the set are black.
 *
 * A worker task on CPU0 renders the view into an indexed image in the worker's memory, first in
 * 4x4 blocks and then per pixel, and starts over when the view changes; a UI timer redraws the
 * image as rows come in. The task sleeps 1 ms after each row.
 *
 * The math is 32-bit fixed point with 28 fraction bits (no floating point is linked), which
 * allows 16 zoom levels (65536x) before pixels get close to the precision.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define IMG_W 128
#define IMG_H 144
#define BAR_H 16
#define PALETTE_SIZE (256 * 4)
#define FRAC 28
#define FIX(x) ((int32_t)((x) * (1 << FRAC)))
#define START_X FIX(-0.6)
#define START_STEP FIX(3.2 / IMG_W) /* view 3.2 wide */
#define MIN_STEP 64                 /* deepest zoom, in units of 2^-28 */
#define LIMIT FIX(2.5)              /* the view's center stays within +-2.5 */
#define BASE_ITER 48
#define ITER_PER_LEVEL 24
#define MAX_ITER 512
#define CYCLE_STEP 8 /* palette steps per iteration: a full cycle every 32 iterations */
#define TICK_MS 50
#define TASK_PRIORITY 2
#define TASK_STACK 0x400 /* words */
#define ROW_PAUSE_MS 1
#define IDLE_POLL_MS 10

/* The worker's memory, shared by the UI and the render task. */
typedef struct {
	volatile int32_t cx, cy, step; /* view center and pixel size, written by the UI */
	volatile uint16_t max_iter;
	volatile uint32_t view;     /* bumped by the UI after changing the view */
	volatile uint32_t progress; /* bumped by the task after each row */
	uint8_t px[PALETTE_SIZE + IMG_W * IMG_H];
} render_t;

typedef struct {
	fw_overlay_t *ov;
	fw_worker_t *w;
	render_t *r; /* the worker's memory, once it runs */
	lv_timer_t *timer;
	lv_obj_t *img, *status;
	lv_img_dsc_t dsc;
	int32_t cx, cy;
	uint8_t level;
	uint32_t drawn; /* r->progress when the image was last invalidated */
} app_t;

static void set_color(uint8_t *pal, int i, int r, int g, int b)
{
	pal[i * 4 + 0] = (uint8_t)b;
	pal[i * 4 + 1] = (uint8_t)g;
	pal[i * 4 + 2] = (uint8_t)r;
	pal[i * 4 + 3] = 0xFF;
}

/* Entry 0 is black (in the set); 1-255 are one cycle blue, green, red, back to blue. */
static void set_palette(uint8_t *pal)
{
	static const uint8_t stops[4][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {0, 0, 255}};
	set_color(pal, 0, 0, 0, 0);
	for (int i = 0; i < 255; i++) {
		int seg = i / 85, f = i % 85;
		const uint8_t *a = stops[seg], *b = stops[seg + 1];
		set_color(pal, 1 + i, a[0] + (b[0] - a[0]) * f / 85, a[1] + (b[1] - a[1]) * f / 85,
			  a[2] + (b[2] - a[2]) * f / 85);
	}
}

/* ---------------------------------------------------------------- render task */

/* Palette index for c = cr + ci*i. While |z| <= 2 every product fits: |z|^2 stays below 2^63
 * in Q56, and the next z below 8 in Q28. */
static uint8_t point_color(int32_t cr, int32_t ci, int max_iter)
{
	int32_t zr = 0, zi = 0;
	for (int n = 0; n < max_iter; n++) {
		int64_t zr2 = (int64_t)zr * zr, zi2 = (int64_t)zi * zi; /* Q56 */
		if (zr2 + zi2 > (4ll << (2 * FRAC)))
			return (uint8_t)(1 + n * CYCLE_STEP % 255);
		int64_t zri = (int64_t)zr * zi;
		zi = (int32_t)(zri >> (2 * FRAC - FRAC - 1)) + ci; /* 2 * zr * zi */
		zr = (int32_t)((zr2 - zi2) >> FRAC) + cr;
	}
	return 0;
}

/* Renders the view in blocks of size x size pixels; false if the view changed or the task is
 * stopping. */
static bool render_pass(fw_worker_t *w, render_t *r, uint32_t view, int size)
{
	int32_t cx = r->cx, cy = r->cy, step = r->step;
	int max_iter = r->max_iter;
	uint8_t *px = r->px + PALETTE_SIZE;
	for (int y = 0; y < IMG_H; y += size) {
		int32_t ci = cy - (y - IMG_H / 2) * step;
		for (int x = 0; x < IMG_W; x += size) {
			uint8_t c = point_color(cx + (x - IMG_W / 2) * step, ci, max_iter);
			for (int dy = 0; dy < size; dy++)
				memset(px + (y + dy) * IMG_W + x, c, (size_t)size);
		}
		r->progress++;
		fw_os_sleep(ROW_PAUSE_MS);
		if (w->stop || r->view != view)
			return false;
	}
	return true;
}

static void render_task(fw_worker_t *w)
{
	render_t *r = w->mem;
	uint32_t view = 0; /* the UI's first set_view makes it 1 */
	while (!w->stop) {
		if (r->view == view) {
			fw_os_sleep(IDLE_POLL_MS);
			continue;
		}
		view = r->view;
		if (render_pass(w, r, view, 4))
			render_pass(w, r, view, 1);
	}
}

/* ---------------------------------------------------------------- UI */

static void set_view(app_t *app)
{
	render_t *r = app->r;
	r->cx = app->cx;
	r->cy = app->cy;
	r->step = START_STEP >> app->level;
	uint32_t iter = BASE_ITER + ITER_PER_LEVEL * app->level;
	r->max_iter = (uint16_t)(iter < MAX_ITER ? iter : MAX_ITER);
	__sync_synchronize();
	r->view++;

	char text[32];
	char *p = fw_put_str(text, "Zoom ");
	p = fw_put_dec(p, 1u << app->level);
	p = fw_put_str(p, "x");
	if ((START_STEP >> (app->level + 1)) < MIN_STEP)
		fw_put_str(p, " (max)");
	lv_label_set_text(app->status, text);
}

static int32_t clamp_center(int32_t v)
{
	return v > LIMIT ? LIMIT : v < -LIMIT ? -LIMIT : v;
}

/* Shows the image once the worker runs. */
static void start_view(app_t *app)
{
	render_t *r = app->r;
	set_palette(r->px);
	app->dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_8BIT, IMG_W, IMG_H);
	app->dsc.data_size = sizeof(r->px);
	app->dsc.data = r->px;
	app->img = lv_img_create(app->ov->root);
	lv_img_set_src(app->img, &app->dsc);
	lv_obj_set_pos(app->img, 0, 0);
	set_view(app);
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	if (!app->r) {
		app->r = fw_worker_mem(app->w);
		if (app->r)
			start_view(app);
		else if (fw_worker_failed(app->w))
			lv_label_set_text(app->status, "Out of memory");
		return;
	}
	uint32_t progress = app->r->progress;
	if (progress != app->drawn) {
		app->drawn = progress;
		lv_obj_invalidate(app->img);
	}
}

static void on_delete(void *ctx)
{
	app_t *app = ctx;
	if (app->timer)
		lv_timer_del(app->timer);
	fw_worker_release(app->w);
	lv_mem_free(app);
}

static void key_cb(lv_event_t *e)
{
	app_t *app = lv_event_get_user_data(e);
	uint32_t key = *(uint32_t *)lv_event_get_param(e);
	if (key == FW_KEY_BACK_LONG) {
		fw_overlay_close(app->ov); /* frees app via on_delete */
		return;
	}
	if (!app->r)
		return;
	int32_t step = START_STEP >> app->level;
	int32_t dx = step * (IMG_W / 4), dy = step * (IMG_H / 4);
	switch (key) {
	case FW_KEY_UP:
	case FW_KEY_UP_LONG:
	case FW_KEY_UP_REPEAT:
		app->cy = clamp_center(app->cy + dy);
		break;
	case FW_KEY_BACK:
		app->cy = clamp_center(app->cy - dy);
		break;
	case FW_KEY_LEFT:
	case FW_KEY_LEFT_LONG:
	case FW_KEY_LEFT_REPEAT:
		app->cx = clamp_center(app->cx - dx);
		break;
	case FW_KEY_RIGHT:
	case FW_KEY_RIGHT_LONG:
	case FW_KEY_RIGHT_REPEAT:
		app->cx = clamp_center(app->cx + dx);
		break;
	case FW_KEY_CENTER: /* arrives on release of a short press */
		if ((START_STEP >> (app->level + 1)) < MIN_STEP)
			return;
		app->level++;
		break;
	case FW_KEY_CENTER_LONG:
		if (!app->level)
			return;
		app->level--;
		break;
	default:
		return;
	}
	set_view(app);
}

static void open(lv_obj_t *return_focus)
{
	app_t *app = lv_mem_alloc(sizeof(app_t));
	if (!app)
		return;
	memset(app, 0, sizeof(*app));
	app->cx = START_X;
	app->ov = fw_overlay_open(return_focus, lv_rgb(0, 0, 0), on_delete, app);
	if (!app->ov) {
		lv_mem_free(app);
		return;
	}
	lv_obj_t *root = app->ov->root;
	lv_coord_t h = lv_disp_get_ver_res(NULL);
	app->status = fw_label(root, "", lv_rgb(0xC0, 0xC8, 0xD0));
	lv_obj_set_pos(app->status, 2, (lv_coord_t)(h - BAR_H + 1));
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);

	app->w = fw_worker_start(render_task, sizeof(render_t), "mandel", TASK_PRIORITY, TASK_STACK);
	if (!app->w) {
		lv_label_set_text(app->status, "Busy, try again");
		return;
	}
	app->timer = lv_timer_create(tick, TICK_MS, app);
}

FW_MENU_ITEM(FW_SYMBOL_PLAY, "Mandelbrot", open);
