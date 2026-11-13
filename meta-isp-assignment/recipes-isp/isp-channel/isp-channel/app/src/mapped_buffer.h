#pragma once
// CPU access to a libcamera FrameBuffer.
//
// A FrameBuffer is a set of planes; each plane is (dmabuf fd, offset, length).
// The memory belongs to the kernel driver (Unicam / ISP). To read pixels on the CPU we
// mmap() the dmabuf fd. We do it ONCE at startup for every buffer, not per frame,
// because mmap/munmap per frame costs syscalls and TLB flushes.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace libcamera { class FrameBuffer; }

class MappedBuffer {
public:
    explicit MappedBuffer(const libcamera::FrameBuffer *fb);
    ~MappedBuffer();
    MappedBuffer(const MappedBuffer &) = delete;
    MappedBuffer &operator=(const MappedBuffer &) = delete;

    bool valid() const { return !planes_.empty(); }
    const uint8_t *plane(unsigned i) const { return planes_[i]; }
    size_t planeLength(unsigned i) const { return lengths_[i]; }

    // Bracket CPU reads with these. DMA_BUF_IOCTL_SYNC lets the exporter do cache
    // maintenance so the CPU does not read stale cache lines after the ISP wrote via DMA.
    // (This is a dma-buf ioctl, not a V4L2 one.)
    void beginCpuRead() const;
    void endCpuRead() const;

private:
    struct Mapping { int fd; void *addr; size_t len; };
    std::vector<Mapping> maps_;
    std::vector<const uint8_t *> planes_;
    std::vector<size_t> lengths_;
};
