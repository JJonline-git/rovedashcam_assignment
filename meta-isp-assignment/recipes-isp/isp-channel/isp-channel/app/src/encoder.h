#pragma once
// H.264 encode + mux with GStreamer:
//   appsrc -> v4l2h264enc (Pi 4 hardware encoder) -> h264parse -> tee -> splitmuxsink (.mp4 segments)
//                                                                      \-> rtph264pay -> udpsink (optional)
// GStreamer's v4l2h264enc talks V4L2 to the bcm2835-codec driver internally; our code
// only uses the GStreamer API, which keeps the "no V4L2 in userspace app code" rule.
#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "stats.h"

struct EncoderOptions {
    unsigned width = 1920, height = 1080, fps = 30;
    unsigned bitrate = 8000000;       // bits/s
    std::string outdir = ".";
    unsigned segmentSec = 60;         // one .mp4 per minute, like a dash cam loop recorder
    std::string udpHost;              // empty = no network stream
    int udpPort = 5000;
    bool zeroCopy = true;
};

// One frame handed to the encoder. All planes are in the same dmabuf (true on the Pi).
struct EncoderFrame {
    int fd;                           // dmabuf fd (zero-copy path)
    const uint8_t *cpu;               // CPU mapping of the same buffer (copy path)
    size_t size;                      // bytes covering all planes
    size_t offset[3];
    int stride[3];
    uint64_t sensorTsNs;              // FrameMetadata::timestamp (CLOCK_MONOTONIC)
    void *cookie;                     // returned through the release callback
};

class Encoder {
public:
    using ReleaseFn = std::function<void(void *cookie)>;

    ~Encoder();
    bool start(const EncoderOptions &opts, ReleaseFn onRelease);
    bool push(const EncoderFrame &f);
    bool checkBus();                  // returns false on a pipeline error
    void stop();                      // EOS, wait for the mp4 to be finalised, tear down

    // Sensor timestamp -> encoded H.264 frame leaving the encoder.
    TimingStat takeLatency();

private:
    static GstPadProbeReturn encodedProbe(GstPad *, GstPadProbeInfo *info, gpointer self);

    EncoderOptions opts_;
    ReleaseFn onRelease_;
    GstElement *pipeline_ = nullptr;
    GstElement *appsrc_ = nullptr;
    GstAllocator *dmabufAlloc_ = nullptr;
    std::atomic<uint64_t> firstTsNs_{ 0 };  // read by the probe in a GStreamer thread
    std::mutex latLock_;
    TimingStat latency_;
};
