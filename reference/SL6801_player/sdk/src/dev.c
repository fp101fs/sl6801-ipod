/*
 * Device helpers shared by the SDK modules.
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

bool fw_dev_busy(const char *name)
{
	void **head = FW_DEV_LIST;
	for (void **link = *head; link != head; link = *link) {
		uint8_t *node = (uint8_t *)link - 0x14;
		if (rom_strcmp(*(const char **)node, name) == 0)
			return node[0x10] & 1;
	}
	return true;
}
