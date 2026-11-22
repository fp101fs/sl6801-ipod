/*
 * "Files" entry in the APP menu: a browser for the SD card with a hex and text viewer.
 * In the list, Left/Right move the selection (hold to repeat), Up jumps a page, Center opens a
 * folder or file, Back goes up a folder (or exits at the top). In the viewer, Left/Right page
 * through the file, Up jumps ahead 1/16 of it, Center switches between hex and text, Back
 * returns to the list. Holding Back exits from anywhere.
 *
 * A worker task on CPU0 does all FatFs calls (the UI runs on CPU1 and must not touch the card):
 * the UI writes a request into the worker's memory, and shows the result once the task bumps
 * `finished`. Each read opens, seeks, reads and closes the file, so nothing stays open if the card
 * goes away. Folders are listed into a pool of names and sorted, folders first.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define PATH_SIZE 512
#define MAX_ENTRIES 400
#define POOL_SIZE 12288
#define READ_SIZE 512
#define TASK_PRIORITY 2
#define TASK_STACK 0x400 /* words */
#define CALL_HEAP_MIN 0x2000 /* free heap needed before FatFs calls (each takes 0x460) */
#define POLL_MS 5
#define TICK_MS 50
#define LINE_H 15
#define ROWS 8
#define LIST_Y LINE_H
#define NAME_W 92
#define SIZE_X 92
#define SIZE_W 34
#define TITLE_CHARS 22 /* longer paths show their end */

/* Hex view: 14 rows of 8 bytes in a 5x7 font, bytes alternating in two colors. */
#define HEX_ROWS 14
#define HEX_PER_ROW 8
#define HEX_PAGE (HEX_ROWS * HEX_PER_ROW)
#define HEX_ROW_H 9
#define HEX_W 128
#define HEX_H (HEX_ROWS * HEX_ROW_H)
#define HEX_STRIDE (HEX_W / 4)
#define HEX_PALETTE (4 * 4)
#define DIGIT_W 6  /* 5 px glyph + 1 */
#define BYTE_W 14  /* two digits + 3 */
#define HALF_GAP 4 /* between bytes 3 and 4 */
#define TEXT_STEP 160

enum { REQ_NONE, REQ_LIST, REQ_READ };
enum { MODE_LIST, MODE_HEX, MODE_TEXT };
enum { PAL_BG, PAL_EVEN, PAL_ODD };

