#include "stats.h"

#include <cstdio>
#include <ctime>
#include <unistd.h>

uint64_t monotonicNs()
{
    // CLOCK_MONOTONIC is the same clock libcamera uses for FrameMetadata::timestamp,
    // so we can subtract the two to get latency.
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

std::string TimingStat::str() const
{
    if (!n)
        return "n/a";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "min %.0f / avg %.0f / max %.0f us",
                  minUs, sumUs / n, maxUs);
    return buf;
}

static uint64_t readCpuTicks()
{
    // /proc/self/stat fields 14 and 15 are utime and stime in clock ticks.
    FILE *f = std::fopen("/proc/self/stat", "r");
    if (!f)
        return 0;
    unsigned long utime = 0, stime = 0;
    // Skip "pid (comm) state" and 10 more fields, then read utime and stime.
    int r = std::fscanf(f, "%*d (%*[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                        &utime, &stime);
    std::fclose(f);
    return r == 2 ? utime + stime : 0;
}

ProcSampler::ProcSampler()
    : lastTicks_(readCpuTicks()), lastWallNs_(monotonicNs())
{
}

void ProcSampler::sample(double &cpuPercent, long &rssKb)
{
    uint64_t ticks = readCpuTicks();
    uint64_t now = monotonicNs();
    double cpuSec = double(ticks - lastTicks_) / sysconf(_SC_CLK_TCK);
    double wallSec = double(now - lastWallNs_) / 1e9;
    // 100% = one full core. On a 4-core Pi the maximum is 400%.
    cpuPercent = wallSec > 0 ? 100.0 * cpuSec / wallSec : 0;
    lastTicks_ = ticks;
    lastWallNs_ = now;

    // /proc/self/statm field 2 = resident pages.
    rssKb = 0;
    if (FILE *f = std::fopen("/proc/self/statm", "r")) {
        long size = 0, resident = 0;
        if (std::fscanf(f, "%ld %ld", &size, &resident) == 2)
            rssKb = resident * (sysconf(_SC_PAGESIZE) / 1024);
        std::fclose(f);
    }
}
