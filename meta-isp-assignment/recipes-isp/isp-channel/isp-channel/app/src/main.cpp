// isp-channel: libcamera capture -> Raspberry Pi hardware ISP -> H.264 (hardware) -> mp4 / RTP.
//
// Threads:
//   libcamera thread  : signals completed Requests          -> EventQueue
//   GStreamer threads : signal "encoder finished a buffer"   -> EventQueue
//   main thread       : the only thread that calls libcamera (queueRequest) and appsrc.
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstring>
#include <memory>
#include <string>

#include <gst/gst.h>
#include <libcamera/libcamera.h>

#include "camera_channel.h"
#include "encoder.h"
#include "log.h"
#include "snapshot.h"
#include "stats.h"

using namespace libcamera;

static volatile sig_atomic_t gStop = 0;
static volatile sig_atomic_t gSnapshot = 0;

static void onSignal(int sig)
{
    // Only set flags here: almost nothing else is async-signal-safe.
    if (sig == SIGUSR1)
        gSnapshot = 1;
    else
        gStop = 1;
}

struct AppOptions {
    ChannelOptions cam;
    EncoderOptions enc;
    unsigned durationSec = 0;          // 0 = run until stopped
    unsigned statsSec = 5;
    std::string triggerFile = "/run/isp-channel/snapshot";
    bool listControls = false;
};

static void usage()
{
    std::printf(
        "isp-channel [options]\n"
        "  --camera ID            camera id (default: first)\n"
        "  --width W --height H   viewfinder size (default 1920x1080)\n"
        "  --fps N                target frame rate (default 30)\n"
        "  --raw full|video|off   raw stream: largest sensor mode, mode matching viewfinder, or none\n"
        "  --buffers N            buffers per stream (default 6)\n"
        "  --ae on|off --awb on|off\n"
        "  --exposure US --gain G --brightness B (-1..1) --contrast C (0..)\n"
        "  --awb-mode auto|incandescent|tungsten|fluorescent|indoor|daylight|cloudy|custom\n"
        "  --bitrate BPS          H.264 bitrate (default 8000000)\n"
        "  --outdir DIR           mp4 segments + snapshots (default .)\n"
        "  --segment SEC          mp4 segment length (default 60)\n"
        "  --udp HOST:PORT        also stream RTP/H.264 over UDP\n"
        "  --no-zero-copy         let the encoder copy frames instead of importing the dmabuf\n"
        "  --duration SEC         stop after SEC seconds (default: run forever)\n"
        "  --stats SEC            stats interval (default 5)\n"
        "  --trigger-file PATH    snapshot when this file appears (default /run/isp-channel/snapshot)\n"
        "  --list-controls        print the camera's ControlInfoMap and exit\n"
        "Snapshot: kill -USR1 <pid>, or touch the trigger file.\n");
}

static bool onOff(const char *s) { return !std::strcmp(s, "on") || !std::strcmp(s, "1"); }

