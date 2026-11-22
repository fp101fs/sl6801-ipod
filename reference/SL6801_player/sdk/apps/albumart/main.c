/*
 * Album art: the cover of the playing MP3 behind the music player's "now playing" page. No APP
 * menu row; it works through firmware patches (patches.json).
 * - hook_media_start replaces MediaTask's 16 calls to fw_media_start (CPU0) and copies each new
 *   track's path into a shared record (module slot FW_MODULE_ALBUM_ART) with a sequence count.
 * - hook_music_play_create replaces view_page_create's call to page_music_play_create (CPU1). It
 *   builds the page, adds an image as the page container's first child (so LVGL draws it before
 *   the labels) and starts a worker. Deleting the page releases the worker.
 * - The worker (CPU0) waits for a new path, finds the front cover (an APIC frame holding a JPEG;
 *   else the first JPEG) in the file's ID3v2 tag, and decodes it with the firmware's TJpgDec at
 *   the smallest scale that still fills the screen. Its output callback crops the picture to
 *   fill 128x160, halves the brightness so the labels stay readable, and swaps the bytes for
 *   LVGL. Before it overwrites a shown cover, it asks the UI to hide it.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define ART_W 128
#define ART_H 160
#define PATH_SIZE 256
#define TASK_PRIORITY 2 /* below the player's tasks */
#define TASK_STACK 0x300 /* words */
#define POLL_MS 100
#define TICK_MS 100
#define HIDE_POLL_MS 20
#define HIDE_TIMEOUT_MS 1000
#define READ_SIZE 512
#define MIME_SIZE 32
#define FATFS_HEAP_MIN 0x2000 /* free heap needed before FatFs calls (each takes 0x460) */
#define TYPE_FRONT_COVER 3

/* The playing track, written by MediaTask. */
typedef struct {
	volatile uint32_t seq; /* odd while being written */
	char path[PATH_SIZE];
} track_t;

/* Buffered reads from a region of the file, undoing ID3 unsynchronisation (FF 00 -> FF). */
typedef struct {
	fw_file_t file;
	uint32_t fpos; /* file offset just past buf's data */
	uint32_t left; /* bytes of the region not read into buf yet */
	bool unsync;
	uint8_t prev;
	uint16_t pos, len;
	uint8_t buf[READ_SIZE];
} reader_t;

typedef struct {
	bool found;
	uint8_t type;
	bool unsync;
	uint32_t offset, length; /* the picture data in the file */
} picture_t;

/* The worker's memory. */
typedef struct {
	volatile uint32_t version; /* bumped when px holds a new cover, or none */
	volatile bool have_art;
	volatile uint32_t hide_req, hide_ack; /* the worker asks the UI to stop showing px */
	uint16_t px[ART_W * ART_H];          /* RGB565, bytes swapped for LVGL */
	uint32_t jd[FW_JD_SIZE / 4];
	uint32_t pool[FW_JD_POOL / 4];
	fw_worker_t *self;
	reader_t rd;
	uint16_t map_x[ART_W], map_y[ART_H]; /* scaled source pixel for each screen column and row */
	uint32_t seq;                        /* track_t seq of path */
	char path[PATH_SIZE];
	picture_t pic; /* the cover being decoded */
} art_t;

/* UI state, on the LVGL heap. */
typedef struct {
	fw_worker_t *w;
	lv_obj_t *page; /* the page's container */
	lv_obj_t *img;  /* the cover: the container's first child, so drawn before the others */
	lv_timer_t *timer;
	lv_img_dsc_t dsc;
	uint32_t version;
	bool shown;
} view_t;

/* ---------------------------------------------------------------- path capture (MediaTask) */

__attribute__((used)) int hook_media_start(const char *path, uint32_t type)
{
	void *volatile *slot = fw_module_slot(FW_MODULE_ALBUM_ART);
	if (slot && !*slot) {
		track_t *t = fw_os_calloc(1, sizeof(track_t));
		if (fw_is_ram(t))
			*slot = t;
	}
	track_t *t = slot ? *slot : NULL;
	if (t && path) {
		t->seq++;
		__sync_synchronize();
		uint32_t i = 0;
		for (; i < PATH_SIZE - 1 && path[i]; i++)
			t->path[i] = path[i];
		t->path[i] = 0;
		__sync_synchronize();
		t->seq++;
	}
	return fw_media_start(path, type);
}

