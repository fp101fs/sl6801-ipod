#include "sdk.h"

#define RAW_POWER_PRESS 4 /* a long Center press, reported by the power management chip */
#define RAW_EVENT(id, event) (((uint32_t)(event) << 16) | (id))
#define RAW_IGNORED 0     /* an unknown key id: the key manager logs it and does nothing */
#define KEY_ID_ENTER 0x21
#define KEY_ID_BACK 0x45
#define KEY_EVENT_LONG_START 0x40
#define KEYS_READY 0x5A
#define KEY_STATE_WORK 1  /* screen on */

static uint32_t focused_flags(void)
{
	if (!fw_is_ram(FW_VIEW_CTX))
		return 0;
	lv_obj_t *focused = lv_group_get_focused(fw_view_group());
	return fw_is_ram(focused) ? lv_obj_flags(focused) : 0;
}

/*
 * Runs in the key manager's task (FUN_00CDD5C0) in place of its "keys ready" check, with the raw
 * event it is about to decode, on CPU0. Only reads LVGL state, which belongs to the UI on CPU1.
 * - The key manager treats a long Center press as the power key and powers off (or turns the
 *   screen off). With FW_OBJ_FLAG_CENTER_LONG on the focused object, the event becomes a Center
 *   long start instead, which the keypad driver reports as FW_KEY_CENTER_LONG.
 * - With the screen on, it opens the volume control on a Back long start. With
 *   FW_OBJ_FLAG_BACK_LONG, the event goes straight to the keypad driver (FW_KEY_BACK_LONG) and
 *   the key manager gets an ignored event instead.
 */
uint8_t hook_key_event(uint32_t *ev)
{
	uint8_t ready = fw_key_ready();
	if (ready != KEYS_READY)
		return ready;
	uint32_t raw = *ev;
	if (raw == RAW_POWER_PRESS) {
		if (focused_flags() & FW_OBJ_FLAG_CENTER_LONG)
			*ev = RAW_EVENT(KEY_ID_ENTER, KEY_EVENT_LONG_START);
	} else if ((raw & 0xFF) == KEY_ID_BACK && ((raw >> 16) & 0xFF) == KEY_EVENT_LONG_START &&
		   FW_KEY_STATE == KEY_STATE_WORK && (focused_flags() & FW_OBJ_FLAG_BACK_LONG)) {
		fw_key_forward(KEY_ID_BACK, KEY_EVENT_LONG_START);
		*ev = RAW_IGNORED;
	}
	return ready;
}