static const uint8_t hex_glyphs[16][7] = {
	{0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
	{0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
	{0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
	{0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
	{0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
	{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
	{0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
	{0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
};
static const uint8_t palette_rgb[3][3] = {{0x00, 0x00, 0x00}, {0xF0, 0xF0, 0xF0}, {0x70, 0xC8, 0xFF}};

typedef struct {
	const char *name; /* in the pool */
	uint64_t size;
	bool dir;
} entry_t;

/* The worker's memory, shared by the UI and the file task. The UI writes the request fields only
 * while the task is idle (req == REQ_NONE and !busy), and reads results only then. */
typedef struct {
	volatile uint8_t req;
	volatile bool busy;
	volatile uint32_t finished; /* bumped after each request */
	int result;                 /* FRESULT of the last request */
	char dir_path[PATH_SIZE];
	char file_path[PATH_SIZE];
	uint64_t read_pos;
	/* REQ_LIST results */
	uint16_t count;
	bool truncated;
	uint32_t pool_used;
	entry_t entries[MAX_ENTRIES];
	char pool[POOL_SIZE];
	/* REQ_READ results */
	uint64_t file_size;
	uint32_t read_len;
	uint8_t data[READ_SIZE];
	fw_dir_t dir;
	fw_file_t file;
	fw_fileinfo_t info;
} fs_t;

typedef struct {
	fw_overlay_t *ov;
	fw_worker_t *w;
	fs_t *fs; /* the worker's memory, once it runs */
	lv_timer_t *timer;
	lv_obj_t *title, *footer, *bar, *names[ROWS], *sizes[ROWS], *hex, *text;
	lv_img_dsc_t dsc;
	uint8_t mode;
	bool waiting; /* a request is out */
	uint32_t seen; /* fs->finished when last handled */
	uint16_t sel;
	uint64_t pos; /* viewer offset */
	char reselect[256]; /* folder to select after going up */
	char text_buf[READ_SIZE + 1];
	uint8_t px[HEX_PALETTE + HEX_STRIDE * HEX_H];
} app_t;

/* ---------------------------------------------------------------- file task */

static char fold(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int by_name(const void *a, const void *b)
{
	const entry_t *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	const char *p = x->name, *q = y->name;
	while (*p && fold(*p) == fold(*q))
		p++, q++;
	return (uint8_t)fold(*p) - (uint8_t)fold(*q);
}

static int list_dir(fs_t *fs)
{
	fs->count = 0;
	fs->pool_used = 0;
	fs->truncated = false;
	int r = f_opendir(&fs->dir, fs->dir_path);
	if (r != FR_OK)
		return r;
	for (;;) {
		r = f_readdir(&fs->dir, &fs->info);
		if (r != FR_OK || !fs->info.name[0])
			break;
		uint32_t n = (uint32_t)rom_strlen(fs->info.name) + 1;
		if (fs->count == MAX_ENTRIES || fs->pool_used + n > POOL_SIZE) {
			fs->truncated = true;
			break;
		}
		char *name = fs->pool + fs->pool_used;
		rom_memcpy(name, fs->info.name, n);
		fs->pool_used += n;
		entry_t *e = &fs->entries[fs->count++];
		e->name = name;
		e->size = fs->info.size;
		e->dir = (fs->info.attrib & AM_DIR) != 0;
	}
	f_closedir(&fs->dir);
	rom_qsort(fs->entries, fs->count, sizeof(entry_t), by_name);
	return r;
}

static int read_file(fs_t *fs)
{
	fs->read_len = 0;
	int r = f_open(&fs->file, fs->file_path, FA_READ);
	if (r != FR_OK)
		return r;
	fs->file_size = fw_file_size(&fs->file);
	r = f_lseek(&fs->file, fs->read_pos);
	if (r == FR_OK)
		r = f_read(&fs->file, fs->data, READ_SIZE, &fs->read_len);
	f_close(&fs->file);
	return r;
}

static void file_task(fw_worker_t *w)
{
	fs_t *fs = w->mem;
	while (!w->stop) {
		uint8_t req = fs->req;
		if (req != REQ_NONE) {
			fs->busy = true;
			fs->req = REQ_NONE;
			if (!fw_sd_mounted())
				fs->result = FR_NOT_ENABLED;
			else if (FW_HEAP_FREE < CALL_HEAP_MIN) /* see fw.h: FatFs crashes when oal_malloc fails */
				fs->result = FR_NOT_ENOUGH_CORE;
			else
				fs->result = req == REQ_LIST ? list_dir(fs) : read_file(fs);
			fs->busy = false;
			__sync_synchronize();
			fs->finished++;
		}
		fw_os_sleep(POLL_MS);
	}
}

/* ---------------------------------------------------------------- helpers */

static char *put_size(char *p, uint64_t size)
{
	static const char units[] = "KMG";
	if (size < 1000)
		return fw_put_dec(p, (uint32_t)size);
	int u = 0;
	uint64_t scaled = (size * 10 + 512) / 1024; /* tenths, rounded */
	while (scaled >= 10000 && u < 2) {
		scaled = (scaled + 512) / 1024;
		u++;
	}
	p = fw_put_dec(p, (uint32_t)(scaled / 10));
	if (scaled < 100) {
		p = fw_put_str(p, ".");
		p = fw_put_dec(p, (uint32_t)(scaled % 10));
	}
	*p++ = units[u];
	*p = 0;
	return p;
}

/* Copies the end of s into p (at most `chars` bytes, starting on a UTF-8 character). */
static char *put_tail(char *p, const char *s, uint32_t chars)
{
	uint32_t n = (uint32_t)rom_strlen(s);
	if (n <= chars)
		return fw_put_str(p, s);
	s += n - chars;
	while ((*s & 0xC0) == 0x80)
		s++;
	p = fw_put_str(p, "...");
	return fw_put_str(p, s);
}

static const char *error_text(int r)
{
	switch (r) {
	case FR_NOT_ENABLED:
	case FR_INVALID_OBJECT:
		return "No card";
	case FR_NO_FILESYSTEM:
		return "No file system";
	case FR_NO_FILE:
	case FR_NO_PATH:
		return "Not found";
	case FR_NOT_ENOUGH_CORE:
		return "Low memory";
	default:
		return "Read error";
	}
}

/* ---------------------------------------------------------------- list view */

static void show_list(app_t *app)
{
	fs_t *fs = app->fs;
	char text[64];
	put_tail(text, fs->dir_path, TITLE_CHARS);
	lv_label_set_text(app->title, text);
	uint16_t top = (uint16_t)(app->sel / ROWS * ROWS);
	for (int i = 0; i < ROWS; i++) {
		uint16_t k = (uint16_t)(top + i);
		bool selected = k == app->sel && k < fs->count;
		lv_label_set_long_mode(app->names[i], selected ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP);
		if (k >= fs->count) {
			lv_label_set_text(app->names[i], "");
			lv_label_set_text(app->sizes[i], "");
			continue;
		}
		const entry_t *e = &fs->entries[k];
		lv_label_set_text(app->names[i], e->name);
		lv_obj_set_style_text_color(app->names[i], e->dir ? lv_rgb(0xFF, 0xD0, 0x60) : lv_rgb(0xFF, 0xFF, 0xFF), 0);
		if (e->dir)
			text[0] = 0;
		else
			put_size(text, e->size);
		lv_label_set_text(app->sizes[i], text);
		if (selected)
			lv_obj_set_pos(app->bar, 0, (lv_coord_t)(LIST_Y + i * LINE_H));
	}
	if (fs->count)
		lv_obj_clear_flag(app->bar, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(app->bar, LV_OBJ_FLAG_HIDDEN);

	char *p = text;
	if (fs->result != FR_OK) {
		p = fw_put_str(p, error_text(fs->result));
	} else if (!fs->count) {
		p = fw_put_str(p, "Empty folder");
	} else {
		p = fw_put_dec(p, app->sel + 1u);
		p = fw_put_str(p, "/");
		p = fw_put_dec(p, fs->count);
		if (fs->truncated)
			p = fw_put_str(p, "+");
	}
	lv_label_set_text(app->footer, text);
}

/* ---------------------------------------------------------------- viewer */

static void plot(uint8_t *px, int x, int y, uint8_t c)
{
	uint8_t *p = &px[y * HEX_STRIDE + x / 4];
	int shift = (3 - x % 4) * 2;
	*p = (uint8_t)((*p & ~(3 << shift)) | (c << shift));
}

static void draw_digit(uint8_t *px, int x, int y, int d, uint8_t c)
{
	for (int row = 0; row < 7; row++)
		for (int col = 0; col < 5; col++)
			if (hex_glyphs[d][row] & (0x10 >> col))
				plot(px, x + col, y + row, c);
}

static void draw_hex(app_t *app, const uint8_t *data, uint32_t len)
{
	uint8_t *px = app->px + HEX_PALETTE;
	memset(px, 0, HEX_STRIDE * HEX_H);
	for (uint32_t i = 0; i < len && i < HEX_PAGE; i++) {
		int col = (int)(i % HEX_PER_ROW), x = 2 + col * BYTE_W + (col >= HEX_PER_ROW / 2 ? HALF_GAP : 0);
		int y = (int)(i / HEX_PER_ROW) * HEX_ROW_H + 1;
		uint8_t c = col & 1 ? PAL_ODD : PAL_EVEN;
		draw_digit(px, x, y, data[i] >> 4, c);
		draw_digit(px, x + DIGIT_W, y, data[i] & 15, c);
	}
	lv_obj_invalidate(app->hex);
}

/* Text view: control characters become spaces or dots; a cut-off UTF-8 character at the end is
 * dropped. */
static void draw_text(app_t *app, const uint8_t *data, uint32_t len)
{
	char *text = app->text_buf;
	uint32_t n = 0;
	for (uint32_t i = 0; i < len; i++) {
		uint8_t c = data[i];
		if (c == '\r')
			continue;
		text[n++] = c == '\n' || c >= 0x80 ? (char)c : c == '\t' ? ' ' : c < 0x20 || c == 0x7F ? '.' : (char)c;
	}
	uint32_t end = n;
	while (end && (text[end - 1] & 0xC0) == 0x80)
		end--;
	if (end && (text[end - 1] & 0x80)) {
		uint8_t lead = (uint8_t)text[end - 1];
		uint32_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
		if (n - (end - 1) < need)
			n = end - 1;
	}
	text[n] = 0;
	lv_label_set_text(app->text, text);
}

static void show_viewer(app_t *app)
{
	fs_t *fs = app->fs;
	char text[64];
	const entry_t *e = &fs->entries[app->sel];
	put_tail(text, e->name, TITLE_CHARS);
	lv_label_set_text(app->title, text);
	char *p = text;
	if (fs->result != FR_OK) {
		p = fw_put_str(p, error_text(fs->result));
		draw_hex(app, fs->data, 0);
		lv_label_set_text(app->text, "");
	} else {
		p = fw_put_hex(p, (uint32_t)app->pos, 6);
		p = fw_put_str(p, " of ");
		p = put_size(p, fs->file_size);
		p = fw_put_str(p, app->mode == MODE_HEX ? "  hex" : "  text");
		if (app->mode == MODE_HEX)
			draw_hex(app, fs->data, fs->read_len);
		else
			draw_text(app, fs->data, fs->read_len);
	}
	lv_label_set_text(app->footer, text);
}

static void set_mode(app_t *app, uint8_t mode)
{
	app->mode = mode;
	bool list = mode == MODE_LIST;
	for (int i = 0; i < ROWS; i++) {
		if (list) {
			lv_obj_clear_flag(app->names[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_clear_flag(app->sizes[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(app->names[i], LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(app->sizes[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (!list)
		lv_obj_add_flag(app->bar, LV_OBJ_FLAG_HIDDEN);
	if (mode == MODE_HEX)
		lv_obj_clear_flag(app->hex, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(app->hex, LV_OBJ_FLAG_HIDDEN);
	if (mode == MODE_TEXT)
		lv_obj_clear_flag(app->text, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(app->text, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------------- requests */

static bool task_idle(app_t *app)
{
	return app->fs && !app->waiting && app->fs->req == REQ_NONE && !app->fs->busy;
}

static void request(app_t *app, uint8_t req)
{
	app->waiting = true;
	__sync_synchronize();
	app->fs->req = req;
	lv_label_set_text(app->footer, "Reading...");
}

static void request_list(app_t *app)
{
	set_mode(app, MODE_LIST);
	request(app, REQ_LIST);
}

static void request_read(app_t *app, uint64_t pos)
{
	app->pos = pos;
	app->fs->read_pos = pos;
	request(app, REQ_READ);
}

/* Appends a name to the folder path; false if it doesn't fit. */
static bool path_enter(char *path, const char *name)
{
	uint32_t n = (uint32_t)rom_strlen(path), m = (uint32_t)rom_strlen(name);
	bool slash = path[n - 1] != '/';
	if (n + slash + m + 1 > PATH_SIZE)
		return false;
	if (slash)
		path[n++] = '/';
	rom_memcpy(path + n, name, m + 1);
	return true;
}

/* Removes the last folder from the path, copying its name to `last`; false at the top. */
static bool path_up(char *path, char *last)
{
	uint32_t n = (uint32_t)rom_strlen(path);
	if (n <= 3) /* "0:/" */
		return false;
	uint32_t cut = n;
	while (cut > 3 && path[cut - 1] != '/')
		cut--;
	rom_strcpy(last, path + cut);
	path[cut > 3 ? cut - 1 : 3] = 0;
	return true;
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	if (!app->fs) {
		app->fs = fw_worker_mem(app->w);
		if (app->fs) {
			fw_put_str(app->fs->dir_path, "0:/");
			request_list(app);
		} else if (fw_worker_failed(app->w)) {
			lv_label_set_text(app->footer, "Out of memory");
		}
		return;
	}
	fs_t *fs = app->fs;
	if (!app->waiting || fs->busy || fs->req != REQ_NONE || fs->finished == app->seen)
		return;
	app->seen = fs->finished;
	app->waiting = false;
	if (app->mode == MODE_LIST) {
		app->sel = 0;
		for (uint16_t i = 0; app->reselect[0] && i < fs->count; i++)
			if (rom_strcmp(fs->entries[i].name, app->reselect) == 0)
				app->sel = i;
		app->reselect[0] = 0;
		show_list(app);
	} else {
		show_viewer(app);
	}
}

static void move_sel(app_t *app, int delta)
{
	uint16_t count = app->fs->count;
	if (!count)
		return;
	app->sel = (uint16_t)((app->sel + delta % count + count) % count);
	show_list(app);
}

static void list_key(app_t *app, uint32_t key)
{
	fs_t *fs = app->fs;
	switch (key) {
	case FW_KEY_BACK:
		if (path_up(fs->dir_path, app->reselect))
			request_list(app);
		else
			fw_overlay_close(app->ov); /* frees app via on_delete */
		break;
	case FW_KEY_LEFT:
	case FW_KEY_LEFT_REPEAT:
		move_sel(app, -1);
		break;
	case FW_KEY_RIGHT:
	case FW_KEY_RIGHT_REPEAT:
		move_sel(app, 1);
		break;
	case FW_KEY_UP:
	case FW_KEY_UP_REPEAT:
		if (fs->count)
			app->sel = (uint16_t)(app->sel / ROWS * ROWS + ROWS < fs->count ? app->sel / ROWS * ROWS + ROWS : 0);
		show_list(app);
		break;
	case FW_KEY_CENTER: {
		if (!fs->count)
			break;
		const entry_t *e = &fs->entries[app->sel];
		if (e->dir) {
			if (path_enter(fs->dir_path, e->name))
				request_list(app);
		} else {
			rom_strcpy(fs->file_path, fs->dir_path);
			if (path_enter(fs->file_path, e->name)) {
				set_mode(app, MODE_HEX);
				request_read(app, 0);
			}
		}
		break;
	}
	default:
		break;
	}
}

static void viewer_key(app_t *app, uint32_t key)
{
	fs_t *fs = app->fs;
	uint64_t size = fs->file_size;
	uint32_t step = app->mode == MODE_HEX ? HEX_PAGE : TEXT_STEP;
	switch (key) {
	case FW_KEY_BACK:
		set_mode(app, MODE_LIST);
		show_list(app);
		break;
	case FW_KEY_LEFT:
	case FW_KEY_LEFT_REPEAT:
		request_read(app, app->pos >= step ? app->pos - step : 0);
		break;
	case FW_KEY_RIGHT:
	case FW_KEY_RIGHT_REPEAT:
		if (app->pos + step < size)
			request_read(app, app->pos + step);
		break;
	case FW_KEY_UP:
	case FW_KEY_UP_REPEAT: {
		uint64_t jump = size / 16 / HEX_PER_ROW * HEX_PER_ROW;
		uint64_t to = app->pos + (jump > step ? jump : step);
		request_read(app, to < size ? to : 0);
		break;
	}
	case FW_KEY_CENTER:
		set_mode(app, app->mode == MODE_HEX ? MODE_TEXT : MODE_HEX);
		show_viewer(app);
		break;
	default:
		break;
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
	if (key == FW_KEY_BACK_LONG || key == LV_KEY_ESC || !app->fs) {
		if (key == FW_KEY_BACK_LONG || key == LV_KEY_ESC || key == FW_KEY_BACK)
			fw_overlay_close(app->ov); /* frees app via on_delete */
		return;
	}
	if (!task_idle(app))
		return;
	if (app->mode == MODE_LIST)
		list_key(app, key);
	else
		viewer_key(app, key);
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

	app->title = fw_label(root, "", lv_rgb(0x80, 0xA0, 0xC0));
	lv_obj_set_pos(app->title, 2, 0);
	app->bar = fw_rect(root, 128, LINE_H, lv_rgb(0x20, 0x38, 0x60));
	lv_obj_add_flag(app->bar, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < ROWS; i++) {
		app->names[i] = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
		lv_obj_set_size(app->names[i], NAME_W - 4, LINE_H);
		lv_obj_set_pos(app->names[i], 2, (lv_coord_t)(LIST_Y + i * LINE_H));
		app->sizes[i] = fw_label(root, "", lv_rgb(0xA0, 0xA8, 0xB0));
		lv_obj_set_width(app->sizes[i], SIZE_W);
		lv_obj_set_style_text_align(app->sizes[i], LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_set_pos(app->sizes[i], SIZE_X, (lv_coord_t)(LIST_Y + i * LINE_H));
	}
	for (int i = 0; i < 3; i++) {
		app->px[i * 4 + 0] = palette_rgb[i][2];
		app->px[i * 4 + 1] = palette_rgb[i][1];
		app->px[i * 4 + 2] = palette_rgb[i][0];
		app->px[i * 4 + 3] = 0xFF;
	}
	app->dsc.header = LV_IMG_HEADER(LV_IMG_CF_INDEXED_2BIT, HEX_W, HEX_H);
	app->dsc.data_size = sizeof(app->px);
	app->dsc.data = app->px;
	app->hex = lv_img_create(root);
	lv_img_set_src(app->hex, &app->dsc);
	lv_obj_set_pos(app->hex, 0, LIST_Y);
	app->text = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_label_set_long_mode(app->text, LV_LABEL_LONG_CLIP);
	lv_obj_set_size(app->text, 124, (lv_coord_t)(h - 2 * LINE_H));
	lv_obj_set_pos(app->text, 2, LIST_Y);
	app->footer = fw_label(root, "", lv_rgb(0xC0, 0xC8, 0xD0));
	lv_obj_set_pos(app->footer, 2, (lv_coord_t)(h - LINE_H));
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);
	set_mode(app, MODE_LIST);

	app->w = fw_worker_start(file_task, sizeof(fs_t), "files", TASK_PRIORITY, TASK_STACK);
	if (!app->w) {
		lv_label_set_text(app->footer, "Busy, try again");
		return;
	}
	app->timer = lv_timer_create(tick, TICK_MS, app);
}

FW_MENU_ITEM(FW_SYMBOL_BARS, "Files", open);
