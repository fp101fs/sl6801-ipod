/*
 * "FPU test" entry in the APP menu: a rotating wireframe drawn with single-precision float math,
 * and a check that FPU registers survive task switches. Left/Right change the shape, Up the spin
 * speed, Center pauses, Back exits.
 *
 * Each frame rotates and projects the shape's vertices in float (angles through the ROM's sinf and
 * cosf) and draws its edges into an indexed image, the far ones dimmer. The status shows how long
 * that took, timed with the ROM's 64-bit hardware timer.
 *
 * The check: a low-priority worker task on CPU0 keeps repeating a calculation that holds 16
 * floats across calls (so they live in s16-s31, which only the task switch saves) and also runs
 * double and 64-bit integer math through the ROM's helpers. Each result must match the one the
 * UI (on CPU1) computed when the app opened; the tick and other tasks preempt the task often.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define IMG_W 128
#define IMG_H 112
#define STRIDE (IMG_W / 4)
#define PALETTE_SIZE (4 * 4)
#define LINE_H 16
#define MAX_VERTS 72
#define MAX_EDGES 144
#define TORUS_MAJOR 12 /* TORUS_MAJOR * TORUS_MINOR = MAX_VERTS */
#define TORUS_MINOR 6
#define TWO_PI 6.2831853f
#define CAMERA_DIST 4.0f
#define FOCAL 165.0f      /* pixels: radius 1 spans 41 px at the center, 55 px at the nearest */
#define BASE_SPIN 0.0008f /* radians per ms at speed x1 */
#define SPEEDS 4          /* x1, x2, x4, x8 */
#define TICK_MS 33
#define STATS_TICKS 15
#define CHECK_PRIORITY 2
#define CHECK_STACK 0x400 /* words */
#define CHECK_ITERS 2000 /* about 1-2 ms per run, so ticks and other tasks interrupt it */
#define CHECK_PAUSE_MS 2

enum { PAL_BG, PAL_BACK, PAL_FRONT, PAL_VERTEX };
enum { SHAPE_CUBE, SHAPE_OCTAHEDRON, SHAPE_ICOSAHEDRON, SHAPE_TORUS, SHAPES };

static const char *const shape_names[SHAPES] = {"Cube", "Octahedron", "Icosahedron", "Torus"};
static const uint8_t palette_rgb[4][3] = {{0x04, 0x08, 0x14}, {0x28, 0x48, 0x78}, {0x80, 0xE0, 0xFF}, {0xFF, 0xC0, 0x40}};

/* The worker's memory, shared by the UI and the check task. */
typedef struct {
	volatile bool armed; /* set by the UI once the seeds and expected are in */
	float seed[16];
	double dseed;
	uint64_t useed;
	uint32_t expected;
	volatile uint32_t runs, errors;
} check_t;

typedef struct {
	fw_overlay_t *ov;
	fw_worker_t *w;
	check_t *check; /* the worker's memory, once armed */
	lv_timer_t *timer;
	lv_obj_t *img, *info, *timing, *status;
	lv_img_dsc_t dsc;
	uint8_t shape, speed, ticks;
	bool paused;
	uint32_t frames;
	uint32_t last_tick;
	uint64_t frame_ticks; /* timer ticks spent drawing over the last `frames` frames */
	float ax, ay, az;
	uint8_t nverts, nedges;
	float verts[MAX_VERTS][3];
	float depth[MAX_VERTS];
	int16_t screen[MAX_VERTS][2];
	uint8_t edges[MAX_EDGES][2];
	uint8_t px[PALETTE_SIZE + STRIDE * IMG_H];
} app_t;

static uint32_t float_bits(float f)
{
	union {
		float f;
		uint32_t u;
	} v = {f};
	return v.u;
}

/* ---------------------------------------------------------------- FPU check */

#define CHECK_STEP(i, j) a##i = a##i * 0.75f + a##j * s * 0.25f + 0.125f;
#define CHECK_HASH(i) hash = hash * 31 + float_bits(a##i);

/* Deterministic float, double and 64-bit integer work on c's seeds, hashed. The rom_sinf call in
 * the loop makes the 16 accumulators live across a call, so they sit in s16-s31. Not inlined, so
 * the UI and the check task run the same code. */
