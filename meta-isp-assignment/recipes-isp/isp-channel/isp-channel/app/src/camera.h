/*
 * camera.h - C interface to the camera.
 *
 * libcamera only has a C++ API. This header is the boundary: everything behind it is in
 * camera.cpp (the only C++ file), everything that uses it is plain C.
 *
 * Model:
 *   cam_open()        enumerate, configure streams, allocate buffers, create Requests
 *   cam_start()       start streaming, queue all Requests
 *   cam_wait_frame()  get the next completed frame (a Request with its buffers)
 *   cam_release()     give the frame back; its Request is requeued (request recycling)
 *   cam_stop/close()  stop streaming, free everything
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>
#include <stdint.h>

/* extern "C": when a C++ compiler reads this header, do not "mangle" the function names,
 * so C code can link against them. A C compiler ignores this block. */
#ifdef __cplusplus
extern "C" {
#endif

enum cam_raw_mode {
	CAM_RAW_FULL,   /* raw stream at the largest sensor mode (brief); caps the frame rate */
	CAM_RAW_VIDEO,  /* raw stream at the smallest mode that covers the viewfinder; keeps 30 fps */
	CAM_RAW_OFF,    /* no raw stream */
};

struct cam_options {
	const char *camera_id;         /* NULL = first camera */
	unsigned width, height, fps;   /* viewfinder */
	enum cam_raw_mode raw;
	unsigned buffer_count;         /* per stream */

	/* Controls: only applied when the matching has_* flag is 1. */
	int has_ae, ae;
	int has_awb, awb;
	int has_exposure; int32_t exposure_us;
	int has_gain; float gain;
	int has_brightness; float brightness;
	int has_contrast; float contrast;
	const char *awb_mode;          /* NULL = leave default */
};

struct cam_stream_info {
	unsigned width, height, stride;
	char pixel_format[32];         /* e.g. "YUV420", "GBRG10_CSI2P" */
	int rec709;                    /* 1 = BT.709 YCbCr matrix, 0 = BT.601 */
	int full_range;                /* 1 = 0..255, 0 = 16..235 */
};

struct cam_plane {
	int fd;                        /* dmabuf file descriptor (owned by libcamera) */
	size_t offset, length;
	const uint8_t *cpu;            /* CPU mapping of this plane (mapped once at open) */
};

struct cam_buffer {
	unsigned num_planes;
	struct cam_plane plane[3];
	uint32_t sequence;             /* frame counter from the sensor; gaps = dropped frames */
	uint64_t timestamp_ns;         /* CLOCK_MONOTONIC, start of frame */
	int ok;                        /* 1 = captured successfully */
};

struct cam_frame {
	void *handle;                  /* opaque: the libcamera Request. Pass to cam_release(). */
	uint64_t completed_ns;         /* CLOCK_MONOTONIC when libcamera completed it */
	struct cam_buffer vf;          /* viewfinder YUV420 from the ISP */
	struct cam_buffer raw;         /* raw Bayer from the sensor, valid if has_raw */
	int has_raw;
	/* What AE/AWB actually applied to this frame (request metadata), -1 if unknown. */
	int32_t exposure_us;
	float analogue_gain, colour_gain_r, colour_gain_b, lux;
	int32_t colour_temp_k;
};

struct cam;                        /* opaque handle; the C side never sees inside */

struct cam *cam_open(const struct cam_options *opts, int list_only);
int cam_start(struct cam *c);
const char *cam_model(const struct cam *c);
/* stream: 0 = viewfinder, 1 = raw. Returns 0 on success, -1 if the stream does not exist. */
int cam_stream_info(const struct cam *c, int stream, struct cam_stream_info *info);
/* Returns 1 with a frame, 0 on timeout. */
int cam_wait_frame(struct cam *c, struct cam_frame *frame, int timeout_ms);
/* Thread-safe: may be called from any thread (e.g. a GStreamer thread). */
void cam_release(struct cam *c, void *handle);
void cam_stop(struct cam *c);
void cam_close(struct cam *c);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
