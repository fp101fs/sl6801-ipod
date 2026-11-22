/*
 * "Crash log" entry in the APP menu: the state of the black box (sdk/src/blackbox.c), which writes
 * 0:/crashlog.txt on a fault or when the UI loop stops. Up turns the 0:/bbstatus.txt file (CPU
 * use and state of every task, rewritten every second) on or off. Holding Center crashes the UI
 * core on purpose and holding Up hangs it, to test both reports; the watchdog resets the player
 * about 15 s later. Back exits.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define REFRESH_MS 250

typedef struct {
	fw_overlay_t *ov;
	lv_timer_t *timer;
	lv_obj_t *label;
} app_t;

static const char *result_text(int32_t r)
{
	switch (r) {
	case BB_REPORT_NONE:
		return "none yet";
	case BB_REPORT_OK:
		return "written";
	case BB_REPORT_NO_CARD:
		return "no card";
	case BB_REPORT_NO_MEMORY:
		return "low memory";
	default:
		return "card error";
	}
}

static void refresh(lv_timer_t *t)
{
	app_t *app = t->user_data;
	const fw_blackbox_t *bb = fw_blackbox();
	char text[256];
	char *p = fw_put_str(text, "Black box\n");
	if (!bb) {
		fw_put_str(p, "not running");
	} else {
		p = fw_put_str(p, "Report: ");
		p = fw_put_str(p, result_text(bb->report));
		p = fw_put_str(p, "\nStatus file: ");
		if (bb->status_file) {
			p = fw_put_dec(p, bb->status_writes);
			p = fw_put_str(p, ", ");
			p = fw_put_str(p, result_text(bb->status_result));
		} else {
			p = fw_put_str(p, "off");
		}
		fw_put_str(p, "\n\nUp: status file on/off\nHold Center: crash test\nHold Up: hang test");
	}
	lv_label_set_text(app->label, text);
}

static void on_delete(void *ctx)
{
	app_t *app = ctx;
	if (app->timer)
		lv_timer_del(app->timer);
	lv_mem_free(app);
}

static void key_cb(lv_event_t *e)
{
	app_t *app = lv_event_get_user_data(e);
	uint32_t key = *(uint32_t *)lv_event_get_param(e);
	fw_blackbox_t *bb = fw_blackbox();
	switch (key) {
	case FW_KEY_BACK:
	case FW_KEY_BACK_LONG:
	case LV_KEY_ESC:
		fw_overlay_close(app->ov); /* frees app via on_delete */
		return;
	case FW_KEY_UP:
		if (bb)
			bb->status_file = !bb->status_file;
		break;
	case FW_KEY_CENTER_LONG:
		__builtin_trap();
		break;
	case FW_KEY_UP_LONG:
		for (volatile int spin = 1; spin;) {
		}
		break;
	default:
		return;
	}
	if (app->timer)
		refresh(app->timer);
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
	app->label = fw_label(app->ov->root, "", lv_rgb(0xFF, 0xFF, 0xFF));
	lv_obj_set_pos(app->label, 2, 2);
	lv_obj_add_event_cb(app->ov->root, key_cb, LV_EVENT_KEY, app);
	app->timer = lv_timer_create(refresh, REFRESH_MS, app);
	if (app->timer)
		refresh(app->timer);
}

FW_MENU_ITEM(FW_SYMBOL_BARS, "Crash log", open);
