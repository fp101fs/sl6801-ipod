/*
 * Black box for crashes and hangs (see sdk.h). RAM does not survive the watchdog's reset (it goes
 * through the power management chip), so reports go to the SD card while CPU0 still runs:
 * - Fault vectors 3-6 of the RAM vector table, which both cores use, point to bb_fault_entry. It
 *   records the fault. A faulting task on CPU0 is parked by rewriting its stacked pc, so the other
 *   tasks keep running; CPU1 (the UI) spins in the handler, and CPU0 carries on.
 * - hook_view_alive replaces the UI loop's per-pass watchdog check-in (CPU1) and timestamps it.
 * - bb_tick, called by the service task on CPU0 every 100 ms, writes 0:/crashlog.txt once after a
 *   fault or when the UI loop has not run for BB_HANG_MS, and 0:/bbstatus.txt every second while
 *   the UI asks for it. Running at high priority, the service also preempts a spinning task, so
 *   the CPU use it reports counts that task too.
 */
#include "internal.h"

#define BB_HANG_MS 3000
#define BB_STATUS_TICKS 10 /* status file every 10 ticks */
#define TEXT_SIZE 6144
#define REPORT_PATH "0:/crashlog.txt"
#define STATUS_PATH "0:/bbstatus.txt"
#define FATFS_HEAP_MIN 0x2000 /* left for FatFs's path buffers */

#define SCB_VTOR 0xE000ED08u
#define SCB_CFSR 0xE000ED28u
#define SCB_HFSR 0xE000ED2Cu
#define SCB_MMFAR 0xE000ED34u
#define SCB_BFAR 0xE000ED38u
#define TCB_PRIORITY 0x2C
#define TCB_NAME 0x34
#define WATCH_MONITOR ((const volatile uint32_t *)0x0081D15Cu) /* registered mask, checked-in mask */
#define REG(a) (*(volatile uint32_t *)(a))

/* Working memory for building and writing the files, allocated once. */
typedef struct {
	char text[TEXT_SIZE];
	fw_file_t file;
	fw_task_status_t st[BB_MAX_TASKS];
	uint32_t ticks;
} work_t;

int bb_record(uint32_t *frame, uint32_t exc_return);

fw_blackbox_t *fw_blackbox(void)
{
	sdk_shared_t *s = sdk_shared();
	return s ? s->bb : NULL;
}

/* Runs in handler mode on a fault. Returns to a parked task, or spins until the watchdog resets
 * the player. */
__attribute__((naked)) static void bb_fault_entry(void)
{
	__asm volatile("tst lr, #4\n"
		       "ite eq\n"
		       "mrseq r0, msp\n"
		       "mrsne r0, psp\n"
		       "mov r1, lr\n"
		       "push {r4, lr}\n"
		       "bl bb_record\n"
		       "pop {r4, lr}\n"
		       "cbz r0, 1f\n"
		       "bx lr\n"
		       "1: b 1b\n");
}

static uint32_t now(void)
{
	return (uint32_t)rom_timer_read64();
}

static uint32_t ms(uint32_t ticks)
{
	return (uint32_t)((uint64_t)ticks * 1000u / FW_TIMER_HZ);
}

static bool ram_range(const void *p, uint32_t bytes)
{
	return fw_is_ram(p) && fw_is_ram((const uint8_t *)p + bytes - 1);
}

static void copy_name(char *dst, const char *src)
{
	int i = 0;
	if (ram_range(src, BB_NAME_LEN))
		for (; i < BB_NAME_LEN - 1 && src[i]; i++)
			dst[i] = src[i];
	dst[i] = 0;
}

__attribute__((noreturn)) static void bb_park(void)
{
	for (;;)
		fw_os_sleep(1000);
}

/* For a fault in the IDLE task, which must never block: waits for interrupts instead. */
__attribute__((noreturn)) static void bb_park_idle(void)
{
	for (;;)
		__asm volatile("wfi");
}

__attribute__((used, noinline)) int bb_record(uint32_t *frame, uint32_t exc_return)
{
	fw_blackbox_t *bb = fw_blackbox();
	if (!bb || bb->faulted)
		return 0;
	bb->fault_core = FW_CORE_ID;
	bb->fault_time = now();
	bb->exc_return = exc_return;
	bb->sp = (uint32_t)frame;
	bb->cfsr = REG(SCB_CFSR);
	bb->hfsr = REG(SCB_HFSR);
	bb->mmfar = REG(SCB_MMFAR);
	bb->bfar = REG(SCB_BFAR);
	REG(SCB_CFSR) = bb->cfsr; /* write 1 to clear */
	REG(SCB_HFSR) = bb->hfsr;
	uint32_t hw_words = exc_return & 0x10 ? 8 : 26; /* 26 with the FPU's extended frame */
	bool frame_ok = ram_range(frame, (hw_words + BB_STACK_WORDS) * 4);
	if (frame_ok) {
		for (int i = 0; i < 8; i++)
			bb->frame[i] = frame[i];
		for (int i = 0; i < BB_STACK_WORDS; i++)
			bb->stack[i] = frame[hw_words + i];
	}
	const uint8_t *tcb = FW_CURRENT_TCB;
	copy_name(bb->task, (const char *)tcb + TCB_NAME);
	__sync_synchronize();
	bb->faulted = 1;
	if (bb->fault_core != 0 || !(exc_return & 4) || !frame_ok)
		return 0;
	bool idle = *(const uint32_t *)(tcb + TCB_PRIORITY) == 0;
	frame[5] = 0; /* lr */
	frame[6] = (uint32_t)(idle ? bb_park_idle : bb_park) & ~1u;
	frame[7] = 0x01000000u; /* xPSR: Thumb */
	return 1;
}

