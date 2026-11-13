/*
 * dmabuf.h - cache maintenance around CPU reads of a dmabuf.
 *
 * The ISP writes pixels by DMA, bypassing the CPU caches. Before the CPU reads them we
 * tell the buffer's exporter (DMA_BUF_SYNC_START) so it can invalidate stale cache lines,
 * and when done (DMA_BUF_SYNC_END). This is a dma-buf ioctl, not a V4L2 one.
 */
#ifndef DMABUF_H
#define DMABUF_H
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

static inline void dmabuf_begin_cpu_read(int fd)
{
	struct dma_buf_sync s = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
	ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);   /* best effort */
}

static inline void dmabuf_end_cpu_read(int fd)
{
	struct dma_buf_sync s = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
	ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}
#endif