static bool parseArgs(int argc, char **argv, AppOptions &o)
{
    enum { CAMERA = 256, WIDTH, HEIGHT, FPS, RAW, BUFFERS, AE, AWB, EXPOSURE, GAIN, BRIGHTNESS,
           CONTRAST, AWBMODE, BITRATE, OUTDIR, SEGMENT, UDP, NOZC, DURATION, STATS, TRIGGER, LIST };
    static const option longOpts[] = {
        { "camera", required_argument, nullptr, CAMERA },   { "width", required_argument, nullptr, WIDTH },
        { "height", required_argument, nullptr, HEIGHT },   { "fps", required_argument, nullptr, FPS },
        { "raw", required_argument, nullptr, RAW },         { "buffers", required_argument, nullptr, BUFFERS },
        { "ae", required_argument, nullptr, AE },           { "awb", required_argument, nullptr, AWB },
        { "exposure", required_argument, nullptr, EXPOSURE }, { "gain", required_argument, nullptr, GAIN },
        { "brightness", required_argument, nullptr, BRIGHTNESS }, { "contrast", required_argument, nullptr, CONTRAST },
        { "awb-mode", required_argument, nullptr, AWBMODE }, { "bitrate", required_argument, nullptr, BITRATE },
        { "outdir", required_argument, nullptr, OUTDIR },   { "segment", required_argument, nullptr, SEGMENT },
        { "udp", required_argument, nullptr, UDP },         { "no-zero-copy", no_argument, nullptr, NOZC },
        { "duration", required_argument, nullptr, DURATION }, { "stats", required_argument, nullptr, STATS },
        { "trigger-file", required_argument, nullptr, TRIGGER }, { "list-controls", no_argument, nullptr, LIST },
        { "help", no_argument, nullptr, 'h' },              { nullptr, 0, nullptr, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", longOpts, nullptr)) != -1) {
        switch (c) {
        case CAMERA: o.cam.cameraId = optarg; break;
        case WIDTH: o.cam.width = std::stoul(optarg); break;
        case HEIGHT: o.cam.height = std::stoul(optarg); break;
        case FPS: o.cam.fps = std::stoul(optarg); break;
        case RAW:
            if (!std::strcmp(optarg, "full")) o.cam.raw = RawMode::Full;
            else if (!std::strcmp(optarg, "video")) o.cam.raw = RawMode::Video;
            else if (!std::strcmp(optarg, "off")) o.cam.raw = RawMode::Off;
            else { ERR("--raw must be full|video|off"); return false; }
            break;
        case BUFFERS: o.cam.bufferCount = std::stoul(optarg); break;
        case AE: o.cam.aeEnable = onOff(optarg); break;
        case AWB: o.cam.awbEnable = onOff(optarg); break;
        case EXPOSURE: o.cam.exposureUs = std::stoi(optarg); break;
        case GAIN: o.cam.gain = std::stof(optarg); break;
        case BRIGHTNESS: o.cam.brightness = std::stof(optarg); break;
        case CONTRAST: o.cam.contrast = std::stof(optarg); break;
        case AWBMODE: o.cam.awbMode = optarg; break;
        case BITRATE: o.enc.bitrate = std::stoul(optarg); break;
        case OUTDIR: o.enc.outdir = optarg; break;
        case SEGMENT: o.enc.segmentSec = std::stoul(optarg); break;
        case UDP: {
            std::string s = optarg;
            auto colon = s.rfind(':');
            if (colon == std::string::npos) { ERR("--udp needs HOST:PORT"); return false; }
            o.enc.udpHost = s.substr(0, colon);
            o.enc.udpPort = std::stoi(s.substr(colon + 1));
            break;
        }
        case NOZC: o.enc.zeroCopy = false; break;
        case DURATION: o.durationSec = std::stoul(optarg); break;
        case STATS: o.statsSec = std::stoul(optarg); break;
        case TRIGGER: o.triggerFile = optarg; break;
        case LIST: o.listControls = true; break;
        default: usage(); return false;
        }
    }
    o.enc.width = o.cam.width;
    o.enc.height = o.cam.height;
    o.enc.fps = o.cam.fps;
    return true;
}

// Copy one frame out of the camera buffers for the snapshot thread.
static SnapshotJob makeSnapshot(const CameraChannel &ch, Request *req, unsigned index,
                                const std::string &outdir)
{
    SnapshotJob job;
    char name[64];
    std::snprintf(name, sizeof(name), "/snap-%06u", index);
    job.basePath = outdir + name;

    const StreamConfiguration &vc = ch.viewfinder()->configuration();
    const FrameBuffer *vfb = req->findBuffer(ch.viewfinder());
    const MappedBuffer *m = ch.mapped(vfb);
    job.width = vc.size.width;
    job.height = vc.size.height;
    if (vc.colorSpace) {
        job.rec709 = vc.colorSpace->ycbcrEncoding == ColorSpace::YcbcrEncoding::Rec709;
        job.fullRange = vc.colorSpace->range == ColorSpace::Range::Full;
    }
    // COPY #1 (snapshot only): strip stride padding, Y then U then V.
    m->beginCpuRead();
    auto copyPlane = [](const uint8_t *src, unsigned stride, unsigned w, unsigned h,
                        std::vector<uint8_t> &dst) {
        dst.resize(size_t(w) * h);
        for (unsigned r = 0; r < h; r++)
            std::memcpy(&dst[size_t(r) * w], src + size_t(r) * stride, w);
    };
    copyPlane(m->plane(0), vc.stride, job.width, job.height, job.y);
    copyPlane(m->plane(1), vc.stride / 2, job.width / 2, job.height / 2, job.u);
    copyPlane(m->plane(2), vc.stride / 2, job.width / 2, job.height / 2, job.v);
    m->endCpuRead();

    if (ch.raw()) {
        const StreamConfiguration &rc = ch.raw()->configuration();
        const FrameBuffer *rfb = req->findBuffer(ch.raw());
        const MappedBuffer *rm = ch.mapped(rfb);
        // COPY #2 (snapshot only): raw Bayer kept bit-exact, packing and stride included.
        rm->beginCpuRead();
        job.raw.assign(rm->plane(0), rm->plane(0) + rm->planeLength(0));
        rm->endCpuRead();
        const ControlList &md = req->metadata();
        char info[256];
        std::snprintf(info, sizeof(info),
                      "format=%s\nwidth=%u\nheight=%u\nstride=%u\nsequence=%u\nexposure_us=%d\nanalogue_gain=%.3f\n",
                      rc.pixelFormat.toString().c_str(), rc.size.width, rc.size.height, rc.stride,
                      rfb->metadata().sequence, md.get(controls::ExposureTime).value_or(-1),
                      md.get(controls::AnalogueGain).value_or(-1.0f));
        job.rawInfo = info;
    }
    return job;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);   // line-buffered so journald gets lines immediately

    AppOptions opts;
    if (!parseArgs(argc, argv, opts))
        return 1;

    gst_init(&argc, &argv);

    struct sigaction sa = {};
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);

    // The CameraManager enumerates cameras when started. At boot the sensor/ISP modules may
    // still be loading, so retry for a few seconds before giving up (systemd also restarts us).
    auto cm = std::make_unique<CameraManager>();
    if (cm->start()) {
        ERR("CameraManager start failed");
        return 1;
    }
    for (int i = 0; i < 20 && cm->cameras().empty() && !gStop; i++) {
        cm->stop();
        usleep(500000);
        cm->start();
    }

    EventQueue events;
    // Scope: destruction order matters. Encoder (holds dmabuf references) must be
    // destroyed before the CameraChannel frees the buffers.
    int rc = 0;
    {
        auto channel = std::make_unique<CameraChannel>(events);
        if (!channel->open(*cm, opts.cam, opts.listControls))
            return 1;
        if (opts.listControls)
            return 0;

        auto encoder = std::make_unique<Encoder>();
        bool ok = encoder->start(opts.enc, [&events](void *cookie) {
            // Runs in a GStreamer thread (or main thread on drop): just queue it.
            events.push({ Event::Released, static_cast<Request *>(cookie), monotonicNs() });
        });
        if (!ok || !channel->start())
            return 1;

        LOG("running: %s %ux%u@%u raw=%s zero-copy=%s",
            channel->cameraModel().c_str(), opts.cam.width, opts.cam.height, opts.cam.fps,
            opts.cam.raw == RawMode::Full ? "full" : opts.cam.raw == RawMode::Video ? "video" : "off",
            opts.enc.zeroCopy ? "yes" : "no");

        SnapshotWriter snapshots;
        StreamCounter vfCount, rawCount;
        TimingStat procTime, sensorToApp;
        ProcSampler proc;
        uint64_t encodeRejects = 0, snapIndex = 0;
        long rssBaseline = 0, rssKb = 0;
        double cpu = 0;
        const uint64_t t0 = monotonicNs();
        uint64_t lastStats = t0;
        // AE/AWB results of the latest frame. Copied at completion time: once a Request is
        // requeued, reuse() clears its metadata and libcamera owns it again.
        struct { int32_t exp = -1; float gain = -1, cgR = -1, cgB = -1, lux = -1; int32_t k = -1; } meta;
        bool stopping = false;

        while (!stopping) {
            Event ev;
            if (events.pop(ev, 100)) {
                Request *req = ev.request;
                if (ev.type == Event::Released) {
                    channel->requeue(req);                 // request recycling
                } else if (req->status() == Request::RequestCancelled) {
                    // Only happens during stop(); nothing to do.
                } else {
                    FrameBuffer *vfb = req->findBuffer(channel->viewfinder());
                    const FrameMetadata &vmd = vfb->metadata();
                    vfCount.onFrame(vmd.sequence);
                    if (channel->raw())
                        rawCount.onFrame(req->findBuffer(channel->raw())->metadata().sequence);
                    sensorToApp.add(double(ev.whenNs - vmd.timestamp) / 1000.0);
                    const ControlList &md = req->metadata();
                    meta.exp = md.get(controls::ExposureTime).value_or(-1);
                    meta.gain = md.get(controls::AnalogueGain).value_or(-1.0f);
                    if (auto cg = md.get(controls::ColourGains)) { meta.cgR = (*cg)[0]; meta.cgB = (*cg)[1]; }
                    meta.k = md.get(controls::ColourTemperature).value_or(-1);
                    meta.lux = md.get(controls::Lux).value_or(-1.0f);

                    if (vmd.status != FrameMetadata::FrameSuccess) {
                        channel->requeue(req);
                    } else {
                        if (gSnapshot || access(opts.triggerFile.c_str(), F_OK) == 0) {
                            gSnapshot = 0;
                            unlink(opts.triggerFile.c_str());
                            if (snapshots.busy())
                                WARN("snapshot still being written, trigger ignored");
                            else
                                snapshots.submit(makeSnapshot(*channel, req, snapIndex++, opts.enc.outdir));
                        }

                        // Hand the viewfinder buffer to the encoder. The Request is requeued
                        // only when GStreamer releases the buffer (Released event).
                        const auto &planes = vfb->planes();
                        const StreamConfiguration &vc = channel->viewfinder()->configuration();
                        const MappedBuffer *m = channel->mapped(vfb);
                        EncoderFrame f = {};
                        f.fd = planes[0].fd.get();
                        f.cpu = m->plane(0) - planes[0].offset;   // start of the dmabuf mapping
                        for (unsigned i = 0; i < 3 && i < planes.size(); i++) {
                            f.offset[i] = planes[i].offset;
                            f.size = std::max<size_t>(f.size, planes[i].offset + planes[i].length);
                        }
                        f.stride[0] = vc.stride;
                        f.stride[1] = f.stride[2] = vc.stride / 2;
                        f.sensorTsNs = vmd.timestamp;
                        f.cookie = req;
                        if (!encoder->push(f))
                            encodeRejects++;
                        procTime.add(double(monotonicNs() - ev.whenNs) / 1000.0);
                    }
                }
            }

            if (!encoder->checkBus()) {
                rc = 2;
                break;
            }

            uint64_t now = monotonicNs();
            if (now - lastStats >= uint64_t(opts.statsSec) * 1000000000ull) {
                double sec = double(now - lastStats) / 1e9;
                proc.sample(cpu, rssKb);
                if (!rssBaseline)
                    rssBaseline = rssKb;   // first window = warmed-up baseline
                TimingStat lat = encoder->takeLatency();
                LOG("[stats] vf %.2f fps (drops %llu) | raw %.2f fps (drops %llu) | cpu %.1f%% | rss %ld kB | "
                    "enc rejects %llu",
                    vfCount.frames / sec, (unsigned long long)vfCount.drops,
                    rawCount.frames / sec, (unsigned long long)rawCount.drops, cpu, rssKb,
                    (unsigned long long)encodeRejects);
                LOG("[stats] app processing %s | sensor->app %s | sensor->encoded %s",
                    procTime.str().c_str(), sensorToApp.str().c_str(), lat.str().c_str());
                LOG("[meta] exposure %d us | gain %.2f | colour gains R %.2f B %.2f | %d K | lux %.0f",
                    meta.exp, meta.gain, meta.cgR, meta.cgB, meta.k, meta.lux);
                vfCount.resetWindow();
                rawCount.resetWindow();
                procTime.reset();
                sensorToApp.reset();
                lastStats = now;
            }

            if (gStop || (opts.durationSec && now - t0 >= uint64_t(opts.durationSec) * 1000000000ull))
                stopping = true;
        }

        // Shutdown order:
        //  1. camera stop: no new frames; pending requests come back Cancelled.
        //  2. encoder stop: EOS finalises the mp4; NULL state releases every dmabuf it held.
        //  3. channel destroyed: munmap + free buffers (safe: nobody references them now).
        LOG("stopping");
        channel->stop();
        encoder.reset();
        proc.sample(cpu, rssKb);
        double total = double(monotonicNs() - t0) / 1e9;
        LOG("[summary] %.0f s | vf frames %llu drops %llu | raw frames %llu drops %llu | "
            "rss baseline %ld kB end %ld kB (growth %ld kB)",
            total, (unsigned long long)vfCount.totalFrames, (unsigned long long)vfCount.totalDrops,
            (unsigned long long)rawCount.totalFrames, (unsigned long long)rawCount.totalDrops,
            rssBaseline, rssKb, rssKb - rssBaseline);
    }
    cm->stop();
    return rc;
}