/* Replaces the UI loop's watchdog check-in, once per pass of the loop (on CPU1). */
__attribute__((used)) int hook_view_alive(void *watch_task)
{
	fw_blackbox_t *bb = fw_blackbox();
	if (bb)
		bb->view_beat = now();
	return fw_watch_checkin(watch_task);
}

/* ---------------------------------------------------------------- text output */

typedef struct {
	char *p, *end;
} out_t;

static void put(out_t *o, const char *s)
{
	while (*s && o->p < o->end)
		*o->p++ = *s++;
}

static void put_dec(out_t *o, uint32_t v)
{
	char tmp[12];
	fw_put_dec(tmp, v);
	put(o, tmp);
}

static void put_hex(out_t *o, uint32_t v)
{
	char tmp[12];
	fw_put_hex(tmp, v, 8);
	put(o, tmp);
}

static void put_reg(out_t *o, const char *name, uint32_t v)
{
	put(o, name);
	put(o, " ");
	put_hex(o, v);
}

static void put_words(out_t *o, const uint32_t *w, int n)
{
	for (int i = 0; i < n; i++) {
		put(o, i % 4 ? " " : "\n  ");
		put_hex(o, w[i]);
	}
	put(o, "\n");
}

/* ---------------------------------------------------------------- report sections */

/* The saved context of a task that is not running: r4-r11 and EXC_RETURN, s16-s31 if the task
 * used the FPU, then the hardware frame (r0-r3, r12, lr, pc, xPSR). */
static const uint32_t *saved_frame(const void *tcb)
{
	const uint32_t *top = *(const uint32_t *const *)tcb;
	if (!ram_range(top, 9 * 4))
		return NULL;
	const uint32_t *hw = top + 9 + (top[8] & 0x10 ? 0 : 16);
	return ram_range(hw, 8 * 4) ? hw : NULL;
}

static void put_fault(out_t *o, const fw_blackbox_t *bb)
{
	put(o, "\n[Fault]\nCore: ");
	put(o, bb->fault_core ? "1 (UI)" : "0 (tasks)");
	put(o, "\nCPU0's task: ");
	put(o, bb->task);
	static const char *const names[8] = {"r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr"};
	for (int i = 0; i < 8; i++) {
		put(o, i % 4 ? "  " : "\n");
		put_reg(o, names[i], bb->frame[i]);
	}
	put(o, "\n");
	put_reg(o, "cfsr", bb->cfsr);
	put(o, "  ");
	put_reg(o, "hfsr", bb->hfsr);
	put(o, "\n");
	put_reg(o, "mmfar", bb->mmfar);
	put(o, "  ");
	put_reg(o, "bfar", bb->bfar);
	put(o, "\n");
	put_reg(o, "sp", bb->sp);
	put(o, "  ");
	put_reg(o, "exc_return", bb->exc_return);
	put(o, "\nStack above the frame:");
	put_words(o, bb->stack, BB_STACK_WORDS);
}

/* Tasks with their CPU use since the previous call, which becomes the new baseline. */
static void put_tasks(out_t *o, fw_blackbox_t *bb, fw_task_status_t *st)
{
	uint32_t total = 0;
	uint32_t n = rom_task_states(st, BB_MAX_TASKS, &total);
	uint32_t dt = total - bb->prev_total;
	const void *self = FW_CURRENT_TCB;
	put(o, "\n[Tasks: name, state (1 ready 2 blocked 3 suspended 4 deleted), priority, CPU % since "
	       "last write, pc, lr, free stack words]\n");
	for (uint32_t i = 0; i < n; i++) {
		put(o, st[i].name);
		put(o, " ");
		put_dec(o, st[i].state);
		put(o, " ");
		put_dec(o, st[i].priority);
		put(o, " ");
		bool found = false;
		for (uint32_t j = 0; j < bb->prev_count && dt; j++)
			if (bb->prev_handle[j] == st[i].handle) {
				put_dec(o, (uint32_t)((uint64_t)(st[i].run_time - bb->prev_run[j]) * 100 / dt));
				found = true;
				break;
			}
		if (!found)
			put(o, "-");
		const uint32_t *hw = st[i].handle != self ? saved_frame(st[i].handle) : NULL;
		put(o, " ");
		put_hex(o, hw ? hw[6] : 0);
		put(o, " ");
		put_hex(o, hw ? hw[5] : 0);
		put(o, " ");
		put_dec(o, st[i].stack_free);
		put(o, "\n");
	}
	for (uint32_t i = 0; i < n; i++) {
		bb->prev_handle[i] = st[i].handle;
		bb->prev_run[i] = st[i].run_time;
	}
	bb->prev_count = n;
	bb->prev_total = total;
}

