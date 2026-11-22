/*
 * "SD map" and "Flash map" entries in the APP menu: show the SD card's 512-byte sectors or the
 * flash's 4 KB sectors, 256 per page as a 16x16 grid, each square colored by the Shannon entropy
 * of the block's bytes on a hue scale from blue (all bytes equal) through cyan, green and yellow
 * to red (8 bits per byte; random data reaches about 7.6). A read error shows white.
 * Left/Right page through the device (hold to repeat), Up jumps 1/16 of it ahead, Back exits.
 *
 * A worker task on CPU0 reads the requested page and stores a color per block (the UI runs on
 * CPU1 and must not touch the card); a UI timer draws the colors. The flash is memory mapped and
 * read the same way, for simplicity.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define COLS 16
#define ROWS 16
#define PAGE (COLS * ROWS)
#define CELL 8 /* 7 px square and a 1 px gap */
#define GRID_W (COLS * CELL)
#define GRID_H (ROWS * CELL)
#define PALETTE_SIZE (256 * 4)
#define SD_SHIFT 9     /* 512-byte sectors */
#define FLASH_SHIFT 12 /* FW_FLASH_SECTOR */
#define TICK_MS 30
#define TASK_PRIORITY 4
#define TASK_STACK 0x400 /* words */
#define ROW_PAUSE_MS 2 /* the task pauses after each row, so the card stays free for others */
#define IDLE_POLL_MS 10

enum { PAL_GAP, PAL_UNREAD, PAL_ERROR, PAL_RAMP }; /* PAL_RAMP..255: entropy 0..8 bits */
#define RAMP_LEVELS (256 - PAL_RAMP)
enum { SCAN_STARTING, SCAN_READY, SCAN_NO_CARD, SCAN_NO_SIZE };

/* The worker's memory, shared by the UI and the reader task. */
typedef struct {
	volatile uint8_t status; /* SCAN_*; the fields below are set once it is SCAN_READY */
	void *sd;                /* NULL: read the flash */
	uint8_t shift;           /* log2 of the block size */
	uint32_t blocks;
	volatile uint32_t page;     /* written by the UI */
	volatile uint32_t progress; /* bumped by the task after each change to color[] */
	uint8_t color[PAGE];
	uint8_t sector[1 << SD_SHIFT];
	uint16_t hist[256];
	uint8_t px[PALETTE_SIZE + GRID_W * GRID_H]; /* used by the UI only */
} scan_t;

typedef struct {
	fw_overlay_t *ov;
	fw_worker_t *w;
	scan_t *scan; /* the worker's memory, once it is ready */
	lv_timer_t *timer;
	lv_obj_t *img, *title, *info;
	lv_img_dsc_t dsc;
	uint32_t pages, page;
	uint32_t drawn; /* scan->progress when the grid was last drawn */
} app_t;

/* Ramp stops: entropy position (0-255) and color, a hue sweep from blue to red. */
static const uint8_t stops[][4] = {{0, 0x00, 0x00, 0xFF},   {64, 0x00, 0xFF, 0xFF}, {128, 0x00, 0xFF, 0x00},
				   {191, 0xFF, 0xFF, 0x00}, {255, 0xFF, 0x00, 0x00}};

static void set_color(uint8_t *pal, int i, uint8_t r, uint8_t g, uint8_t b)
{
	pal[i * 4 + 0] = b;
	pal[i * 4 + 1] = g;
	pal[i * 4 + 2] = r;
	pal[i * 4 + 3] = 0xFF;
}

static void set_palette(uint8_t *pal)
{
	set_color(pal, PAL_GAP, 0x00, 0x00, 0x00);
	set_color(pal, PAL_UNREAD, 0x28, 0x28, 0x28);
	set_color(pal, PAL_ERROR, 0xFF, 0xFF, 0xFF);
	for (int i = 0; i < RAMP_LEVELS; i++) {
		int t = i * 255 / (RAMP_LEVELS - 1), s = 0;
		while (t > stops[s + 1][0])
			s++;
		const uint8_t *a = stops[s], *b = stops[s + 1];
		int f = (t - a[0]) * 256 / (b[0] - a[0]);
		uint8_t c[3];
		for (int k = 0; k < 3; k++)
			c[k] = (uint8_t)(a[k + 1] + (b[k + 1] - a[k + 1]) * f / 256);
		set_color(pal, PAL_RAMP + i, c[0], c[1], c[2]);
	}
}

