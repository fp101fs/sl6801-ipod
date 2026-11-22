/*
 * "Benchmark" entry in the APP menu: CPU clock, memory and flash throughput, and math speed.
 * Center runs it again, Back exits.
 *
 * A low-priority worker task on CPU0 runs the tests one by one and times each with the ROM's
 * 64-bit hardware timer; the UI (on CPU1) shows results as they come in. Firmware tasks still run
 * in between, so results vary a little from run to run. The CPU clock comes from the Cortex-M
 * cycle counter (DWT CYCCNT) against the timer, or from the SysTick reload if the cycle counter
 * doesn't run.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define BUF_SIZE 8192
#define COPY_ROUNDS 64        /* 512 KB per memory test */
#define FLASH_START 0x00E000  /* FIRM, in flash offsets */
#define FLASH_SPAN 0x080000   /* 512 KB */
#define CLOCK_SAMPLE_DIV 20   /* measure the clock for 1/20 s */
#define FLOAT_ITERS 200000
#define DOUBLE_ITERS 20000
#define SIN_ITERS 100000
#define DIV_ITERS 50000
#define TASK_PRIORITY 2
#define TASK_STACK 0x400 /* words */
#define POLL_MS 10
#define TICK_MS 100
#define LINE_H 15

#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_HZ 1000 /* the FreeRTOS tick */

enum { T_CLOCK, T_MEMCPY, T_BYTECOPY, T_MEMSET, T_FLASH, T_FLOAT, T_DOUBLE, T_SINF, T_DIV64, TESTS };

static const struct {
	const char *name, *unit;
} tests[TESTS] = {
	{"CPU clock", "MHz"},     {"memcpy", "MB/s"}, {"Byte copy", "MB/s"},
	{"memset", "MB/s"},       {"Flash read", "MB/s"}, {"float FMA", "MFLOPS"},
	{"double", "MFLOPS"},     {"rom_sinf", "M/s"},    {"64-bit div", "M/s"},
};

/* The worker's memory, shared by the UI and the bench task. */
typedef struct {
	volatile bool run;  /* a run is requested or in progress */
	volatile uint8_t current; /* test being run; TESTS when idle */
	volatile uint32_t version; /* bumped after each result */
	volatile uint32_t result[TESTS]; /* value x10, or 0 if not measured */
	volatile uint32_t run_ms;
	float fin[6];
	volatile float fout;
	volatile double dout;
	volatile uint64_t uout;
	uint8_t src[BUF_SIZE], dst[BUF_SIZE];
} bench_t;

typedef struct {
	fw_overlay_t *ov;
	fw_worker_t *w;
	bench_t *b; /* the worker's memory, once it runs */
	lv_timer_t *timer;
	lv_obj_t *names, *values, *status;
	uint32_t shown; /* b->version last shown */
} app_t;

/* ---------------------------------------------------------------- tests (bench task) */

static uint64_t elapsed_us(uint64_t t0)
{
	return (rom_timer_read64() - t0) * 1000000u / FW_TIMER_HZ;
}

/* Work units per microsecond (= millions per second), x10. */
static uint32_t rate_x10(uint64_t work, uint64_t t0)
{
	uint64_t us = elapsed_us(t0);
	return (uint32_t)(work * 10 / (us ? us : 1));
}

static uint32_t clock_x10(void)
{
	DEMCR |= DEMCR_TRCENA;
	DWT_CTRL |= 1;
	uint32_t c0 = DWT_CYCCNT;
	uint64_t t0 = rom_timer_read64();
	while (rom_timer_read64() - t0 < FW_TIMER_HZ / CLOCK_SAMPLE_DIV) {
	}
	uint32_t cycles = DWT_CYCCNT - c0;
	if (cycles == 0) /* no cycle counter: SysTick reloads every tick, at the CPU clock */
		return (SYST_RVR + 1) * SYSTICK_HZ / 100000;
	return rate_x10(cycles, t0);
}

static __attribute__((noinline)) void byte_copy(uint8_t *d, const uint8_t *s, size_t n)
{
	while (n--)
		*d++ = *s++;
}

static uint32_t memory_x10(bench_t *b, int test)
{
	uint64_t t0 = rom_timer_read64();
	if (test == T_FLASH) {
		for (uint32_t off = 0; off < FLASH_SPAN; off += BUF_SIZE)
			rom_memcpy(b->dst, (const void *)(FW_FLASH_BASE + FLASH_START + off), BUF_SIZE);
		return rate_x10(FLASH_SPAN, t0);
	}
	for (int r = 0; r < COPY_ROUNDS; r++) {
		if (test == T_MEMCPY)
			rom_memcpy(b->dst, b->src, BUF_SIZE);
		else if (test == T_BYTECOPY)
			byte_copy(b->dst, b->src, BUF_SIZE);
		else
			rom_memset(b->dst, r, BUF_SIZE);
	}
	return rate_x10((uint64_t)BUF_SIZE * COPY_ROUNDS, t0);
}

/* Four independent fused multiply-adds per iteration: 8 floating-point operations. */
static uint32_t float_x10(bench_t *b)
{
	float a0 = b->fin[0], a1 = b->fin[1], a2 = b->fin[2], a3 = b->fin[3], m = b->fin[4], c = b->fin[5];
	uint64_t t0 = rom_timer_read64();
	for (int i = 0; i < FLOAT_ITERS; i++) {
		a0 = __builtin_fmaf(a0, m, c);
		a1 = __builtin_fmaf(a1, m, c);
		a2 = __builtin_fmaf(a2, m, c);
		a3 = __builtin_fmaf(a3, m, c);
	}
	uint32_t r = rate_x10((uint64_t)FLOAT_ITERS * 8, t0);
	b->fout = a0 + a1 + a2 + a3;
	return r;
}

