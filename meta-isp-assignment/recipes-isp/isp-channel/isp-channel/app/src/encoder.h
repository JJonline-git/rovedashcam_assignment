/*
 * encoder.h - H.264 encode + MP4 mux with GStreamer (C API).
 *
 *   appsrc -> v4l2h264enc (Pi 4 hardware H.264) -> h264parse -> tee -> splitmuxsink (.mp4 segments)
 *                                                                   \-> rtph264pay -> udpsink (optional)
 *
 * v4l2h264enc talks V4L2 to the bcm2835-codec driver INSIDE GStreamer. Our code only uses
 * the GStreamer API, which respects the "no V4L2 in userspace app code" rule.
 */
#ifndef ENCODER_H
#define ENCODER_H
#include <stddef.h>
#include <stdint.h>

#include "stats.h"

struct encoder_options {
	unsigned width, height, fps;
	unsigned bitrate;              /* bits/s */
	const char *outdir;
	unsigned segment_sec;          /* one .mp4 per segment, like a dash cam loop recorder */
	unsigned max_files;            /* ring buffer of segments; 0 = keep all (fills the disk) */
	const char *udp_host;          /* NULL = no network stream */
	int udp_port;
	int zero_copy;                 /* 1 = encoder imports our dmabuf directly */
	const char *colorimetry;       /* GStreamer colorimetry of the ISP output, e.g. "1:4:7:1" */
};

/* One viewfinder frame. All planes are in the same dmabuf (true on the Pi). */
struct encoder_frame {
	int fd;                        /* dmabuf fd (zero-copy path) */
	const uint8_t *cpu;            /* CPU mapping of the start of that dmabuf (copy path) */
	size_t size;                   /* bytes covering all planes */
	size_t offset[3];
	int stride[3];
	uint64_t sensor_ts_ns;         /* frame timestamp, CLOCK_MONOTONIC */
	void *cookie;                  /* handed back through the release callback */
};

/* Called when GStreamer no longer needs a frame (any thread). */
typedef void (*encoder_release_fn)(void *user, void *cookie);

struct encoder;
struct encoder *encoder_start(const struct encoder_options *o, encoder_release_fn fn, void *user);
int encoder_push(struct encoder *e, const struct encoder_frame *f);   /* 0 = ok, -1 = dropped */
int encoder_check_bus(struct encoder *e);                              /* -1 on pipeline error */
void encoder_take_latency(struct encoder *e, struct timing *out);      /* sensor -> encoded */
void encoder_stop(struct encoder *e);                                  /* EOS, finalise, free */
#endif