static __attribute__((noinline)) uint32_t check_work(const check_t *c)
{
	float a0 = c->seed[0], a1 = c->seed[1], a2 = c->seed[2], a3 = c->seed[3];
	float a4 = c->seed[4], a5 = c->seed[5], a6 = c->seed[6], a7 = c->seed[7];
	float a8 = c->seed[8], a9 = c->seed[9], a10 = c->seed[10], a11 = c->seed[11];
	float a12 = c->seed[12], a13 = c->seed[13], a14 = c->seed[14], a15 = c->seed[15];
	for (int n = 0; n < CHECK_ITERS; n++) {
		float s = rom_sinf(a0 + a15);
		CHECK_STEP(0, 1) CHECK_STEP(1, 2) CHECK_STEP(2, 3) CHECK_STEP(3, 4)
		CHECK_STEP(4, 5) CHECK_STEP(5, 6) CHECK_STEP(6, 7) CHECK_STEP(7, 8)
		CHECK_STEP(8, 9) CHECK_STEP(9, 10) CHECK_STEP(10, 11) CHECK_STEP(11, 12)
		CHECK_STEP(12, 13) CHECK_STEP(13, 14) CHECK_STEP(14, 15) CHECK_STEP(15, 0)
	}
	uint32_t hash = 0;
	CHECK_HASH(0) CHECK_HASH(1) CHECK_HASH(2) CHECK_HASH(3) CHECK_HASH(4) CHECK_HASH(5)
	CHECK_HASH(6) CHECK_HASH(7) CHECK_HASH(8) CHECK_HASH(9) CHECK_HASH(10) CHECK_HASH(11)
	CHECK_HASH(12) CHECK_HASH(13) CHECK_HASH(14) CHECK_HASH(15)

	double d = c->dseed;
	uint64_t u = c->useed;
	for (int32_t n = 0; n < 32; n++) {
		d = d * 1.0001 + (double)n / 3.0;
		u = u * 6364136223846793005ull + 1442695040888963407ull;
		hash = hash * 31 + (uint32_t)(u / (uint64_t)(n + 3)) + (uint32_t)((int64_t)u / -7);
	}
	return hash * 31 + (uint32_t)d;
}

static void check_task(fw_worker_t *w)
{
	check_t *c = w->mem;
	while (!w->stop && !c->armed)
		fw_os_sleep(CHECK_PAUSE_MS);
	__sync_synchronize();
	while (!w->stop) {
		if (check_work(c) != c->expected)
			c->errors++;
		c->runs++;
		fw_os_sleep(CHECK_PAUSE_MS);
	}
}

/* Seeds the check and computes the expected result here on the UI's core; the task then starts. */
static void arm_check(check_t *c)
{
	for (int k = 0; k < 16; k++)
		c->seed[k] = (float)(k + 1) * 0.03125f;
	c->dseed = 1.5;
	c->useed = 0x0123456789ABCDEFull;
	c->expected = check_work(c);
	__sync_synchronize();
	c->armed = true;
}

/* ---------------------------------------------------------------- shapes */

static void add_vert(app_t *app, float x, float y, float z)
{
	float *v = app->verts[app->nverts++];
	v[0] = x;
	v[1] = y;
	v[2] = z;
}

static void add_edge(app_t *app, int a, int b)
{
	app->edges[app->nedges][0] = (uint8_t)a;
	app->edges[app->nedges][1] = (uint8_t)b;
	app->nedges++;
}

