#pragma once
// Snapshot on trigger: a PNG of the ISP output (YUV420 -> RGB) and the untouched Bayer
// frame from the Raw stream (.raw + a .txt describing its layout).
//
// The frame is COPIED out of the camera buffer first (a few ms), then converted and written
// in a background thread, so the camera Request goes back to the camera quickly and the
// video does not drop frames while the PNG is compressed (~100+ ms).
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

struct SnapshotJob {
    std::string basePath;          // e.g. /var/lib/isp-channel/snap-000042
    // Viewfinder YUV420, tightly packed after copy (stride removed).
    unsigned width = 0, height = 0;
    std::vector<uint8_t> y, u, v;
    bool fullRange = false, rec709 = false;
    // Raw Bayer, copied as-is (still CSI-2 packed, with stride).
    std::vector<uint8_t> raw;
    std::string rawInfo;           // format, size, stride, exposure, gain...
};

class SnapshotWriter {
public:
    ~SnapshotWriter();
    bool busy() const;
    void submit(SnapshotJob &&job);

private:
    std::thread worker_;
    std::atomic<bool> running_{ false };
};

// Exposed for testing.
bool writePng(const SnapshotJob &job, const std::string &path);
