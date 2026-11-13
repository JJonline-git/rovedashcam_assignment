#include "encoder.h"
#include "log.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

// Attached to every GstBuffer we create. When GStreamer frees the buffer (the encoder is
// done reading it) GLib calls releaseCookie(), and we hand the Request back to the camera.
struct ReleaseCtx {
    Encoder::ReleaseFn *fn;
    void *cookie;
};

static void releaseCookie(gpointer data)
{
    auto *ctx = static_cast<ReleaseCtx *>(data);
    (*ctx->fn)(ctx->cookie);
    delete ctx;
}

static GQuark releaseQuark()
{
    static GQuark q = g_quark_from_static_string("isp-release");
    return q;
}

Encoder::~Encoder()
{
    stop();
}

bool Encoder::start(const EncoderOptions &opts, ReleaseFn onRelease)
{
    opts_ = opts;
    onRelease_ = std::move(onRelease);

    // gst_parse_launch keeps the pipeline readable as one string.
    //  - appsrc is-live/format=time: we provide real timestamps from the sensor.
    //  - output-io-mode=dmabuf-import: the encoder reads our camera buffer directly (zero copy).
    //    Without it, v4l2h264enc copies each frame into its own buffers (1 copy).
    //  - level=4: the Pi encoder needs an explicit level for 1080p30.
    //  - repeat_sequence_header + config-interval: SPS/PPS on every IDR so a stream
    //    receiver can join at any time.
    //  - splitmuxsink: closes a complete .mp4 every segmentSec. After a power cut only the
    //    current segment is lost (an unfinished mp4 has no index).
    std::string desc =
        "appsrc name=src is-live=true format=time do-timestamp=false block=false max-buffers=4 "
        "! v4l2h264enc name=enc " + std::string(opts.zeroCopy ? "output-io-mode=dmabuf-import " : "") +
        "extra-controls=\"controls,video_bitrate=" + std::to_string(opts.bitrate) +
        ",h264_i_frame_period=" + std::to_string(opts.fps) + ",repeat_sequence_header=1\" "
        "! video/x-h264,level=(string)4,profile=(string)high "
        "! h264parse config-interval=-1 ! tee name=t "
        "t. ! queue ! splitmuxsink name=mux location=" + opts.outdir + "/seg-%05d.mp4 "
        "max-size-time=" + std::to_string(uint64_t(opts.segmentSec) * GST_SECOND) + " ";
    if (!opts.udpHost.empty())
        desc += "t. ! queue leaky=downstream max-size-buffers=30 ! rtph264pay pt=96 config-interval=1 "
                "! udpsink host=" + opts.udpHost + " port=" + std::to_string(opts.udpPort) +
                " sync=false async=false";

    LOG("gst pipeline: %s", desc.c_str());
    GError *err = nullptr;
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_) {
        ERR("gst_parse_launch: %s", err ? err->message : "?");
        g_clear_error(&err);
        return false;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, int(opts.width),
        "height", G_TYPE_INT, int(opts.height),
        "framerate", GST_TYPE_FRACTION, int(opts.fps), 1, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(appsrc_), caps);
    gst_caps_unref(caps);

    // Latency probe on the encoder output: buffer PTS maps back to the sensor timestamp.
    GstElement *enc = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
    GstPad *pad = gst_element_get_static_pad(enc, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &Encoder::encodedProbe, this, nullptr);
    gst_object_unref(pad);
    gst_object_unref(enc);

    dmabufAlloc_ = gst_dmabuf_allocator_new();

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        ERR("pipeline failed to start");
        return false;
    }
    return true;
}

