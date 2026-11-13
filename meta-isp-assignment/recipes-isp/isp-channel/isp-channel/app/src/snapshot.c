#include "snapshot.h"
#include "dmabuf.h"
#include "log.h"

#include <png.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static pthread_t worker;
static atomic_int running;            /* atomic: written by worker, read by main thread */
static int worker_started;

static void copy_plane(const uint8_t *src, unsigned stride, unsigned w, unsigned h, uint8_t *dst)
{
	for (unsigned r = 0; r < h; r++)
		memcpy(dst + (size_t)r * w, src + (size_t)r * stride, w);
}

struct snapshot_job *snapshot_copy(const struct cam *c, const struct cam_frame *f,
				   const char *outdir, unsigned index)
{
	struct cam_stream_info vi;
	struct snapshot_job *j = calloc(1, sizeof(*j));
	cam_stream_info(c, 0, &vi);
	snprintf(j->base_path, sizeof(j->base_path), "%s/snap-%06u", outdir, index);
	j->width = vi.width;
	j->height = vi.height;
	j->rec709 = vi.rec709;
	j->full_range = vi.full_range;

	/* COPY (snapshot only): Y full size, U and V quarter size (4:2:0). */
	j->y = malloc((size_t)vi.width * vi.height);
	j->u = malloc((size_t)vi.width * vi.height / 4);
	j->v = malloc((size_t)vi.width * vi.height / 4);
	dmabuf_begin_cpu_read(f->vf.plane[0].fd);
	copy_plane(f->vf.plane[0].cpu, vi.stride, vi.width, vi.height, j->y);
	copy_plane(f->vf.plane[1].cpu, vi.stride / 2, vi.width / 2, vi.height / 2, j->u);
	copy_plane(f->vf.plane[2].cpu, vi.stride / 2, vi.width / 2, vi.height / 2, j->v);
	dmabuf_end_cpu_read(f->vf.plane[0].fd);

	if (f->has_raw) {
		struct cam_stream_info ri;
		cam_stream_info(c, 1, &ri);
		/* COPY (snapshot only): raw kept bit-exact, packing and stride included. */
		j->raw_len = f->raw.plane[0].length;
		j->raw = malloc(j->raw_len);
		dmabuf_begin_cpu_read(f->raw.plane[0].fd);
		memcpy(j->raw, f->raw.plane[0].cpu, j->raw_len);
		dmabuf_end_cpu_read(f->raw.plane[0].fd);
		snprintf(j->raw_info, sizeof(j->raw_info),
			 "format=%s\nwidth=%u\nheight=%u\nstride=%u\nsequence=%u\n"
			 "exposure_us=%d\nanalogue_gain=%.3f\n",
			 ri.pixel_format, ri.width, ri.height, ri.stride, f->raw.sequence,
			 f->exposure_us, f->analogue_gain);
	}
	return j;
}

static uint8_t clamp8(float v)
{
	v = v * 255.0f + 0.5f;
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static int write_png(const struct snapshot_job *j, const char *path)
{
	/* YCbCr -> RGB. Kr/Kb come from the matrix libcamera reports (BT.601 or BT.709);
	 * the range decides 16..235 (limited) or 0..255 (full) scaling. */
	const float kr = j->rec709 ? 0.2126f : 0.299f;
	const float kb = j->rec709 ? 0.0722f : 0.114f;
	const float kg = 1.0f - kr - kb;
	const unsigned cw = j->width / 2;

	FILE *fp = fopen(path, "wb");
	if (!fp)
		return -1;
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);
	uint8_t *row = malloc((size_t)j->width * 3);
	if (setjmp(png_jmpbuf(png))) {        /* libpng reports errors with longjmp */
		png_destroy_write_struct(&png, &info);
		free(row);
		fclose(fp);
		return -1;
	}
	png_init_io(png, fp);
	png_set_compression_level(png, 1);    /* fast; size does not matter here */
	png_set_IHDR(png, info, j->width, j->height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);

	for (unsigned yy = 0; yy < j->height; yy++) {
		for (unsigned xx = 0; xx < j->width; xx++) {
			/* 4:2:0: one U and one V sample per 2x2 block of Y samples. */
			float Y = j->y[yy * j->width + xx];
			float U = j->u[(yy / 2) * cw + xx / 2];
			float V = j->v[(yy / 2) * cw + xx / 2];
			if (j->full_range) {
				Y /= 255.0f; U = (U - 128.0f) / 255.0f; V = (V - 128.0f) / 255.0f;
			} else {
				Y = (Y - 16.0f) / 219.0f; U = (U - 128.0f) / 224.0f; V = (V - 128.0f) / 224.0f;
			}
			float r = Y + 2.0f * (1.0f - kr) * V;
			float b = Y + 2.0f * (1.0f - kb) * U;
			float g = (Y - kr * r - kb * b) / kg;
			row[xx * 3 + 0] = clamp8(r);
			row[xx * 3 + 1] = clamp8(g);
			row[xx * 3 + 2] = clamp8(b);
		}
		png_write_row(png, row);
	}
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	free(row);
	fclose(fp);
	return 0;
}

static void *worker_main(void *arg)
{
	struct snapshot_job *j = arg;
	char path[300];

	snprintf(path, sizeof(path), "%s.png", j->base_path);
	if (write_png(j, path) == 0)
		LOG("snapshot written: %s", path);
	else
		ERR("cannot write %s", path);

	if (j->raw) {
		snprintf(path, sizeof(path), "%s.raw", j->base_path);
		FILE *f = fopen(path, "wb");
		if (f) {
			fwrite(j->raw, 1, j->raw_len, f);
			fclose(f);
		}
		snprintf(path, sizeof(path), "%s.txt", j->base_path);
		f = fopen(path, "w");
		if (f) {
			fputs(j->raw_info, f);
			fclose(f);
		}
		LOG("raw written: %s.raw (%zu bytes)", j->base_path, j->raw_len);
	}
	free(j->y); free(j->u); free(j->v); free(j->raw); free(j);
	atomic_store(&running, 0);
	return NULL;
}

int snapshot_busy(void)
{
	return atomic_load(&running);
}

void snapshot_submit(struct snapshot_job *job)
{
	snapshot_wait();                      /* reap the previous (finished) worker */
	atomic_store(&running, 1);
	if (pthread_create(&worker, NULL, worker_main, job) == 0) {
		worker_started = 1;
	} else {
		WARN("pthread_create failed, writing snapshot synchronously");
		worker_main(job);                 /* also frees the job and clears "running" */
	}
}

void snapshot_wait(void)
{
	if (worker_started) {
		pthread_join(worker, NULL);
		worker_started = 0;
	}
}
