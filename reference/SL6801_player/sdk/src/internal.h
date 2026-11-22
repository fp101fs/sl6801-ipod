/* Shared between the SDK's own modules (not for apps). */
#pragma once
#include "sdk.h"

/* State shared by both cores, on the firmware heap (sdk/src/service.c). */
typedef struct {
	uint32_t magic;
	fw_worker_t workers[FW_WORKERS];
	fw_blackbox_t *bb;
	void *volatile modules[FW_MODULE_SLOTS];
} sdk_shared_t;

sdk_shared_t *sdk_shared(void);

/* Black box (sdk/src/blackbox.c), run by the service on CPU0. */
fw_blackbox_t *bb_start(void);
void bb_tick(fw_blackbox_t *bb);