static float dist2(const float *a, const float *b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

/* Connects every pair of vertices at the shortest distance between any two: the edges of a
 * regular polyhedron. */
static void connect_nearest(app_t *app)
{
	float best = 1e9f;
	for (int i = 0; i < app->nverts; i++)
		for (int j = i + 1; j < app->nverts; j++) {
			float d = dist2(app->verts[i], app->verts[j]);
			best = d < best ? d : best;
		}
	for (int i = 0; i < app->nverts; i++)
		for (int j = i + 1; j < app->nverts; j++)
			if (dist2(app->verts[i], app->verts[j]) < best * 1.01f)
				add_edge(app, i, j);
}

/* Builds the current shape with its vertices on or within the unit sphere. */
static void build_shape(app_t *app)
{
	app->nverts = app->nedges = 0;
	switch (app->shape) {
	case SHAPE_CUBE: {
		const float s = 0.57735f; /* 1 / sqrt(3) */
		for (int i = 0; i < 8; i++)
			add_vert(app, i & 1 ? s : -s, i & 2 ? s : -s, i & 4 ? s : -s);
		connect_nearest(app);
		break;
	}
	case SHAPE_OCTAHEDRON:
		for (int i = 0; i < 6; i++) {
			float v[3] = {0, 0, 0};
			v[i / 2] = i & 1 ? 1.0f : -1.0f;
			add_vert(app, v[0], v[1], v[2]);
		}
		connect_nearest(app);
		break;
	case SHAPE_ICOSAHEDRON: {
		const float a = 0.52573f, b = 0.85065f; /* 1 and the golden ratio, scaled to radius 1 */
		for (int i = 0; i < 4; i++) {
			float p = i & 1 ? a : -a, q = i & 2 ? b : -b;
			add_vert(app, 0, p, q);
			add_vert(app, p, q, 0);
			add_vert(app, q, 0, p);
		}
		connect_nearest(app);
		break;
	}
	default: { /* torus: tube radius 0.3 around a ring of radius 0.7 */
		for (int i = 0; i < TORUS_MAJOR; i++) {
			float u = TWO_PI * (float)i / TORUS_MAJOR;
			for (int j = 0; j < TORUS_MINOR; j++) {
				float v = TWO_PI * (float)j / TORUS_MINOR;
				float r = 0.7f + 0.3f * rom_cosf(v);
				add_vert(app, r * rom_cosf(u), r * rom_sinf(u), 0.3f * rom_sinf(v));
			}
		}
		for (int i = 0; i < TORUS_MAJOR; i++)
			for (int j = 0; j < TORUS_MINOR; j++) {
				int k = i * TORUS_MINOR + j;
				add_edge(app, k, (i + 1) % TORUS_MAJOR * TORUS_MINOR + j);
				add_edge(app, k, i * TORUS_MINOR + (j + 1) % TORUS_MINOR);
			}
		break;
	}
	}
}

/* ---------------------------------------------------------------- drawing */

static void plot(uint8_t *px, int x, int y, uint8_t c)
{
	if ((unsigned)x >= IMG_W || (unsigned)y >= IMG_H)
		return;
	uint8_t *p = &px[y * STRIDE + x / 4];
	int shift = (3 - x % 4) * 2;
	*p = (uint8_t)((*p & ~(3 << shift)) | (c << shift));
}

static void line(uint8_t *px, int x0, int y0, int x1, int y1, uint8_t c)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		plot(px, x0, y0, c);
		if (x0 == x1 && y0 == y1)
			return;
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

static float wrap_angle(float a)
{
	return a >= TWO_PI ? a - TWO_PI : a;
}

/* Rotates and projects the vertices, then draws far edges, near edges and near vertices. */
static void render(app_t *app)
{
	float sx = rom_sinf(app->ax), cx = rom_cosf(app->ax);
	float sy = rom_sinf(app->ay), cy = rom_cosf(app->ay);
	float sz = rom_sinf(app->az), cz = rom_cosf(app->az);
	for (int i = 0; i < app->nverts; i++) {
		const float *v = app->verts[i];
		float y1 = v[1] * cx - v[2] * sx, z1 = v[1] * sx + v[2] * cx;
		float x2 = v[0] * cy + z1 * sy, z2 = z1 * cy - v[0] * sy;
		float x3 = x2 * cz - y1 * sz, y3 = x2 * sz + y1 * cz;
		float w = FOCAL / (z2 + CAMERA_DIST);
		app->depth[i] = z2;
		app->screen[i][0] = (int16_t)(IMG_W / 2 + x3 * w + 0.5f);
		app->screen[i][1] = (int16_t)(IMG_H / 2 - y3 * w + 0.5f);
	}
	uint8_t *px = app->px + PALETTE_SIZE;
	memset(px, 0, STRIDE * IMG_H);
	for (int pass = 0; pass < 2; pass++)
		for (int e = 0; e < app->nedges; e++) {
			int a = app->edges[e][0], b = app->edges[e][1];
			bool far = app->depth[a] + app->depth[b] > 0;
			if (far == (pass == 0))
				line(px, app->screen[a][0], app->screen[a][1], app->screen[b][0], app->screen[b][1],
				     far ? PAL_BACK : PAL_FRONT);
		}
	for (int i = 0; i < app->nverts; i++)
		if (app->depth[i] < 0) {
			int x = app->screen[i][0], y = app->screen[i][1];
			plot(px, x, y, PAL_VERTEX);
			plot(px, x + 1, y, PAL_VERTEX);
			plot(px, x, y + 1, PAL_VERTEX);
			plot(px, x + 1, y + 1, PAL_VERTEX);
		}
}

/* ---------------------------------------------------------------- UI */

static void update_info(app_t *app)
{
	char text[40];
	char *p = fw_put_str(text, shape_names[app->shape]);
	p = fw_put_str(p, "  x");
	p = fw_put_dec(p, 1u << app->speed);
	if (app->paused)
		fw_put_str(p, "  paused");
	lv_label_set_text(app->info, text);
}

static void update_stats(app_t *app)
{
	char text[40];
	if (app->frames) {
		uint64_t us = app->frame_ticks * 1000000u / ((uint64_t)FW_TIMER_HZ * app->frames);
		char *p = fw_put_str(text, "Frame ");
		p = fw_put_dec(p, (uint32_t)us);
		fw_put_str(p, " us");
		lv_label_set_text(app->timing, text);
		app->frame_ticks = 0;
		app->frames = 0;
	}
	check_t *c = app->check;
	if (!c) {
		c = fw_worker_mem(app->w);
		if (c) {
			arm_check(c);
			app->check = c;
		} else if (fw_worker_failed(app->w)) {
			lv_label_set_text(app->status, "FPU check: out of memory");
		}
		return;
	}
	uint32_t runs = c->runs, errors = c->errors;
	char *p;
	if (errors) {
		p = fw_put_str(text, "FPU: ");
		p = fw_put_dec(p, errors);
		p = fw_put_str(p, " bad of ");
	} else {
		p = fw_put_str(text, "FPU ok, ");
	}
	p = fw_put_dec(p, runs);
	fw_put_str(p, " runs");
	lv_label_set_text(app->status, text);
	lv_obj_set_style_text_color(app->status, errors ? lv_rgb(0xFF, 0x50, 0x40) : lv_rgb(0x60, 0xF0, 0x80), 0);
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	uint32_t elapsed = lv_tick_elaps(app->last_tick);
	app->last_tick += elapsed;
	if (!app->paused) {
		float step = BASE_SPIN * (float)(1u << app->speed) * (float)(elapsed < 100 ? elapsed : 100);
		app->ax = wrap_angle(app->ax + step);
		app->ay = wrap_angle(app->ay + step * 0.7f);
		app->az = wrap_angle(app->az + step * 0.3f);
		uint64_t t0 = rom_timer_read64();
		render(app);
		app->frame_ticks += rom_timer_read64() - t0;
		app->frames++;
		lv_obj_invalidate(app->img);
	}
	if (++app->ticks >= STATS_TICKS) {
		app->ticks = 0;
		update_stats(app);
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
	switch (key) {
	case FW_KEY_BACK:
	case FW_KEY_BACK_LONG:
	case LV_KEY_ESC:
		fw_overlay_close(app->ov); /* frees app via on_delete */
		return;
	case FW_KEY_LEFT:
		app->shape = (uint8_t)((app->shape + SHAPES - 1) % SHAPES);
		build_shape(app);
		break;
	case FW_KEY_RIGHT:
		app->shape = (uint8_t)((app->shape + 1) % SHAPES);
		build_shape(app);
		break;
	case FW_KEY_UP:
		app->speed = (uint8_t)((app->speed + 1) % SPEEDS);
		break;
	case FW_KEY_CENTER:
		app->paused = !app->paused;
		break;
	default:
		return;
	}
	if (app->paused) {
		render(app);
		lv_obj_invalidate(app->img);
	}
	update_info(app);
}

static void open(lv_obj_t *return_focus)
{
	app_t *app = lv_mem_alloc(sizeof(app_t));
	if (!app)
		return;
	memset(app, 0, sizeof(*app));
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
	app->dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_2BIT, IMG_W, IMG_H);
	app->dsc.data_size = sizeof(app->px);
	app->dsc.data = app->px;
	app->img = lv_img_create(root);
	lv_img_set_src(app->img, &app->dsc);
	lv_obj_set_pos(app->img, 0, 0);

	app->info = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_obj_set_pos(app->info, 2, (lv_coord_t)(h - 3 * LINE_H + 1));
	app->timing = fw_label(root, "Frame -", lv_rgb(0xC0, 0xC8, 0xD0));
	lv_obj_set_pos(app->timing, 2, (lv_coord_t)(h - 2 * LINE_H + 1));
	app->status = fw_label(root, "FPU check starting", lv_rgb(0xC0, 0xC8, 0xD0));
	lv_obj_set_pos(app->status, 2, (lv_coord_t)(h - LINE_H + 1));
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);

	app->w = fw_worker_start(check_task, sizeof(check_t), "fpucheck", CHECK_PRIORITY, CHECK_STACK);
	if (!app->w)
		lv_label_set_text(app->status, "FPU check: busy");
	build_shape(app);
	update_info(app);
	app->last_tick = lv_tick_get();
	app->timer = lv_timer_create(tick, TICK_MS, app);
}

FW_MENU_ITEM(FW_SYMBOL_PLAY, "FPU test", open);