/* ---------------------------------------------------------------- reader task */

/* log2(x) in Q16, for x >= 1. */
static uint32_t log2_q16(uint32_t x)
{
	uint32_t ip = 31 - (uint32_t)__builtin_clz(x);
	uint64_t y = (uint64_t)x << (31 - ip); /* x / 2^ip in Q31, in [1, 2) */
	uint32_t frac = 0;
	for (uint32_t bit = 1u << 15; bit; bit >>= 1) {
		y = (y * y) >> 31;
		if (y >= (2ull << 31)) {
			frac |= bit;
			y >>= 1;
		}
	}
	return (ip << 16) | frac;
}

/* Shannon entropy of a block's bytes in bits per byte, Q16 (0 to 8 << 16). */
static uint32_t entropy_q16(scan_t *s, const uint8_t *data)
{
	memset(s->hist, 0, sizeof(s->hist));
	for (uint32_t i = 0; i < 1u << s->shift; i++)
		s->hist[data[i]]++;
	uint32_t sum = 0; /* sum of c * log2(c), Q16; at most 4096 * 12 << 16 */
	for (int v = 0; v < 256; v++)
		if (s->hist[v] > 1)
			sum += s->hist[v] * log2_q16(s->hist[v]);
	return ((uint32_t)s->shift << 16) - (sum >> s->shift);
}

static uint8_t block_color(scan_t *s, uint32_t block)
{
	if (block >= s->blocks)
		return PAL_GAP;
	const uint8_t *data;
	if (s->sd) {
		if (fw_dev_read(s->sd, s->sector, 1, &block) != 1)
			return PAL_ERROR;
		data = s->sector;
	} else {
		data = (const uint8_t *)(uintptr_t)(FW_FLASH_BASE + (block << s->shift));
	}
	return (uint8_t)(PAL_RAMP + entropy_q16(s, data) * (RAMP_LEVELS - 1) / (8u << 16));
}

static void scan(fw_worker_t *w, bool flash)
{
	scan_t *s = w->mem;
	if (flash) {
		s->shift = FLASH_SHIFT;
		s->blocks = FW_FLASH_SIZE / FW_FLASH_SECTOR;
	} else {
		s->sd = FW_SD_NODE;
		if (!fw_is_ram(s->sd)) {
			s->status = SCAN_NO_CARD;
			return;
		}
		s->shift = SD_SHIFT;
		fw_sd_ioctl(FW_SD_SECTOR_COUNT, &s->blocks);
		if (!s->blocks) {
			s->status = SCAN_NO_SIZE;
			return;
		}
	}
	memset(s->color, PAL_UNREAD, PAGE);
	__sync_synchronize();
	s->status = SCAN_READY;
	uint32_t page = 0, next = PAGE + 1; /* next block of the page to read; PAGE + 1: start over */
	while (!w->stop) {
		if (s->page != page || next > PAGE) {
			page = s->page;
			memset(s->color, PAL_UNREAD, PAGE);
			next = 0;
			s->progress++;
		}
		if (next == PAGE) {
			fw_os_sleep(IDLE_POLL_MS);
			continue;
		}
		for (int col = 0; col < COLS && s->page == page && !w->stop; col++, next++) {
			s->color[next] = block_color(s, page * PAGE + next);
			s->progress++;
		}
		fw_os_sleep(ROW_PAUSE_MS);
	}
}

static void scan_sd(fw_worker_t *w)
{
	scan(w, false);
}

static void scan_flash(fw_worker_t *w)
{
	scan(w, true);
}

/* ---------------------------------------------------------------- UI */

static void fill_cell(app_t *app, int index, uint8_t color)
{
	uint8_t *p = app->scan->px + PALETTE_SIZE + (index / COLS) * CELL * GRID_W + (index % COLS) * CELL;
	for (int y = 0; y < CELL - 1; y++)
		memset(p + y * GRID_W, color, CELL - 1);
}

static char *put_size(char *p, uint32_t kb)
{
	if (kb < 1024 * 1024) {
		p = fw_put_dec(p, kb / 1024);
		return fw_put_str(p, " MB");
	}
	uint32_t tenths = kb / 104858; /* 2^20 KB per GB / 10 */
	p = fw_put_dec(p, tenths / 10);
	p = fw_put_str(p, ".");
	p = fw_put_dec(p, tenths % 10);
	return fw_put_str(p, " GB");
}