/* A multiply and an add per iteration, both in software through the ROM's helpers. */
static uint32_t double_x10(bench_t *b)
{
	double d = b->fin[0], m = b->fin[4], c = b->fin[5];
	uint64_t t0 = rom_timer_read64();
	for (int i = 0; i < DOUBLE_ITERS; i++)
		d = d * m + c;
	uint32_t r = rate_x10((uint64_t)DOUBLE_ITERS * 2, t0);
	b->dout = d;
	return r;
}

static uint32_t sinf_x10(bench_t *b)
{
	float x = b->fin[0], sum = 0;
	uint64_t t0 = rom_timer_read64();
	for (int i = 0; i < SIN_ITERS; i++) {
		sum += rom_sinf(x);
		x += 0.001f;
	}
	uint32_t r = rate_x10(SIN_ITERS, t0);
	b->fout = sum;
	return r;
}

static uint32_t div64_x10(bench_t *b)
{
	uint64_t num = 0x0123456789ABCDEFull, den = 1000003, sum = 0;
	uint64_t t0 = rom_timer_read64();
	for (int i = 0; i < DIV_ITERS; i++) {
		sum += num / den;
		num += 0x9E3779B97F4A7C15ull;
		den += 7;
	}
	uint32_t r = rate_x10(DIV_ITERS, t0);
	b->uout = sum;
	return r;
}

static void run_all(fw_worker_t *w, bench_t *b)
{
	uint64_t start = rom_timer_read64();
	for (int t = 0; t < TESTS; t++)
		b->result[t] = 0;
	b->version++;
	for (int t = 0; t < TESTS && !w->stop; t++) {
		b->current = (uint8_t)t;
		uint32_t r;
		switch (t) {
		case T_CLOCK:
			r = clock_x10();
			break;
		case T_FLOAT:
			r = float_x10(b);
			break;
		case T_DOUBLE:
			r = double_x10(b);
			break;
		case T_SINF:
			r = sinf_x10(b);
			break;
		case T_DIV64:
			r = div64_x10(b);
			break;
		default:
			r = memory_x10(b, t);
			break;
		}
		b->result[t] = r;
		b->version++;
		fw_os_sleep(1); /* let other tasks catch up */
	}
	b->run_ms = (uint32_t)(elapsed_us(start) / 1000);
	b->current = TESTS;
}

static void bench_task(fw_worker_t *w)
{
	bench_t *b = w->mem;
	for (int i = 0; i < 4; i++)
		b->fin[i] = 0.5f + 0.125f * (float)i;
	b->fin[4] = 0.9999f;
	b->fin[5] = 0.0001f;
	for (int i = 0; i < BUF_SIZE; i++)
		b->src[i] = (uint8_t)i;
	b->current = TESTS;
	b->run = true;
	while (!w->stop) {
		if (b->run) {
			run_all(w, b);
			b->run = false;
			b->version++;
		}
		fw_os_sleep(POLL_MS);
	}
}

/* ---------------------------------------------------------------- UI */

static void show(app_t *app)
{
	bench_t *b = app->b;
	char text[TESTS * 20];
	char *p = text;
	for (int t = 0; t < TESTS; t++) {
		if (t)
			*p++ = '\n';
		uint32_t r = b->result[t];
		if (r) {
			p = fw_put_dec(p, r / 10);
			p = fw_put_str(p, ".");
			p = fw_put_dec(p, r % 10);
			p = fw_put_str(p, " ");
			p = fw_put_str(p, tests[t].unit);
		} else {
			p = fw_put_str(p, b->run && t == b->current ? "running" : "-");
		}
	}
	*p = 0;
	lv_label_set_text(app->values, text);
	if (b->run) {
		p = fw_put_str(text, "Running ");
		p = fw_put_dec(p, b->current + 1u);
		p = fw_put_str(p, "/");
		fw_put_dec(p, TESTS);
	} else {
		p = fw_put_str(text, "Took ");
		p = fw_put_dec(p, b->run_ms);
		fw_put_str(p, " ms. Center: again");
	}
	lv_label_set_text(app->status, text);
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	if (!app->b) {
		app->b = fw_worker_mem(app->w);
		if (!app->b) {
			if (fw_worker_failed(app->w))
				lv_label_set_text(app->status, "Out of memory");
			return;
		}
		app->shown = app->b->version - 1;
	}
	uint32_t v = app->b->version;
	if (v != app->shown) {
		app->shown = v;
		show(app);
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
	case FW_KEY_CENTER:
		if (app->b && !app->b->run)
			app->b->run = true;
		return;
	default:
		return;
	}
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

	char text[TESTS * 12];
	char *p = text;
	for (int t = 0; t < TESTS; t++) {
		if (t)
			*p++ = '\n';
		p = fw_put_str(p, tests[t].name);
	}
	app->names = fw_label(root, text, lv_rgb(0x80, 0xA0, 0xC0));
	lv_obj_set_pos(app->names, 2, 0);
	app->values = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_obj_set_width(app->values, 74);
	lv_obj_set_style_text_align(app->values, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(app->values, 52, 0);
	app->status = fw_label(root, "", lv_rgb(0xC0, 0xC8, 0xD0));
	lv_obj_set_pos(app->status, 2, (lv_coord_t)(h - LINE_H));
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);

	app->w = fw_worker_start(bench_task, sizeof(bench_t), "bench", TASK_PRIORITY, TASK_STACK);
	if (!app->w) {
		lv_label_set_text(app->status, "Busy, try again");
		return;
	}
	app->timer = lv_timer_create(tick, TICK_MS, app);
}

FW_MENU_ITEM(FW_SYMBOL_BARS, "Benchmark", open);