/* ---------------------------------------------------------------- reader (worker) */

static bool rd_start(reader_t *r, uint32_t offset, uint32_t length, bool unsync)
{
	r->fpos = offset;
	r->left = length;
	r->unsync = unsync;
	r->prev = 0;
	r->pos = r->len = 0;
	return f_lseek(&r->file, offset) == FR_OK;
}

static uint32_t rd_offset(const reader_t *r)
{
	return r->fpos - (r->len - r->pos);
}

/* The next byte, or -1 at the end of the region or on an error. */
static int rd_byte(reader_t *r)
{
	for (;;) {
		if (r->pos == r->len) {
			uint32_t n = r->left < READ_SIZE ? r->left : READ_SIZE, got = 0;
			if (!n || f_read(&r->file, r->buf, n, &got) != FR_OK || !got)
				return -1;
			r->fpos += got;
			r->left -= got;
			r->pos = 0;
			r->len = (uint16_t)got;
		}
		uint8_t c = r->buf[r->pos++];
		bool stuffed = r->unsync && r->prev == 0xFF && c == 0x00;
		r->prev = c;
		if (!stuffed)
			return c;
	}
}

static bool rd_read(reader_t *r, uint8_t *dst, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		int c = rd_byte(r);
		if (c < 0)
			return false;
		dst[i] = (uint8_t)c;
	}
	return true;
}

static bool rd_skip(reader_t *r, uint32_t n)
{
	if (r->unsync) {
		while (n--)
			if (rd_byte(r) < 0)
				return false;
		return true;
	}
	uint32_t buffered = (uint32_t)(r->len - r->pos);
	if (n <= buffered) {
		r->pos = (uint16_t)(r->pos + n);
		return true;
	}
	n -= buffered;
	r->pos = r->len;
	if (n > r->left)
		return false;
	r->fpos += n;
	r->left -= n;
	return f_lseek(&r->file, r->fpos) == FR_OK;
}

/* ---------------------------------------------------------------- ID3v2 (worker) */

