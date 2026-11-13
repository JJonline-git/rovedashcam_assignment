#include "stats.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

uint64_t monotonic_ns(void)
{
	/* Same clock libcamera uses for frame timestamps, so the two can be subtracted. */
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

void counter_init(struct stream_counter *c)
{
	c->frames = c->drops = c->total_frames = c->total_drops = 0;
	c->last_seq = -1;
}

void counter_frame(struct stream_counter *c, uint32_t seq)
{
	if (c->last_seq >= 0 && (int64_t)seq > c->last_seq + 1) {
		uint64_t gap = seq - c->last_seq - 1;
		c->drops += gap;
		c->total_drops += gap;
	}
	c->last_seq = seq;
	c->frames++;
	c->total_frames++;
}

void counter_reset_window(struct stream_counter *c)
{
	c->frames = c->drops = 0;
}

void timing_reset(struct timing *t)
{
	t->min_us = 1e30;
	t->max_us = t->sum_us = 0;
	t->n = 0;
}

void timing_add(struct timing *t, double us)
{
	if (us < t->min_us) t->min_us = us;
	if (us > t->max_us) t->max_us = us;
	t->sum_us += us;
	t->n++;
}

const char *timing_str(const struct timing *t, char *buf, int len)
{
	if (!t->n)
		snprintf(buf, len, "n/a");
	else
		snprintf(buf, len, "min %.0f / avg %.0f / max %.0f us",
			 t->min_us, t->sum_us / t->n, t->max_us);
	return buf;
}

static uint64_t read_cpu_ticks(void)
{
	/* /proc/self/stat: field 14 = utime, 15 = stime, in clock ticks. */
	unsigned long utime = 0, stime = 0;
	FILE *f = fopen("/proc/self/stat", "r");
	if (!f)
		return 0;
	int r = fscanf(f, "%*d (%*[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
		       &utime, &stime);
	fclose(f);
	return r == 2 ? utime + stime : 0;
}

void proc_init(struct proc_sampler *p)
{
	p->last_ticks = read_cpu_ticks();
	p->last_ns = monotonic_ns();
}

void proc_sample(struct proc_sampler *p, double *cpu_percent, long *rss_kb)
{
	uint64_t ticks = read_cpu_ticks();
	uint64_t now = monotonic_ns();
	double cpu_s = (double)(ticks - p->last_ticks) / sysconf(_SC_CLK_TCK);
	double wall_s = (double)(now - p->last_ns) / 1e9;
	*cpu_percent = wall_s > 0 ? 100.0 * cpu_s / wall_s : 0;
	p->last_ticks = ticks;
	p->last_ns = now;

	/* /proc/self/statm: field 2 = resident pages. */
	*rss_kb = 0;
	FILE *f = fopen("/proc/self/statm", "r");
	if (f) {
		long size, resident;
		if (fscanf(f, "%ld %ld", &size, &resident) == 2)
			*rss_kb = resident * (sysconf(_SC_PAGESIZE) / 1024);
		fclose(f);
	}
}
