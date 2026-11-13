#pragma once
// Measurements for the report: FPS and dropped frames per stream, CPU %, RSS memory,
// and min/avg/max timings (app processing time, sensor-to-encoded latency).
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

// Counts frames and detects drops from the per-stream sequence number that libcamera
// puts in FrameMetadata. A gap (seq jumps by more than 1) means the sensor produced a
// frame we never received, usually because no Request was queued in time.
struct StreamCounter {
    uint64_t frames = 0, drops = 0, totalFrames = 0, totalDrops = 0;
    int64_t lastSeq = -1;

    void onFrame(uint32_t seq)
    {
        if (lastSeq >= 0 && seq > lastSeq + 1) {
            drops += seq - lastSeq - 1;
            totalDrops += seq - lastSeq - 1;
        }
        lastSeq = seq;
        frames++;
        totalFrames++;
    }
    void resetWindow() { frames = drops = 0; }
};

struct TimingStat {
    double minUs = std::numeric_limits<double>::max(), maxUs = 0, sumUs = 0;
    uint64_t n = 0;

    void add(double us) { minUs = std::min(minUs, us); maxUs = std::max(maxUs, us); sumUs += us; n++; }
    std::string str() const;
    void reset() { *this = TimingStat(); }
};

// CPU % of this process (user+sys) since the previous sample, and resident memory.
class ProcSampler {
public:
    ProcSampler();
    void sample(double &cpuPercent, long &rssKb);

private:
    uint64_t lastTicks_ = 0;
    uint64_t lastWallNs_ = 0;
};

uint64_t monotonicNs();
