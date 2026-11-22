/*
 * "Pin monitor" entry in the APP menu: the input level of all 128 GPIO pins (4 ports of 32), read
 * with the ROM's rom_gpio_get 50 times a second. Each port takes two rows of 16 cells, pin 0 at
 * the top left; high pins are green, low ones dark, and a pin that just changed gets a yellow
 * frame. Plugging in USB or inserting a card shows which pins they use. Of the buttons only
 * Center (the power key) shows up; the others are read through the ADC. Left/Right move the
 * cursor, Up and Back move it a row up and down, Center clears the change counts, holding Back
 * exits.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define PORTS 4
#define PINS 32
#define ALL_PINS (PORTS * PINS)
#define PER_ROW 16
#define CELL_W 8 /* pitch; 7 px cell + 1 px gap */
#define CELL_H 11
#define ROW_PITCH 13
#define PORT_GAP 4
#define IMG_W (PER_ROW * CELL_W)
#define IMG_H (PORTS * 2 * ROW_PITCH + (PORTS - 1) * PORT_GAP)
#define STRIDE (IMG_W / 2)
#define PALETTE_SIZE (16 * 4)
#define PX_SIZE (PALETTE_SIZE + STRIDE * IMG_H)
#define LINE_H 15
#define POLL_MS 20
#define RECENT_TICKS 75 /* 1.5 s */

enum { PAL_BG, PAL_LOW, PAL_HIGH, PAL_RECENT, PAL_CURSOR, PAL_COUNT };

static const uint8_t palette_rgb[PAL_COUNT][3] = {
	{0x00, 0x00, 0x00}, {0x20, 0x28, 0x38}, {0x30, 0xE0, 0x60}, {0xFF, 0xD0, 0x30}, {0xFF, 0xFF, 0xFF},
};

typedef struct {
	fw_overlay_t *ov;
	lv_timer_t *timer;
	lv_obj_t *img, *cursor_info, *last_info;
	lv_img_dsc_t dsc;
	uint8_t cursor, last;
	bool have_last;
	uint32_t level[PORTS];
	uint16_t changes[ALL_PINS];
	uint8_t recent[ALL_PINS];
	fw_worker_t *w; /* memory only: the image, sparing the 40 KB UI heap */
	uint8_t *px;    /* the worker's PX_SIZE bytes, once allocated */
} app_t;

static uint32_t pin_id(int pin)
{
	return (uint32_t)(pin / PINS) << 16 | (uint32_t)(pin % PINS) << 11;
}

static bool pin_level(const app_t *app, int pin)
{
	return app->level[pin / PINS] >> (pin % PINS) & 1;
}

static char *put_pin(char *p, int pin)
{
	p = fw_put_str(p, "P");
	p = fw_put_dec(p, (uint32_t)(pin / PINS));
	p = fw_put_str(p, ".");
	if (pin % PINS < 10)
		p = fw_put_str(p, "0");
	return fw_put_dec(p, (uint32_t)(pin % PINS));
}

static void fill(app_t *app, int x0, int y0, int w, int h, uint8_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++) {
			uint8_t *p = &app->px[PALETTE_SIZE + y * STRIDE + x / 2];
			*p = (uint8_t)(x & 1 ? (*p & 0xF0) | c : (*p & 0x0F) | c << 4);
		}
}

static void draw_cell(app_t *app, int pin)
{
	int row = pin / PER_ROW;
	int x = pin % PER_ROW * CELL_W, y = row * ROW_PITCH + row / 2 * PORT_GAP;
	int w = CELL_W - 1;
	fill(app, x, y, w, CELL_H, pin_level(app, pin) ? PAL_HIGH : PAL_LOW);
	uint8_t frame = pin == app->cursor ? PAL_CURSOR : app->recent[pin] ? PAL_RECENT : 0;
	if (frame) {
		fill(app, x, y, w, 1, frame);
		fill(app, x, y + CELL_H - 1, w, 1, frame);
		fill(app, x, y, 1, CELL_H, frame);
		fill(app, x + w - 1, y, 1, CELL_H, frame);
	}
}

static void update_text(app_t *app)
{
	char text[48];
	char *p = put_pin(text, app->cursor);
	p = fw_put_str(p, pin_level(app, app->cursor) ? " high, " : " low, ");
	p = fw_put_dec(p, app->changes[app->cursor]);
	fw_put_str(p, app->changes[app->cursor] == 1 ? " change" : " changes");
	lv_label_set_text(app->cursor_info, text);
	if (app->have_last) {
		p = fw_put_str(text, "Last: ");
		p = put_pin(p, app->last);
		fw_put_str(p, pin_level(app, app->last) ? " went high" : " went low");
		lv_label_set_text(app->last_info, text);
	}
}