static uint32_t syncsafe32(const uint8_t *p)
{
	return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static char lower(int c)
{
	return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

/* Reads an APIC (or v2.2 PIC) frame's fields up to the picture data. Returns the picture type, or
 * -1 if the picture isn't a JPEG or the fields don't fit; *used is the bytes read. */
static int picture_fields(reader_t *r, bool v22, uint32_t size, uint32_t *used)
{
	uint32_t n = 0;
	int enc = rd_byte(r);
	n++;
	bool jpeg;
	if (v22) {
		uint8_t fmt[3];
		if (!rd_read(r, fmt, 3))
			return -1;
		n += 3;
		jpeg = lower(fmt[0]) == 'j' && lower(fmt[1]) == 'p';
	} else {
		char mime[MIME_SIZE];
		uint32_t k = 0;
		int c;
		while ((c = rd_byte(r)) > 0 && n < size) {
			n++;
			if (k < MIME_SIZE - 1)
				mime[k++] = lower(c);
		}
		n++;
		mime[k] = 0;
		jpeg = !k; /* no MIME type: let the decoder decide */
		for (uint32_t i = 0; i + 1 < k; i++)
			if (mime[i] == 'j' && mime[i + 1] == 'p')
				jpeg = true;
	}
	int type = rd_byte(r);
	n++;
	if (enc == 1 || enc == 2) { /* UTF-16: ends with a 0000 pair */
		uint8_t pair[2];
		do {
			if (!rd_read(r, pair, 2))
				return -1;
			n += 2;
		} while ((pair[0] || pair[1]) && n < size);
	} else {
		int c;
		do {
			c = rd_byte(r);
			n++;
		} while (c > 0 && n < size);
	}
	*used = n;
	return jpeg && type >= 0 && n < size ? type : -1;
}

/* Scans the ID3v2 tag at the start of the open file for the cover: the front cover if there is
 * one, else the first JPEG picture. */
static bool find_cover(reader_t *r, picture_t *best)
{
	uint8_t h[10];
	uint32_t got = 0;
	best->found = false;
	if (f_read(&r->file, h, sizeof(h), &got) != FR_OK || got != sizeof(h))
		return false;
	uint8_t ver = h[3], flags = h[5];
	if (h[0] != 'I' || h[1] != 'D' || h[2] != '3' || ver < 2 || ver > 4)
		return false;
	bool tag_unsync = flags & 0x80;
	if (!rd_start(r, sizeof(h), syncsafe32(h + 6), tag_unsync && ver < 4))
		return false;
	if ((flags & 0x40) && ver >= 3) { /* extended header */
		uint8_t e[4];
		if (!rd_read(r, e, 4) || !rd_skip(r, ver == 3 ? be32(e) : syncsafe32(e) - 4))
			return false;
	}
	bool v22 = ver == 2;
	uint32_t hlen = v22 ? 6 : 10;
	for (;;) {
		uint8_t f[10];
		if (!rd_read(r, f, hlen) || !f[0]) /* end, or padding */
			break;
		uint32_t size = v22 ? (uint32_t)f[3] << 16 | (uint32_t)f[4] << 8 | f[5] : ver == 4 ? syncsafe32(f + 4) : be32(f + 4);
		uint16_t fflags = v22 ? 0 : (uint16_t)(f[8] << 8 | f[9]);
		bool picture = v22 ? f[0] == 'P' && f[1] == 'I' && f[2] == 'C' : f[0] == 'A' && f[1] == 'P' && f[2] == 'I' && f[3] == 'C';
		bool packed = ver == 3 ? (fflags & 0x00C0) : ver == 4 ? (fflags & 0x000C) : false; /* compressed, encrypted */
		if (!picture || packed || !size) {
			if (!rd_skip(r, size))
				break;
			continue;
		}
		bool tag_level = r->unsync;
		r->unsync = tag_level || (ver == 4 && (tag_unsync || (fflags & 0x0002)));
		uint32_t dli = ver == 4 && (fflags & 0x0001) ? 4 : 0; /* data length indicator */
		uint32_t used = dli;
		int type = -1;
		if (dli < size && rd_skip(r, dli)) {
			uint32_t n = 0;
			type = picture_fields(r, v22, size - dli, &n);
			used += n;
		}
		if (type >= 0 && (!best->found || (type == TYPE_FRONT_COVER && best->type != TYPE_FRONT_COVER))) {
			best->found = true;
			best->type = (uint8_t)type;
			best->unsync = r->unsync;
			best->offset = rd_offset(r);
			/* Sizes count unsynchronised data decoded, so such a picture is read to the end of
			 * the tag; the decoder stops at the JPEG's end marker. */
			best->length = r->unsync ? r->left + (uint32_t)(r->len - r->pos) : size - used;
		}
		bool ok = used <= size && rd_skip(r, size - used);
		r->unsync = tag_level;
		if (!ok)
			break;
	}
	return best->found;
}

/* ---------------------------------------------------------------- decoding (worker) */

static art_t *jd_art(void *jd)
{
	return *(art_t **)((uint8_t *)jd + FW_JD_DEVICE);
}

/* Positions are relative to the JPEG's start in the file. */
static uint32_t jd_in(void *jd, uint8_t *buf, uint32_t n)
{
	art_t *a = jd_art(jd);
	reader_t *r = &a->rd;
	const picture_t *pic = &a->pic;
	if (*(const uint32_t *)((const uint8_t *)jd + FW_JD_TELL))
		return rd_offset(r) - pic->offset;
	if (*(const uint32_t *)((const uint8_t *)jd + FW_JD_SEEK))
		return n <= pic->length && rd_start(r, pic->offset + n, pic->length - n, pic->unsync) ? n : 0;
	if (!buf)
		return rd_skip(r, n) ? n : 0;
	uint32_t i = 0;
	for (int c; i < n && (c = rd_byte(r)) >= 0; i++)
		buf[i] = (uint8_t)c;
	return i;
}

/* Copies the screen pixels that fall in this block of the scaled picture. */
static int jd_out(void *jd, void *bitmap, const uint16_t *rect)
{
	art_t *a = jd_art(jd);
	const uint16_t *src = bitmap;
	uint32_t left = rect[0], right = rect[1], top = rect[2], bottom = rect[3];
	uint32_t width = right - left + 1;
	for (int y = 0; y < ART_H; y++) {
		uint32_t sy = a->map_y[y];
		if (sy < top || sy > bottom)
			continue;
		const uint16_t *row = src + (sy - top) * width;
		uint16_t *dst = a->px + y * ART_W;
		for (int x = 0; x < ART_W; x++) {
			uint32_t sx = a->map_x[x];
			if (sx < left || sx > right)
				continue;
			uint16_t p = (uint16_t)((row[sx - left] >> 1) & 0x7BEF); /* half brightness */
			dst[x] = (uint16_t)(p << 8 | p >> 8);
		}
	}
	return !a->self->stop;
}

/* Maps screen pixels to the scaled picture, cropped around its center to fill the screen. */
static void map_cover(art_t *a, uint32_t sw, uint32_t sh)
{
	uint32_t ch = sh < sw * ART_H / ART_W ? sh : sw * ART_H / ART_W;
	uint32_t cw = ch * ART_W / ART_H;
	uint32_t x0 = (sw - cw) / 2, y0 = (sh - ch) / 2;
	for (uint32_t x = 0; x < ART_W; x++)
		a->map_x[x] = (uint16_t)(x0 + (x * cw + cw / 2) / ART_W);
	for (uint32_t y = 0; y < ART_H; y++)
		a->map_y[y] = (uint16_t)(y0 + (y * ch + ch / 2) / ART_H);
}

static bool decode(art_t *a, const picture_t *pic)
{
	if (!rd_start(&a->rd, pic->offset, pic->length, pic->unsync))
		return false;
	if (fw_jd_prepare(a->jd, jd_in, a->pool, sizeof(a->pool), a, 1) != 0)
		return false;
	const uint8_t *jd = (const uint8_t *)a->jd;
	uint32_t w = *(const uint16_t *)(jd + FW_JD_WIDTH), h = *(const uint16_t *)(jd + FW_JD_HEIGHT);
	uint32_t crop_h = h < w * ART_H / ART_W ? h : w * ART_H / ART_W;
	uint8_t scale = 3; /* a progressive JPEG only has 1/8 detail */
	if (*(const uint32_t *)(jd + FW_JD_PROGRESSIVE) != 1)
		for (scale = 0; scale < 3 && (crop_h >> (scale + 1)) >= ART_H;)
			scale++;
	map_cover(a, w >> scale, h >> scale);
	memset(a->px, 0, sizeof(a->px));
	return fw_jd_decomp(a->jd, jd_out, scale) == 0;
}

/* ---------------------------------------------------------------- worker */

/* Copies the playing track's path if it changed since the last call. */
static bool new_track(art_t *a)
{
	void *volatile *slot = fw_module_slot(FW_MODULE_ALBUM_ART);
	const track_t *t = slot ? *slot : NULL;
	if (!t)
		return false;
	uint32_t seq = t->seq;
	if (seq == a->seq || (seq & 1))
		return false;
	__sync_synchronize();
	memcpy(a->path, t->path, PATH_SIZE);
	__sync_synchronize();
	if (t->seq != seq)
		return false; /* written meanwhile: next time */
	a->seq = seq;
	return true;
}

/* Has the UI stop showing px before it changes. */
static void hide(art_t *a)
{
	if (!a->have_art)
		return;
	a->have_art = false;
	uint32_t req = ++a->hide_req;
	for (int ms = 0; ms < HIDE_TIMEOUT_MS && a->hide_ack != req && !a->self->stop; ms += HIDE_POLL_MS)
		fw_os_sleep(HIDE_POLL_MS);
}

static void art_task(fw_worker_t *w)
{
	art_t *a = w->mem;
	a->self = w;
	while (!w->stop) {
		if (new_track(a)) {
			hide(a);
			bool ok = false;
			if (fw_sd_mounted() && FW_HEAP_FREE >= FATFS_HEAP_MIN && f_open(&a->rd.file, a->path, FA_READ) == FR_OK) {
				ok = find_cover(&a->rd, &a->pic) && decode(a, &a->pic);
				f_close(&a->rd.file);
			}
			a->have_art = ok;
			__sync_synchronize();
			a->version++;
		}
		fw_os_sleep(POLL_MS);
	}
}

/* ---------------------------------------------------------------- UI (CPU1) */

static void show_cover(view_t *v, const void *src)
{
	if (src) {
		lv_img_set_src(v->img, src);
		lv_obj_clear_flag(v->img, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
	}
	lv_obj_invalidate(v->page);
	v->shown = src != NULL;
}

/* Moves obj to the front of its parent's children (lv_obj_move_background isn't in the
 * firmware): spec_attr at +8 holds the children array and its count. */
static void move_to_back(lv_obj_t *obj)
{
	lv_obj_t *parent = lv_obj_get_parent(obj);
	uint32_t *spec = *(uint32_t **)((uint8_t *)parent + 8);
	lv_obj_t **kids = (lv_obj_t **)(uintptr_t)spec[0];
	uint32_t n = spec[1];
	uint32_t i = 0;
	while (i < n && kids[i] != obj)
		i++;
	for (; i > 0 && i < n; i--)
		kids[i] = kids[i - 1];
	if (n)
		kids[0] = obj;
}

static void tick(lv_timer_t *t)
{
	view_t *v = t->user_data;
	art_t *a = fw_worker_mem(v->w);
	if (!a)
		return;
	uint32_t req = a->hide_req;
	if (req != a->hide_ack) {
		if (v->shown)
			show_cover(v, NULL);
		a->hide_ack = req;
	}
	uint32_t version = a->version;
	if (version == v->version)
		return;
	v->version = version;
	__sync_synchronize();
	if (a->have_art) {
		v->dsc.data = (const uint8_t *)a->px;
		show_cover(v, &v->dsc);
	} else if (v->shown) {
		show_cover(v, NULL);
	}
}

static void page_deleted(lv_event_t *e)
{
	view_t *v = lv_event_get_user_data(e);
	lv_timer_del(v->timer);
	fw_worker_release(v->w);
	lv_mem_free(v);
}

/* Replaces view_page_create's call to page_music_play_create. */
__attribute__((used)) int hook_music_play_create(void)
{
	int ret = page_music_play_create();
	lv_obj_t **state = fw_music_play_page();
	if (ret != 0 || !state || !state[1])
		return ret;
	view_t *v = lv_mem_alloc(sizeof(view_t));
	if (!v)
		return ret;
	memset(v, 0, sizeof(*v));
	v->page = lv_obj_get_parent(state[1]);
	v->dsc.header = LV_IMG_HEADER(LV_IMG_CF_TRUE_COLOR, ART_W, ART_H);
	v->dsc.data_size = ART_W * ART_H * 2;
	v->img = lv_img_create(v->page);
	if (!v->img) {
		lv_mem_free(v);
		return ret;
	}
	lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_pos(v->img, 0, 0);
	move_to_back(v->img);
	v->w = fw_worker_start(art_task, sizeof(art_t), "albumart", TASK_PRIORITY, TASK_STACK);
	v->timer = v->w ? lv_timer_create(tick, TICK_MS, v) : NULL;
	if (!v->timer) {
		fw_worker_release(v->w);
		lv_obj_del(v->img);
		lv_mem_free(v);
		return ret;
	}
	lv_obj_add_event_cb(v->page, page_deleted, LV_EVENT_DELETE, v);
	return ret;
}