static void put_state(out_t *o, fw_blackbox_t *bb, work_t *w, uint32_t t, uint32_t beat)
{
	put(o, "Uptime ms: ");
	put_dec(o, rom_tick_count());
	put(o, "\nUI loop last ran ms ago: ");
	put_dec(o, beat ? ms(t - beat) : 0);
	put(o, "\nWatchdog monitor: registered ");
	put_hex(o, WATCH_MONITOR[0]);
	put(o, ", checked in ");
	put_hex(o, WATCH_MONITOR[1]);
	put(o, "\nFault vectors restored by something else: ");
	put_dec(o, bb->repatched);
	put(o, " times\nHeap free ");
	put_dec(o, FW_HEAP_FREE);
	put(o, ", min ");
	put_dec(o, FW_HEAP_MIN_FREE);
	put(o, "\n");
	put_tasks(o, bb, w->st);
}

static int write_file(work_t *w, const char *path, const out_t *o)
{
	if (!fw_sd_mounted())
		return BB_REPORT_NO_CARD;
	if (FW_HEAP_FREE < FATFS_HEAP_MIN)
		return BB_REPORT_NO_MEMORY;
	int res = f_open(&w->file, path, FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK)
		return res;
	uint32_t done;
	res = f_write(&w->file, w->text, (uint32_t)(o->p - w->text), &done);
	int res2 = f_close(&w->file);
	return res != FR_OK ? res : res2;
}

static int write_report(fw_blackbox_t *bb, work_t *w, uint32_t t, uint32_t beat)
{
	out_t o = {w->text, w->text + TEXT_SIZE};
	put(&o, "Black box report\nReason: ");
	put(&o, bb->faulted ? "fault\n" : "UI loop stopped\n");
	put_state(&o, bb, w, t, beat);
	if (bb->faulted)
		put_fault(&o, bb);
	return write_file(w, REPORT_PATH, &o);
}

static void write_status(fw_blackbox_t *bb, work_t *w, uint32_t t, uint32_t beat)
{
	out_t o = {w->text, w->text + TEXT_SIZE};
	put(&o, "Black box status, rewritten every second (write #");
	put_dec(&o, ++bb->status_writes);
	put(&o, ")\n");
	put_state(&o, bb, w, t, beat);
	bb->status_result = write_file(w, STATUS_PATH, &o);
}

/* ---------------------------------------------------------------- service hooks (CPU0) */

/* Points fault vectors 3-6 at bb_fault_entry; returns whether any had to be changed. */
static bool hook_vectors(fw_blackbox_t *bb)
{
	uint32_t *vt = (uint32_t *)REG(SCB_VTOR);
	uint32_t entry = (uint32_t)bb_fault_entry;
	bool changed = false;
	for (int i = 3; i <= 6; i++)
		if (vt[i] == bb->orig_fault) {
			vt[i] = entry;
			changed = true;
		}
	return changed;
}

void bb_tick(fw_blackbox_t *bb)
{
	work_t *w = bb->work;
	if (hook_vectors(bb))
		bb->repatched++;
	uint32_t beat = bb->view_beat; /* before now(), so t - beat can't wrap */
	uint32_t t = now();
	bool hung = beat && ms(t - beat) > BB_HANG_MS;
	if (bb->report == BB_REPORT_NONE && (bb->faulted || hung))
		bb->report = write_report(bb, w, t, beat);
	else if (bb->status_file && ++w->ticks % BB_STATUS_TICKS == 0)
		write_status(bb, w, t, beat);
}

fw_blackbox_t *bb_start(void)
{
	uint32_t *vt = (uint32_t *)REG(SCB_VTOR);
	if (!ram_range(vt, 7 * 4))
		return NULL;
	fw_blackbox_t *bb = fw_os_calloc(1, sizeof(*bb));
	if (!fw_is_ram(bb))
		return NULL;
	bb->work = fw_os_calloc(1, sizeof(work_t));
	if (!fw_is_ram(bb->work)) {
		fw_os_free(bb);
		return NULL;
	}
	bb->orig_fault = vt[3];
	bb->report = BB_REPORT_NONE;
	bb->status_result = BB_REPORT_NONE;
	hook_vectors(bb);
	return bb;
}