bool Encoder::push(const EncoderFrame &f)
{
    if (!firstTsNs_)
        firstTsNs_ = f.sensorTsNs;

    GstBuffer *buf;
    if (opts_.zeroCopy) {
        // Wrap the camera's dmabuf fd. DONT_CLOSE: libcamera owns the fd, not GStreamer.
        GstMemory *mem = gst_dmabuf_allocator_alloc_with_flags(dmabufAlloc_, f.fd, f.size,
                                                               GST_FD_MEMORY_FLAG_DONT_CLOSE);
        buf = gst_buffer_new();
        gst_buffer_append_memory(buf, mem);
    } else {
        // Wrap the CPU mapping (no copy here; v4l2h264enc copies into its own buffers).
        buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
                                          const_cast<uint8_t *>(f.cpu), f.size, 0, f.size,
                                          nullptr, nullptr);
    }

    // Plane layout as libcamera produced it (stride may be padded for the ISP's DMA alignment).
    gsize offsets[GST_VIDEO_MAX_PLANES] = { f.offset[0], f.offset[1], f.offset[2] };
    gint strides[GST_VIDEO_MAX_PLANES] = { f.stride[0], f.stride[1], f.stride[2] };
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420,
                                   opts_.width, opts_.height, 3, offsets, strides);

    // PTS relative to the first frame, from the sensor clock (not "now"), so frame timing
    // in the mp4 reflects real capture times, including any drops.
    GST_BUFFER_PTS(buf) = f.sensorTsNs - firstTsNs_;
    GST_BUFFER_DURATION(buf) = GST_SECOND / opts_.fps;

    gst_mini_object_set_qdata(GST_MINI_OBJECT_CAST(buf), releaseQuark(),
                              new ReleaseCtx{ &onRelease_, f.cookie }, releaseCookie);

    // push_buffer takes ownership. If appsrc is full (encoder stalled) the buffer is
    // still accepted up to max-buffers; beyond that we drop rather than block the camera.
    if (gst_app_src_get_current_level_buffers(GST_APP_SRC(appsrc_)) >= 4) {
        gst_buffer_unref(buf);          // releases the request immediately
        return false;
    }
    return gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf) == GST_FLOW_OK;
}

GstPadProbeReturn Encoder::encodedProbe(GstPad *, GstPadProbeInfo *info, gpointer self)
{
    auto *e = static_cast<Encoder *>(self);
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf && GST_BUFFER_PTS_IS_VALID(buf)) {
        uint64_t sensorTs = e->firstTsNs_ + GST_BUFFER_PTS(buf);
        double us = double(monotonicNs() - sensorTs) / 1000.0;
        std::lock_guard<std::mutex> l(e->latLock_);
        e->latency_.add(us);
    }
    return GST_PAD_PROBE_OK;
}

TimingStat Encoder::takeLatency()
{
    std::lock_guard<std::mutex> l(latLock_);
    TimingStat t = latency_;
    latency_.reset();
    return t;
}

bool Encoder::checkBus()
{
    if (!pipeline_)
        return true;
    GstBus *bus = gst_element_get_bus(pipeline_);
    bool ok = true;
    while (GstMessage *msg = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
        GError *err = nullptr;
        gchar *dbg = nullptr;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &err, &dbg);
            ERR("gst: %s (%s)", err->message, dbg ? dbg : "");
            ok = false;
        } else {
            gst_message_parse_warning(msg, &err, &dbg);
            WARN("gst: %s (%s)", err->message, dbg ? dbg : "");
        }
        g_clear_error(&err);
        g_free(dbg);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return ok;
}

void Encoder::stop()
{
    if (!pipeline_)
        return;
    // EOS flows through the encoder and muxer; splitmuxsink writes the mp4 index (moov).
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    GstBus *bus = gst_element_get_bus(pipeline_);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
                                                 GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (!msg)
        WARN("EOS timeout; last segment may be incomplete");
    else
        gst_message_unref(msg);
    gst_object_unref(bus);

    // Going to NULL frees every buffer still inside the pipeline -> remaining releases fire.
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(appsrc_);
    gst_object_unref(pipeline_);
    gst_object_unref(dmabufAlloc_);
    pipeline_ = nullptr;
    appsrc_ = nullptr;
    dmabufAlloc_ = nullptr;
}
