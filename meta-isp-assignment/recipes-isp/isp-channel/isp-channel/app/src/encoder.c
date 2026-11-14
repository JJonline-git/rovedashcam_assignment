#include "encoder.h"
#include "log.h"

#include <stdlib.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#define APPSRC_MAX_QUEUED 4

struct encoder {
	struct encoder_options opts;
	encoder_release_fn release_fn;
	void *release_user;
	GstElement *pipeline;
	GstElement *appsrc;
	GstAllocator *dmabuf_alloc;
	/* Written once by the main thread before the first push; the probe only reads it after
	 * that buffer went through appsrc's internal lock, so no extra locking is needed. */
	uint64_t first_ts_ns;
	GMutex lat_lock;               /* latency is written by a GStreamer thread */
	struct timing latency;
};

/* Attached to each GstBuffer. When the buffer's refcount reaches 0 (the encoder is done
 * reading it), GLib calls release_ctx_free() and we hand the frame back to the camera. */
struct release_ctx {
	struct encoder *enc;
	void *cookie;
};

static void release_ctx_free(gpointer data)
{
	struct release_ctx *ctx = data;
	ctx->enc->release_fn(ctx->enc->release_user, ctx->cookie);
	free(ctx);
}

static GQuark release_quark(void)
{
	static GQuark q;
	if (!q)
		q = g_quark_from_static_string("isp-release");
	return q;
}

/* Pad probe on the encoder output: runs for every encoded frame. */
static GstPadProbeReturn encoded_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
	struct encoder *e = data;
	GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
	(void)pad;
	if (buf && GST_BUFFER_PTS_IS_VALID(buf)) {
		/* PTS = sensor timestamp - first timestamp, so this recovers the sensor time. */
		uint64_t sensor_ts = e->first_ts_ns + GST_BUFFER_PTS(buf);
		double us = (double)(monotonic_ns() - sensor_ts) / 1000.0;
		g_mutex_lock(&e->lat_lock);
		timing_add(&e->latency, us);
		g_mutex_unlock(&e->lat_lock);
	}
	return GST_PAD_PROBE_OK;
}

struct encoder *encoder_start(const struct encoder_options *o, encoder_release_fn fn, void *user)
{
	struct encoder *e = calloc(1, sizeof(*e));
	e->opts = *o;
	e->release_fn = fn;
	e->release_user = user;
	g_mutex_init(&e->lat_lock);
	timing_reset(&e->latency);

	/*
	 * The whole pipeline as one readable string:
	 *  - appsrc is-live format=time: we push buffers with real sensor timestamps.
	 *  - output-io-mode=dmabuf-import: the encoder reads our camera buffer directly
	 *    (zero copy). Without it, v4l2h264enc copies each frame into its own buffers.
	 *  - h264_i_frame_period=fps: one keyframe per second (seeking, stream join).
	 *  - repeat_sequence_header + config-interval=-1: SPS/PPS before every keyframe.
	 *  - level=4: the Pi encoder needs an explicit level for 1080p30.
	 *  - splitmuxsink: closes a complete .mp4 every segment. An mp4 is only playable once
	 *    its index is written at the end, so a power cut loses only the current segment.
	 *  - leaky queue on the network branch: a slow network drops packets, never stalls recording.
	 */
	gchar *udp = o->udp_host
		? g_strdup_printf("t. ! queue leaky=downstream max-size-buffers=30 "
				  "! rtph264pay pt=96 config-interval=1 "
				  "! udpsink host=%s port=%d sync=false async=false", o->udp_host, o->udp_port)
		: g_strdup("");
	gchar *desc = g_strdup_printf(
		"appsrc name=src is-live=true format=time do-timestamp=false "
		"! v4l2h264enc name=enc %s "
		"extra-controls=\"controls,video_bitrate=%u,h264_i_frame_period=%u,repeat_sequence_header=1\" "
		"! video/x-h264,level=(string)4,profile=(string)high "
		"! h264parse config-interval=-1 ! tee name=t "
		"t. ! queue ! splitmuxsink location=%s/seg-%%05d.mp4 max-size-time=%" G_GUINT64_FORMAT " %s",
		o->zero_copy ? "output-io-mode=dmabuf-import" : "",
		o->bitrate, o->fps, o->outdir, (guint64)o->segment_sec * GST_SECOND, udp);
	LOG("gst pipeline: %s", desc);

	GError *err = NULL;
	e->pipeline = gst_parse_launch(desc, &err);
	g_free(desc);
	g_free(udp);
	if (!e->pipeline) {
		ERR("gst_parse_launch: %s", err ? err->message : "?");
		g_clear_error(&err);
		free(e);
		return NULL;
	}

