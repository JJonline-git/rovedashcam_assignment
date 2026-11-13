/* stats.h - measurements for the report: FPS, drops, CPU %, RSS, timing min/avg/max. */
#ifndef STATS_H
#define STATS_H
#include <stdint.h>

uint64_t monotonic_ns(void);

/* Drop detection: libcamera numbers every sensor frame (FrameMetadata::sequence).
 * If the number jumps by more than 1, the frames in between were never delivered,
 * usually because no Request was queued in time. */
struct stream_counter {
	uint64_t frames, drops;            /* current stats window */
	uint64_t total_frames, total_drops;
	int64_t last_seq;                  /* -1 = none yet */
};
void counter_init(struct stream_counter *c);
void counter_frame(struct stream_counter *c, uint32_t seq);
void counter_reset_window(struct stream_counter *c);

struct timing {
	double min_us, max_us, sum_us;
	uint64_t n;
};
void timing_reset(struct timing *t);
void timing_add(struct timing *t, double us);
/* Formats "min X / avg Y / max Z us" into buf. */
const char *timing_str(const struct timing *t, char *buf, int len);

/* CPU % of this process since the previous sample (100% = one core), and resident memory. */
struct proc_sampler {
	uint64_t last_ticks, last_ns;
};
void proc_init(struct proc_sampler *p);
void proc_sample(struct proc_sampler *p, double *cpu_percent, long *rss_kb);
#endif
