#include "camera_channel.h"
#include "log.h"
#include "stats.h"

#include <algorithm>

using namespace libcamera;

// ---------------------------------------------------------------- EventQueue

void EventQueue::push(const Event &e)
{
    {
        std::lock_guard<std::mutex> l(lock_);
        q_.push_back(e);
    }
    cv_.notify_one();
}

bool EventQueue::pop(Event &e, int timeoutMs)
{
    std::unique_lock<std::mutex> l(lock_);
    if (!cv_.wait_for(l, std::chrono::milliseconds(timeoutMs), [this] { return !q_.empty(); }))
        return false;
    e = q_.front();
    q_.pop_front();
    return true;
}

// ---------------------------------------------------------------- CameraChannel

CameraChannel::~CameraChannel()
{
    stop();
    requests_.clear();
    maps_.clear();          // munmap before the buffers are freed
    allocator_.reset();     // frees the dmabufs
    if (acquired_)
        camera_->release();
}

std::string CameraChannel::cameraModel() const
{
    auto model = camera_->properties().get(properties::Model);
    return model ? *model : camera_->id();
}

const MappedBuffer *CameraChannel::mapped(const FrameBuffer *fb) const
{
    auto it = maps_.find(fb);
    return it == maps_.end() ? nullptr : it->second.get();
}

bool CameraChannel::open(CameraManager &cm, const ChannelOptions &opts, bool listOnly)
{
    opts_ = opts;

    // 1. Enumerate.
    auto cameras = cm.cameras();
    LOG("found %zu camera(s)", cameras.size());
    for (const auto &c : cameras) {
        auto model = c->properties().get(properties::Model);
        LOG("  %s (%s)", c->id().c_str(), model ? model->c_str() : "?");
    }
    if (cameras.empty())
        return false;

    camera_ = opts.cameraId.empty() ? cameras[0] : cm.get(opts.cameraId);
    if (!camera_) {
        ERR("camera '%s' not found", opts.cameraId.c_str());
        return false;
    }
    if (camera_->acquire()) {        // exclusive access; fails if another process has it
        ERR("cannot acquire camera (busy?)");
        return false;
    }
    acquired_ = true;
    LOG("using %s", cameraModel().c_str());

    logControls();
    if (listOnly)
        return true;

    // 2. Configure streams. Viewfinder = processed YUV from the hardware ISP;
    //    Raw = unprocessed Bayer straight from the CSI-2 receiver (Unicam).
    std::vector<StreamRole> roles = { StreamRole::Viewfinder };
    if (opts.raw != RawMode::Off)
        roles.push_back(StreamRole::Raw);

    config_ = camera_->generateConfiguration(roles);
    if (!config_) {
        ERR("camera cannot generate a configuration for these roles");
        return false;
    }

    StreamConfiguration &vf = config_->at(0);
    vf.pixelFormat = formats::YUV420;
    vf.size = { opts.width, opts.height };
    // 6 buffers: the H.264 encoder holds 2-3 frames (input queue + reference handling)
    // while the camera needs >= 2 queued to never starve. 4 (the default) caused drops.
    vf.bufferCount = opts.bufferCount;

    if (opts.raw != RawMode::Off) {
        StreamConfiguration &raw = config_->at(1);
        // The sizes offered for the Raw stream are the sensor's native modes.
        std::vector<Size> modes = raw.formats().sizes(raw.pixelFormat);
        std::string list;
        for (const Size &s : modes)
            list += s.toString() + " ";
        LOG("sensor modes (%s): %s", raw.pixelFormat.toString().c_str(), list.c_str());

        if (!modes.empty()) {
            if (opts.raw == RawMode::Full) {
                raw.size = *std::max_element(modes.begin(), modes.end(),
                    [](const Size &a, const Size &b) { return a.width * a.height < b.width * b.height; });
            } else {
                // Smallest mode that still covers the viewfinder: binned/cropped, fast.
                std::optional<Size> best;
                for (const Size &s : modes)
                    if (s.width >= vf.size.width && s.height >= vf.size.height &&
                        (!best || s.width * s.height < best->width * best->height))
                        best = s;
                raw.size = best ? *best : modes.back();
            }
        }
        raw.bufferCount = opts.bufferCount;
    }

    // validate() may adjust sizes/formats to something the hardware can do.
    CameraConfiguration::Status st = config_->validate();
    if (st == CameraConfiguration::Invalid) {
        ERR("configuration invalid");
        return false;
    }
    if (st == CameraConfiguration::Adjusted)
        WARN("configuration adjusted by the pipeline handler");

    if (camera_->configure(config_.get()) < 0) {
        ERR("configure failed");
        return false;
    }

    for (unsigned i = 0; i < config_->size(); i++) {
        const StreamConfiguration &c = config_->at(i);
        LOG("stream %u: %s stride %u frameSize %u buffers %u colorspace %s", i,
            c.toString().c_str(), c.stride, c.frameSize, c.bufferCount,
            c.colorSpace ? c.colorSpace->toString().c_str() : "n/a");
    }
    vfStream_ = config_->at(0).stream();
    rawStream_ = config_->size() > 1 ? config_->at(1).stream() : nullptr;

    // 3. Allocate buffers. FrameBufferAllocator asks the pipeline handler for dmabuf-backed
    //    buffers from the driver (CMA memory, DMA-capable, shareable with the encoder).
    allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
    unsigned nRequests = UINT32_MAX;
    for (StreamConfiguration &c : *config_) {
        if (allocator_->allocate(c.stream()) < 0) {
            ERR("buffer allocation failed");
            return false;
        }
        nRequests = std::min<unsigned>(nRequests, allocator_->buffers(c.stream()).size());
        for (const auto &b : allocator_->buffers(c.stream()))
            maps_[b.get()] = std::make_unique<MappedBuffer>(b.get());
    }

    // 4. One Request = one buffer per stream. They are created once and recycled forever:
    //    no allocation in the frame loop, so memory stays flat.
    for (unsigned i = 0; i < nRequests; i++) {
        std::unique_ptr<Request> req = camera_->createRequest(i);
        for (StreamConfiguration &c : *config_)
            if (req->addBuffer(c.stream(), allocator_->buffers(c.stream())[i].get()) < 0) {
                ERR("addBuffer failed");
                return false;
            }
        requests_.push_back(std::move(req));
    }
    LOG("%zu requests created", requests_.size());

    buildControls();
    camera_->requestCompleted.connect(this, &CameraChannel::onRequestCompleted);
    return true;
}