	e->appsrc = gst_bin_get_by_name(GST_BIN(e->pipeline), "src");
	GstCaps *caps = gst_caps_new_simple("video/x-raw",
		"format", G_TYPE_STRING, "I420",
		"width", G_TYPE_INT, (int)o->width,
		"height", G_TYPE_INT, (int)o->height,
		"framerate", GST_TYPE_FRACTION, (int)o->fps, 1, NULL);
	/* v4l2h264enc only accepts fixed caps: without colorimetry the caps stay unfixed and
	 * negotiation fails ("not-negotiated"). Tell it what the ISP really produced. */
	if (o->colorimetry)
		gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING, o->colorimetry, NULL);
	gst_app_src_set_caps(GST_APP_SRC(e->appsrc), caps);
	gst_caps_unref(caps);

	GstElement *enc = gst_bin_get_by_name(GST_BIN(e->pipeline), "enc");
	GstPad *pad = gst_element_get_static_pad(enc, "src");
	gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, encoded_probe, e, NULL);
	gst_object_unref(pad);
	gst_object_unref(enc);

	e->dmabuf_alloc = gst_dmabuf_allocator_new();

	if (gst_element_set_state(e->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		ERR("pipeline failed to start");
		encoder_stop(e);
		return NULL;
	}
	return e;
}

int encoder_push(struct encoder *e, const struct encoder_frame *f)
{
	/* Backpressure: if the encoder is stalled, drop this frame instead of blocking the
	 * camera loop. Returning -1 tells the caller to release the frame itself. */
	if (gst_app_src_get_current_level_buffers(GST_APP_SRC(e->appsrc)) >= APPSRC_MAX_QUEUED)
		return -1;

	if (!e->first_ts_ns)
		e->first_ts_ns = f->sensor_ts_ns;

	GstBuffer *buf;
	if (e->opts.zero_copy) {
		/* Wrap the camera's dmabuf fd. DONT_CLOSE: libcamera owns the fd, not GStreamer. */
		GstMemory *mem = gst_dmabuf_allocator_alloc_with_flags(e->dmabuf_alloc, f->fd, f->size,
								       GST_FD_MEMORY_FLAG_DONT_CLOSE);
		buf = gst_buffer_new();
		gst_buffer_append_memory(buf, mem);
	} else {
		/* Wrap our CPU mapping (no copy here); v4l2h264enc then copies into its own buffers. */
		buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, (gpointer)f->cpu,
						  f->size, 0, f->size, NULL, NULL);
	}

	/* Real plane layout: the ISP may pad each row (stride) for DMA alignment. */
	gsize offsets[GST_VIDEO_MAX_PLANES] = { f->offset[0], f->offset[1], f->offset[2] };
	gint strides[GST_VIDEO_MAX_PLANES] = { f->stride[0], f->stride[1], f->stride[2] };
	gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420,
				       e->opts.width, e->opts.height, 3, offsets, strides);

	/* Timestamps from the sensor clock, so mp4 timing follows real capture times. */
	GST_BUFFER_PTS(buf) = f->sensor_ts_ns - e->first_ts_ns;
	GST_BUFFER_DURATION(buf) = GST_SECOND / e->opts.fps;

	struct release_ctx *ctx = malloc(sizeof(*ctx));
	ctx->enc = e;
	ctx->cookie = f->cookie;
	gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(buf), release_quark(), ctx, release_ctx_free);

	/* push_buffer takes ownership of buf (we must not unref it). */
	return gst_app_src_push_buffer(GST_APP_SRC(e->appsrc), buf) == GST_FLOW_OK ? 0 : -1;
}

int encoder_check_bus(struct encoder *e)
{
	int ret = 0;
	GstBus *bus = gst_element_get_bus(e->pipeline);
	GstMessage *msg;
	while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
		GError *err = NULL;
		gchar *dbg = NULL;
		if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
			gst_message_parse_error(msg, &err, &dbg);
			ERR("gst: %s (%s)", err->message, dbg ? dbg : "");
			ret = -1;
		} else {
			gst_message_parse_warning(msg, &err, &dbg);
			WARN("gst: %s (%s)", err->message, dbg ? dbg : "");
		}
		g_clear_error(&err);
		g_free(dbg);
		gst_message_unref(msg);
	}
	gst_object_unref(bus);
	return ret;
}

void encoder_take_latency(struct encoder *e, struct timing *out)
{
	g_mutex_lock(&e->lat_lock);
	*out = e->latency;
	timing_reset(&e->latency);
	g_mutex_unlock(&e->lat_lock);
}

void encoder_stop(struct encoder *e)
{
	if (!e)
		return;
	/* EOS flows through encoder and muxer; splitmuxsink then writes the mp4 index. */
	gst_app_src_end_of_stream(GST_APP_SRC(e->appsrc));
	GstBus *bus = gst_element_get_bus(e->pipeline);
	GstMessage *msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
						     GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
	if (msg)
		gst_message_unref(msg);
	else
		WARN("EOS timeout; the last segment may be incomplete");
	gst_object_unref(bus);

	/* NULL state frees every buffer still in the pipeline: the remaining releases fire. */
	gst_element_set_state(e->pipeline, GST_STATE_NULL);
	gst_object_unref(e->appsrc);
	gst_object_unref(e->pipeline);
	gst_object_unref(e->dmabuf_alloc);
	g_mutex_clear(&e->lat_lock);
	free(e);
}
