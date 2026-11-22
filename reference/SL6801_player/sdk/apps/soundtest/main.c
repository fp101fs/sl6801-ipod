/*
 * "Sound test" entry in the APP menu: opens the SDK sound module, plays a beep and closes it, three
 * times, one step at a time from a UI timer. Each step is logged on screen before it runs, so
 * after a hang the last line names the step that hung. Center runs the test, Back closes the app
 * when no test is running.
 */
#include "sdk.h"

#define TICK_MS 300
#define WAIT_TICKS 3 /* about 1 s */
#define LOG_LINES 9
#define LOG_SIZE 400

enum { OP_OPEN, OP_PLAY, OP_WAIT, OP_CLOSE, OP_END };

static const char *const op_names[] = {"sound open", "sound play", "wait 1 s", "sound close"};

static const uint8_t ops[] = {OP_OPEN, OP_PLAY, OP_WAIT, OP_CLOSE, OP_WAIT, OP_OPEN, OP_PLAY, OP_WAIT,
			      OP_CLOSE, OP_WAIT, OP_OPEN, OP_PLAY, OP_WAIT, OP_CLOSE, OP_WAIT, OP_END};

static const fw_tone_t beep[] = {{660, 990, 150, FW_WAVE_SQUARE, 120}};

typedef struct {
	fw_overlay_t *ov;
	lv_obj_t *log;
	lv_timer_t *timer;
	fw_sound_t *snd;
	uint8_t pos;
	bool running, announced;
	uint8_t polls;
	char text[LOG_SIZE];
} app_t;

enum { DONE, AGAIN, FAIL };

static void log_add(app_t *app, const char *s)
{
	size_t len = strlen(app->text), add = strlen(s);
	if (len + add + 1 > LOG_SIZE)
		len = app->text[0] = 0;
	memcpy(app->text + len, s, add + 1);
	int lines = 0;
	for (size_t i = 0; app->text[i]; i++)
		lines += app->text[i] == '\n';
	while (lines >= LOG_LINES) { /* drop the first line */
		size_t cut = 0;
		while (app->text[cut] != '\n')
			cut++;
		len = strlen(app->text);
		memmove(app->text, app->text + cut + 1, len - cut);
		lines--;
	}
	lv_label_set_text(app->log, app->text);
}

static int run_op(app_t *app, uint8_t op)
{
	switch (op) {
	case OP_OPEN:
		app->snd = fw_sound_open();
		return app->snd ? DONE : FAIL;
	case OP_PLAY:
		fw_sound_play(app->snd, FW_SFX(beep));
		return DONE;
	case OP_WAIT:
		return ++app->polls < WAIT_TICKS ? AGAIN : DONE;
	case OP_CLOSE:
		fw_sound_close(app->snd);
		app->snd = NULL;
		return DONE;
	}
	return FAIL;
}

static void tick(lv_timer_t *t)
{
	app_t *app = t->user_data;
	if (!app->running)
		return;
	uint8_t op = ops[app->pos];
	char line[40];
	if (!app->announced) {
		char *p = fw_put_str(line, "\n");
		p = fw_put_dec(p, app->pos + 1u);
		p = fw_put_str(p, " ");
		fw_put_str(p, op_names[op]);
		log_add(app, line);
		app->announced = true;
		app->polls = 0;
		return;
	}
	int r = run_op(app, op);
	if (r == AGAIN)
		return;
	log_add(app, r == DONE ? " ok" : " FAIL");
	app->announced = false;
	if (r == FAIL || ops[++app->pos] == OP_END) {
		log_add(app, r == FAIL ? "\nStopped." : "\nAll passed.");
		app->running = false;
	}
}

static void on_delete(void *ctx)
{
	app_t *app = ctx;
	lv_timer_del(app->timer);
	fw_sound_close(app->snd);
	lv_mem_free(app);
}

static void key_cb(lv_event_t *e)
{
	app_t *app = lv_event_get_user_data(e);
	uint32_t key = *(uint32_t *)lv_event_get_param(e);
	if (app->running)
		return;
	if (key == FW_KEY_BACK || key == LV_KEY_ESC) {
		fw_overlay_close(app->ov);
	} else if (key == LV_KEY_ENTER) {
		app->pos = 0;
		app->announced = false;
		app->running = true;
	}
}

static void open(lv_obj_t *return_focus)
{
	app_t *app = lv_mem_alloc(sizeof(app_t));
	if (!app)
		return;
	memset(app, 0, sizeof(*app));
	app->ov = fw_overlay_open(return_focus, lv_rgb(0x10, 0x20, 0x30), on_delete, app);
	if (!app->ov) {
		lv_mem_free(app);
		return;
	}
	lv_obj_t *root = app->ov->root;
	app->log = fw_label(root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_label_set_long_mode(app->log, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(app->log, lv_disp_get_hor_res(NULL) - 4);
	lv_obj_align(app->log, LV_ALIGN_BOTTOM_LEFT, 2, -2); /* keeps the newest line visible */
	app->timer = lv_timer_create(tick, TICK_MS, app);
	lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, app);
	log_add(app, "Sound test: opens, beeps and closes the sound 3 times.\nCenter: run");
}

FW_MENU_ITEM(FW_SYMBOL_AUDIO, "Sound test", open);