/* Reads all pins into one bit mask per port. */
static void read_pins(uint32_t *level)
{
	for (int port = 0; port < PORTS; port++) {
		uint32_t v = 0;
		for (int pin = 0; pin < PINS; pin++)
			if (rom_gpio_get(pin_id(port * PINS + pin)))
				v |= 1u << pin;
		level[port] = v;
	}
}

static void start_view(app_t *app)
{
	for (int i = 0; i < PAL_COUNT; i++) {
		app->px[i * 4 + 0] = palette_rgb[i][2];
		app->px[i * 4 + 1] = palette_rgb[i][1];
		app->px[i * 4 + 2] = palette_rgb[i][0];
		app->px[i * 4 + 3] = 0xFF;
	}
	read_pins(app->level);
	for (int pin = 0; pin < ALL_PINS; pin++)
		draw_cell(app, pin);
	app->dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_4BIT, IMG_W, IMG_H);
	app->dsc.data_size = PX_SIZE;
	app->dsc.data = app->px;
	app->img = lv_img_create(app->ov->root);
	lv_img_set_src(app->img, &app->dsc);
	lv_obj_set_pos(app->img, 0, 1);
	update_text(app);
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	if (!app->px) {
		app->px = fw_worker_mem(app->w);
		if (app->px)
			start_view(app);
		else if (fw_worker_failed(app->w))
			lv_label_set_text(app->cursor_info, "Out of memory");
		return;
	}
	uint32_t now[PORTS];
	read_pins(now);
	bool dirty = false, text = false;
	for (int pin = 0; pin < ALL_PINS; pin++) {
		uint32_t bit = 1u << (pin % PINS);
		if ((now[pin / PINS] ^ app->level[pin / PINS]) & bit) {
			app->level[pin / PINS] ^= bit;
			if (app->changes[pin] < 0xFFFF)
				app->changes[pin]++;
			app->recent[pin] = RECENT_TICKS;
			app->last = (uint8_t)pin;
			app->have_last = true;
			draw_cell(app, pin);
			dirty = text = true;
		} else if (app->recent[pin] && --app->recent[pin] == 0) {
			draw_cell(app, pin);
			dirty = true;
		}
	}
	if (dirty)
		lv_obj_invalidate(app->img);
	if (text)
		update_text(app);
}

static void on_delete(void *ctx)
{
	app_t *app = ctx;
	if (app->timer)
		lv_timer_del(app->timer);
	fw_worker_release(app->w);
	lv_mem_free(app);
}

static void move_cursor(app_t *app, int to)
{
	int old = app->cursor;
	app->cursor = (uint8_t)((to + ALL_PINS) % ALL_PINS);
	draw_cell(app, old);
	draw_cell(app, app->cursor);
	lv_obj_invalidate(app->img);
}

static void key_cb(lv_event_t *e)
{
	app_t *app = lv_event_get_user_data(e);
	uint32_t key = *(uint32_t *)lv_event_get_param(e);
	if (!app->px) { /* no image yet (or out of memory): only leaving works */
		if (key == FW_KEY_BACK || key == FW_KEY_BACK_LONG || key == LV_KEY_ESC)
			fw_overlay_close(app->ov);
		return;
	}
	switch (key) {
	case FW_KEY_BACK_LONG:
	case LV_KEY_ESC:
		fw_overlay_close(app->ov); /* frees app via on_delete */
		return;
	case FW_KEY_BACK:
		move_cursor(app, app->cursor + PER_ROW);
		break;
	case FW_KEY_LEFT:
	case FW_KEY_LEFT_REPEAT:
		move_cursor(app, app->cursor - 1);
		break;
	case FW_KEY_RIGHT:
	case FW_KEY_RIGHT_REPEAT:
		move_cursor(app, app->cursor + 1);
		break;
	case FW_KEY_UP:
	case FW_KEY_UP_REPEAT:
		move_cursor(app, app->cursor - PER_ROW);
		break;
	case FW_KEY_CENTER:
		memset(app->changes, 0, sizeof(app->changes));
		break;
	default:
		return;
	}
	update_text(app);
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
	app->cursor_info = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_obj_set_pos(app->cursor_info, 2, (lv_coord_t)(h - 2 * LINE_H));
	app->last_info = fw_label(root, "Last: no changes yet", lv_rgb(0xC0, 0xC8, 0xD0));
	lv_obj_set_pos(app->last_info, 2, (lv_coord_t)(h - LINE_H));
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);

	app->w = fw_worker_start(NULL, PX_SIZE, NULL, 0, 0);
	if (!app->w) {
		lv_label_set_text(app->cursor_info, "Busy, try again");
		return;
	}
	app->timer = lv_timer_create(tick, POLL_MS, app);
}

FW_MENU_ITEM(FW_SYMBOL_BARS, "Pin monitor", open);
