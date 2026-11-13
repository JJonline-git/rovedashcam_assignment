#pragma once
// Everything that talks to libcamera: pick the camera, configure the Viewfinder (YUV420)
// and Raw (Bayer) streams, log and apply controls, allocate buffers, and recycle Requests.
#include <libcamera/libcamera.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "mapped_buffer.h"

enum class RawMode {
    Full,   // Raw stream at the sensor's largest mode (what the brief asks). Caps FPS.
    Video,  // Raw stream at the smallest sensor mode that covers the viewfinder (keeps 30 fps).
    Off,    // No Raw stream.
};

struct ChannelOptions {
    std::string cameraId;          // empty = first camera
    unsigned width = 1920, height = 1080, fps = 30;
    RawMode raw = RawMode::Full;
    unsigned bufferCount = 6;      // per stream; see camera_channel.cpp for why 6

    // Controls. Unset = leave to the IPA default.
    std::optional<bool> aeEnable, awbEnable;
    std::optional<int32_t> exposureUs;
    std::optional<float> gain, brightness, contrast;
    std::optional<std::string> awbMode;
};

// Events delivered to the main thread.
struct Event {
    enum Type { Completed, Released } type;
    libcamera::Request *request;
    uint64_t whenNs;               // when the event was raised (for processing-time stats)
};

class EventQueue {
public:
    void push(const Event &e);
    // Waits up to timeoutMs; returns false on timeout.
    bool pop(Event &e, int timeoutMs);

private:
    std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Event> q_;
};

class CameraChannel {
public:
    CameraChannel(EventQueue &events) : events_(events) {}
    ~CameraChannel();

    bool open(libcamera::CameraManager &cm, const ChannelOptions &opts, bool listOnly = false);
    bool start();
    void requeue(libcamera::Request *req);   // request recycling
    void stop();

    libcamera::Stream *viewfinder() const { return vfStream_; }
    libcamera::Stream *raw() const { return rawStream_; }
    const MappedBuffer *mapped(const libcamera::FrameBuffer *fb) const;
    std::string cameraModel() const;

private:
    void logControls() const;
    void buildControls();
    void onRequestCompleted(libcamera::Request *req);   // runs in libcamera's thread

    EventQueue &events_;
    ChannelOptions opts_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> config_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    std::map<const libcamera::FrameBuffer *, std::unique_ptr<MappedBuffer>> maps_;
    libcamera::ControlList startControls_;
    libcamera::Stream *vfStream_ = nullptr;
    libcamera::Stream *rawStream_ = nullptr;
    bool acquired_ = false, started_ = false;
};