void CameraChannel::logControls() const
{
    // ControlInfoMap = every control this camera supports, with min/max/default.
    const ControlInfoMap &ctrls = camera_->controls();
    LOG("supported controls (%zu):", ctrls.size());
    for (const auto &[id, info] : ctrls)
        LOG("  %-28s %s", id->name().c_str(), info.toString().c_str());
}

// Only set a control if the camera advertises it; otherwise log and skip.
template<typename T, typename V>
static void setIfSupported(ControlList &list, const ControlInfoMap &info,
                           const Control<T> &ctrl, const V &value)
{
    if (info.find(&ctrl) == info.end()) {
        WARN("control %s not supported, ignored", ctrl.name().c_str());
        return;
    }
    list.set(ctrl, static_cast<T>(value));
    LOG("control %s = %s", ctrl.name().c_str(), ControlValue(static_cast<T>(value)).toString().c_str());
}

void CameraChannel::buildControls()
{
    const ControlInfoMap &info = camera_->controls();
    startControls_ = ControlList(info);

    // Frame rate: min = max frame duration pins the sensor to a fixed rate.
    // If the sensor mode cannot reach it (e.g. full-res raw), the IPA clamps it.
    int64_t frameUs = 1000000 / opts_.fps;
    if (info.find(&controls::FrameDurationLimits) != info.end())
        startControls_.set(controls::FrameDurationLimits, Span<const int64_t, 2>({ frameUs, frameUs }));

    if (opts_.aeEnable)   setIfSupported(startControls_, info, controls::AeEnable, *opts_.aeEnable);
    if (opts_.exposureUs) setIfSupported(startControls_, info, controls::ExposureTime, *opts_.exposureUs);
    if (opts_.gain)       setIfSupported(startControls_, info, controls::AnalogueGain, *opts_.gain);
    if (opts_.awbEnable)  setIfSupported(startControls_, info, controls::AwbEnable, *opts_.awbEnable);
    if (opts_.brightness) setIfSupported(startControls_, info, controls::Brightness, *opts_.brightness);
    if (opts_.contrast)   setIfSupported(startControls_, info, controls::Contrast, *opts_.contrast);

    if (opts_.awbMode) {
        static const std::map<std::string, int32_t> modes = {
            { "auto", controls::AwbAuto },           { "incandescent", controls::AwbIncandescent },
            { "tungsten", controls::AwbTungsten },   { "fluorescent", controls::AwbFluorescent },
            { "indoor", controls::AwbIndoor },       { "daylight", controls::AwbDaylight },
            { "cloudy", controls::AwbCloudy },       { "custom", controls::AwbCustom },
        };
        auto m = modes.find(*opts_.awbMode);
        auto it = info.find(&controls::AwbMode);
        if (m == modes.end()) {
            WARN("unknown AWB mode '%s'", opts_.awbMode->c_str());
        } else if (it == info.end()) {
            WARN("AwbMode not supported");
        } else {
            // For enum controls, ControlInfo::values() lists the values this camera accepts.
            const auto &vals = it->second.values();
            bool ok = vals.empty() || std::any_of(vals.begin(), vals.end(),
                [&](const ControlValue &v) { return v.get<int32_t>() == m->second; });
            if (ok)
                setIfSupported(startControls_, info, controls::AwbMode, m->second);
            else
                WARN("AWB mode '%s' not offered by this camera", opts_.awbMode->c_str());
        }
    }
}

bool CameraChannel::start()
{
    // Controls passed to start() apply from the very first frame.
    if (camera_->start(&startControls_) < 0) {
        ERR("camera start failed");
        return false;
    }
    started_ = true;
    for (auto &req : requests_)
        if (camera_->queueRequest(req.get()) < 0) {
            ERR("queueRequest failed");
            return false;
        }
    return true;
}

void CameraChannel::onRequestCompleted(Request *req)
{
    // Called from libcamera's internal thread. Keep it short: hand off to the main thread.
    events_.push({ Event::Completed, req, monotonicNs() });
}

void CameraChannel::requeue(Request *req)
{
    if (!started_)
        return;
    // ReuseBuffers keeps the same buffers attached, so no allocation per frame.
    req->reuse(Request::ReuseBuffers);
    camera_->queueRequest(req);
}

void CameraChannel::stop()
{
    if (!started_)
        return;
    started_ = false;
    // stop() blocks until every pending request has completed (status Cancelled).
    camera_->stop();
    camera_->requestCompleted.disconnect(this);
}
