#include "mapped_buffer.h"
#include "log.h"

#include <libcamera/framebuffer.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <algorithm>

MappedBuffer::MappedBuffer(const libcamera::FrameBuffer *fb)
{
    const auto &planes = fb->planes();

    // Several planes can share one fd (on the Pi, Y/U/V are one dmabuf at different
    // offsets). Map each distinct fd once, large enough to cover all its planes.
    for (const auto &p : planes) {
        int fd = p.fd.get();
        auto it = std::find_if(maps_.begin(), maps_.end(),
                               [fd](const Mapping &m) { return m.fd == fd; });
        size_t end = p.offset + p.length;
        if (it == maps_.end())
            maps_.push_back({ fd, nullptr, end });
        else
            it->len = std::max(it->len, end);
    }

    for (auto &m : maps_) {
        m.addr = mmap(nullptr, m.len, PROT_READ, MAP_SHARED, m.fd, 0);
        if (m.addr == MAP_FAILED) {
            ERR("mmap of dmabuf fd %d failed", m.fd);
            m.addr = nullptr;
            planes_.clear();
            return;
        }
    }

    for (const auto &p : planes) {
        int fd = p.fd.get();
        auto it = std::find_if(maps_.begin(), maps_.end(),
                               [fd](const Mapping &m) { return m.fd == fd; });
        planes_.push_back(static_cast<const uint8_t *>(it->addr) + p.offset);
        lengths_.push_back(p.length);
    }
}

MappedBuffer::~MappedBuffer()
{
    for (auto &m : maps_)
        if (m.addr)
            munmap(m.addr, m.len);
}

static void dmabufSync(const std::vector<int> &fds, uint64_t flags)
{
    for (int fd : fds) {
        struct dma_buf_sync sync = { flags };
        ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);   // best effort; harmless if unsupported
    }
}

void MappedBuffer::beginCpuRead() const
{
    std::vector<int> fds;
    for (auto &m : maps_) fds.push_back(m.fd);
    dmabufSync(fds, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

void MappedBuffer::endCpuRead() const
{
    std::vector<int> fds;
    for (auto &m : maps_) fds.push_back(m.fd);
    dmabufSync(fds, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}
