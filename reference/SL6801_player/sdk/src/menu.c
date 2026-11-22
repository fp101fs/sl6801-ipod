/* Appends the app's entries to the firmware's "Apps" menu (page 0x53). */
#include "sdk.h"

static void item_event_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_KEY && *(uint32_t *)lv_event_get_param(e) == LV_KEY_ENTER) {
		const fw_menu_item_t *item = lv_event_get_user_data(e);
		item->open(e->current_target);
		return;
	}
	page_home_menu_event_cb(e); /* navigation and back work regardless of which row has focus */
}

/* Builds a row the same way page_home_menu_create builds its own. */
static void add_row(lv_obj_t *list, const fw_menu_item_t *entry)
{
	lv_obj_t *row = fw_list_item_create(list);
	lv_obj_set_height(row, lv_disp_get_ver_res(NULL) / 7);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_add_event_cb(row, item_event_cb, LV_EVENT_KEY, (void *)entry);
	lv_group_add_obj(fw_view_group(), row);

	lv_obj_t *icon = lv_label_create(row);
	lv_obj_set_style_text_font(icon, FW_FONT_ICONS, 0);
	lv_label_set_text(icon, entry->symbol);

	lv_obj_t *label = lv_label_create(row);
	lv_obj_set_width(label, lv_disp_get_hor_res(NULL) - 4 - lv_obj_get_width(icon));
	lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
	lv_label_set_text(label, entry->text);
	fw_list_item_layout(row, 0, 0, 2);
}

extern const fw_menu_item_t __fw_menu_start[], __fw_menu_end[]; /* app.ld */

/* Replaces the call to page_home_menu_create in view_page_create. */
__attribute__((used, section(".text.entry"))) int hook_home_menu_create(void)
{
	int ret = page_home_menu_create();
	if (ret != 0)
		return ret;
	lv_obj_t *list = lv_obj_get_parent(fw_home_menu_page()->item[0]);
	for (const fw_menu_item_t *item = __fw_menu_start; item < __fw_menu_end; item++)
		add_row(list, item);
	return 0;
}
