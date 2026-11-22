/*
 * The SDK's service task on CPU0 (see "the two cores" in sdk.h). hook_cpu0_start replaces a call
 * that the firmware's first task makes once at startup (the watchdog setup at 0xCD8C7A) and starts
 * the service there. The service carries out worker requests from the UI and runs the black box.
 * The state both cores share is on the firmware heap; app code has no writable globals, so the
 * reserved vector table entry at 0x80001C holds its address.
 */
#include "internal.h"

#define SERVICE_PRIORITY 18 /* above the firmware's tasks, so the black box can watch them */
#define SERVICE_STACK 0x300 /* words */
#define POLL_MS 5
#define BB_EVERY 20 /* black box every 20 polls, 100 ms */
#define HEAP_RESERVE 0x8000
#define SHARED_MAGIC 0x53444B31u
#define SHARED_PTR (*(sdk_shared_t *volatile *)0x0080001Cu)

sdk_shared_t *sdk_shared(void)
{
	sdk_shared_t *s = SHARED_PTR;
	return fw_is_ram(s) && s->magic == SHARED_MAGIC ? s : NULL;
}

void *volatile *fw_module_slot(unsigned i)
{
	sdk_shared_t *s = sdk_shared();
	return s && i < FW_MODULE_SLOTS ? &s->modules[i] : NULL;
}

/* ---------------------------------------------------------------- CPU1 (UI) side */

fw_worker_t *fw_worker_start(void (*entry)(fw_worker_t *w), uint32_t mem_size, const char *name, int priority,
			     uint32_t stack_words)
{
	sdk_shared_t *s = sdk_shared();
	if (!s)
		return NULL;
	for (int i = 0; i < FW_WORKERS; i++) {
		fw_worker_t *w = &s->workers[i];
		if (w->state != FW_WORKER_FREE)
			continue;
		w->entry = entry;
		w->mem_size = mem_size;
		w->name = name;
		w->priority = (uint8_t)priority;
		w->stack_words = stack_words;
		w->stop = false;
		w->released = false;
		w->exited = false;
		w->mem = NULL;
		w->task = NULL;
		__sync_synchronize();
		w->state = FW_WORKER_STARTING;
		return w;
	}
	return NULL;
}

void fw_worker_release(fw_worker_t *w)
{
	if (!w)
		return;
	w->stop = true;
	__sync_synchronize();
	w->released = true;
}

void *fw_worker_mem(const fw_worker_t *w)
{
	if (!w)
		return NULL;
	uint8_t state = w->state;
	__sync_synchronize();
	return state == FW_WORKER_RUNNING || state == FW_WORKER_DONE ? w->mem : NULL;
}

bool fw_worker_failed(const fw_worker_t *w)
{
	return !w || w->state == FW_WORKER_FAILED;
}

/* ---------------------------------------------------------------- CPU0 side */

static void worker_main(void *arg)
{
	fw_worker_t *w = arg;
	w->entry(w);
	__sync_synchronize();
	w->exited = true;
}

static void start_worker(fw_worker_t *w)
{
	uint32_t need = w->mem_size + (w->entry ? w->stack_words * 4 : 0) + HEAP_RESERVE;
	if (FW_HEAP_FREE < need) {
		w->state = FW_WORKER_FAILED;
		return;
	}
	void *mem = NULL;
	if (w->mem_size) {
		mem = fw_os_calloc(1, w->mem_size);
		if (!fw_is_ram(mem)) {
			w->state = FW_WORKER_FAILED;
			return;
		}
	}
	w->mem = mem;
	if (w->entry) {
		void *task = fw_task_create(worker_main, w, w->name ? w->name : "worker", w->priority, w->stack_words);
		if (!fw_is_ram(task)) {
			if (mem)
				fw_os_free(mem);
			w->mem = NULL;
			w->state = FW_WORKER_FAILED;
			return;
		}
		w->task = task;
	}
	__sync_synchronize();
	w->state = FW_WORKER_RUNNING;
}

static void service_worker(fw_worker_t *w)
{
	switch (w->state) {
	case FW_WORKER_STARTING:
		if (w->released)
			w->state = FW_WORKER_FREE;
		else
			start_worker(w);
		break;
	case FW_WORKER_RUNNING:
		if (w->task && w->exited) {
			fw_task_join(w->task); /* entry has returned, so this doesn't wait long */
			w->task = NULL;
			w->state = FW_WORKER_DONE;
		}
		break;
	default:
		break;
	}
	uint8_t state = w->state;
	bool idle = state == FW_WORKER_DONE || state == FW_WORKER_FAILED || (state == FW_WORKER_RUNNING && !w->task);
	if (w->released && idle) {
		if (w->mem)
			fw_os_free(w->mem);
		w->mem = NULL;
		__sync_synchronize();
		w->state = FW_WORKER_FREE;
	}
}

static void service_task(void *arg)
{
	sdk_shared_t *s = arg;
	for (uint32_t n = 0;; n++) {
		for (int i = 0; i < FW_WORKERS; i++)
			service_worker(&s->workers[i]);
		if (s->bb && n % BB_EVERY == 0)
			bb_tick(s->bb);
		fw_os_sleep(POLL_MS);
	}
}

/* Replaces the firmware's first task's call to the watchdog setup; runs once, on CPU0. */
__attribute__((used)) void hook_cpu0_start(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	fw_watchdog_setup(a, b, c, d);
	if (sdk_shared())
		return;
	sdk_shared_t *s = fw_os_calloc(1, sizeof(*s));
	if (!fw_is_ram(s))
		return;
	s->bb = bb_start();
	s->magic = SHARED_MAGIC;
	__sync_synchronize();
	SHARED_PTR = s;
	fw_task_create(service_task, s, "sdk", SERVICE_PRIORITY, SERVICE_STACK);
}
