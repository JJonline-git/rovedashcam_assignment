/*
 * snapshot.h - on a trigger, save a PNG of the ISP output and the untouched raw Bayer frame.
 *
 * The main thread COPIES the frame out of the camera buffers (a few ms) and returns the
 * Request to the camera. A worker thread then converts YUV->RGB and writes the PNG
 * (~100+ ms), so the video does not drop frames while PNG compression runs.
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>

#include "camera.h"

struct snapshot_job {
	char base_path[256];           /* e.g. /var/lib/isp-channel/snap-000003 */
	unsigned width, height;
	uint8_t *y, *u, *v;            /* tightly packed copies (stride padding removed) */
	int rec709, full_range;
	uint8_t *raw;                  /* raw Bayer copy, bit-exact (still CSI-2 packed) */
	size_t raw_len;
	char raw_info[256];            /* written to <base>.txt */
};

/* Copy a frame out of the camera buffers into a new job (main thread). */
struct snapshot_job *snapshot_copy(const struct cam *c, const struct cam_frame *f,
				   const char *outdir, unsigned index);
/* 1 while the worker thread is still writing the previous snapshot. */
int snapshot_busy(void);
/* Hand the job to a worker thread; it frees the job when done. */
void snapshot_submit(struct snapshot_job *job);
/* Wait for a running worker (call at exit). */
void snapshot_wait(void);
#endif