static void show_page(app_t *app)
{
	scan_t *s = app->scan;
	uint32_t first = app->page * PAGE;
	char text[40];
	char *p;
	if (s->sd) {
		p = fw_put_str(text, "Sector ");
		fw_put_dec(p, first);
	} else {
		p = fw_put_str(text, "Offset 0x");
		fw_put_hex(p, first << s->shift, 6);
	}
	lv_label_set_text(app->title, text);
	p = fw_put_dec(text, app->page + 1);
	p = fw_put_str(p, "/");
	p = fw_put_dec(p, app->pages);
	p = fw_put_str(p, " ");
	put_size(p, s->shift >= 10 ? s->blocks << (s->shift - 10) : s->blocks >> (10 - s->shift));
	lv_label_set_text(app->info, text);
	s->page = app->page;
}

/* Sets up the grid once the worker has found the device's size. */
static void start_view(app_t *app, scan_t *s)
{
	app->scan = s;
	app->pages = (s->blocks + PAGE - 1) / PAGE;
	uint8_t *px = s->px;
	set_palette(px);
	memset(px + PALETTE_SIZE, PAL_GAP, GRID_W * GRID_H);
	app->dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_8BIT, GRID_W, GRID_H);
	app->dsc.data_size = sizeof(s->px);
	app->dsc.data = px;
	app->img = lv_img_create(app->ov->root);
	lv_img_set_src(app->img, &app->dsc);
	lv_obj_set_pos(app->img, 0, (lv_coord_t)((lv_disp_get_ver_res(NULL) - GRID_H) / 2));
	app->drawn = s->progress - 1; /* draws the first frame */
	show_page(app);
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	if (!app->scan) {
		scan_t *s = fw_worker_mem(app->w);
		if (fw_worker_failed(app->w))
			lv_label_set_text(app->title, "Out of memory");
		else if (s && s->status == SCAN_NO_CARD)
			lv_label_set_text(app->title, "No SD card");
		else if (s && s->status == SCAN_NO_SIZE)
			lv_label_set_text(app->title, "SD card size unknown");
		else if (s && s->status == SCAN_READY)
			start_view(app, s);
		return;
	}
	uint32_t progress = app->scan->progress;
	if (progress == app->drawn)
		return;
	app->drawn = progress;
	for (int i = 0; i < PAGE; i++)
		fill_cell(app, i, app->scan->color[i]);
	lv_obj_invalidate(app->img);
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
	uint32_t page = app->page;
	switch (key) {
	case FW_KEY_BACK:
	case LV_KEY_ESC:
		fw_overlay_close(app->ov); /* frees app via on_delete */
		return;
	case FW_KEY_LEFT:
	case FW_KEY_LEFT_LONG:
	case FW_KEY_LEFT_REPEAT:
		page = (page ? page : app->pages) - 1;
		break;
	case FW_KEY_RIGHT:
	case FW_KEY_RIGHT_LONG:
	case FW_KEY_RIGHT_REPEAT:
		page = page + 1 < app->pages ? page + 1 : 0;
		break;
	case FW_KEY_UP: {
		uint32_t step = app->pages / 16 ? app->pages / 16 : 1;
		page = page + step < app->pages ? page + step : 0;
		break;
	}
	default:
		return;
	}
	if (app->scan && page != app->page) {
		app->page = page;
		show_page(app);
	}
}

static void open_map(lv_obj_t *return_focus, bool flash)
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
	app->title = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_obj_set_pos(app->title, 2, 1);
	app->info = fw_label(root, "", lv_rgb(0xA0, 0xB0, 0xC0));
	lv_obj_set_pos(app->info, 2, (lv_coord_t)(h - 15));
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);

	app->w = fw_worker_start(flash ? scan_flash : scan_sd, sizeof(scan_t), "blockmap", TASK_PRIORITY, TASK_STACK);
	if (!app->w) {
		lv_label_set_text(app->title, "Busy, try again");
		return;
	}
	app->timer = lv_timer_create(tick, TICK_MS, app);
}

static void open_sd(lv_obj_t *return_focus)
{
	open_map(return_focus, false);
}

static void open_flash(lv_obj_t *return_focus)
{
	open_map(return_focus, true);
}

FW_MENU_ITEM(FW_SYMBOL_SAVE, "SD map", open_sd);
FW_MENU_ITEM(FW_SYMBOL_SAVE, "Flash map", open_flash);
