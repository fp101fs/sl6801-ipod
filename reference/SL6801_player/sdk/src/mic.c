/*
 * Microphone capture from /dev/audio1, kept in a ring by a worker task on CPU0 (the UI runs on
 * CPU1 and must not touch devices; see "the two cores" in sdk.h). As with the sound module, the
 * task owns the device from open to close (reads block until a period is in).
 *
 * Dmitry Brant, 2026
 */
#include "sdk.h"

#define MIC_DEV "/dev/audio1"
#define PERIOD 128 /* frames per read; FW_MIC_RING is a multiple of it */
#define SOURCE 7
#define TASK_PRIORITY 6
#define TASK_STACK 0x400 /* words */

enum { MIC_OPENING, MIC_OPEN, MIC_FAILED };

/* The worker's memory. */
typedef struct {
	void *dev;
	volatile uint8_t status;  /* MIC_* */
	volatile uint32_t total; /* frames captured so far */
	int16_t ring[FW_MIC_RING * 2];
} mic_t;

static void mic_task(fw_worker_t *w)
{
	mic_t *m = w->mem;
	static const uint32_t params[10] = {2, 2, FW_MIC_RATE, PERIOD, 2, 0, 0, SOURCE << 8, 0, 0};
	if (!fw_dev_busy(MIC_DEV))
		m->dev = fw_dev_open(MIC_DEV, 0, params);
	if (!m->dev) {
		m->status = MIC_FAILED;
		return;
	}
	m->status = MIC_OPEN;
	while (!w->stop) {
		uint32_t pos = m->total % FW_MIC_RING;
		if (fw_dev_read(m->dev, &m->ring[pos * 2], PERIOD, 0) != PERIOD) {
			fw_os_sleep(10);
			continue;
		}
		__sync_synchronize();
		m->total += PERIOD;
	}
	fw_dev_close(&m->dev);
}

fw_mic_t *fw_mic_open(void)
{
	return fw_worker_start(mic_task, sizeof(mic_t), "mic", TASK_PRIORITY, TASK_STACK);
}

bool fw_mic_failed(const fw_mic_t *mic)
{
	const mic_t *m = fw_worker_mem(mic);
	return fw_worker_failed(mic) || (m && m->status == MIC_FAILED);
}

/* The task fills the period after the newest one, so the newest half of the ring stays intact
 * while it is copied. */
uint32_t fw_mic_latest(fw_mic_t *mic, int16_t *dst, uint32_t frames)
{
	const mic_t *m = fw_worker_mem(mic);
	if (!m || m->status != MIC_OPEN)
		return 0;
	uint32_t total = m->total;
	__sync_synchronize();
	if (frames > FW_MIC_RING / 2)
		frames = FW_MIC_RING / 2;
	if (frames > total)
		frames = total;
	uint32_t pos = (total - frames) % FW_MIC_RING;
	uint32_t first = frames < FW_MIC_RING - pos ? frames : FW_MIC_RING - pos;
	memcpy(dst, &m->ring[2 * pos], first * 4);
	memcpy(dst + 2 * first, m->ring, (frames - first) * 4);
	return frames;
}

void fw_mic_close(fw_mic_t *mic)
{
	fw_worker_release(mic);
}
