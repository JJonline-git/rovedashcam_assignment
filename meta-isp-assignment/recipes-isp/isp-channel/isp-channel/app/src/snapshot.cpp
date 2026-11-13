#include "snapshot.h"
#include "log.h"

#include <png.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

SnapshotWriter::~SnapshotWriter()
{
    if (worker_.joinable())
        worker_.join();
}

bool SnapshotWriter::busy() const
{
    return running_;
}

void SnapshotWriter::submit(SnapshotJob &&job)
{
    if (worker_.joinable())
        worker_.join();             // previous job already finished (busy() was false)
    running_ = true;
    worker_ = std::thread([this, j = std::move(job)]() {
        std::string png = j.basePath + ".png";
        if (writePng(j, png))
            LOG("snapshot written: %s", png.c_str());
        if (!j.raw.empty()) {
            std::ofstream(j.basePath + ".raw", std::ios::binary)
                .write(reinterpret_cast<const char *>(j.raw.data()), j.raw.size());
            std::ofstream(j.basePath + ".txt") << j.rawInfo;
            LOG("raw written: %s.raw (%zu bytes)", j.basePath.c_str(), j.raw.size());
        }
        running_ = false;
    });
}

static inline uint8_t clamp8(float v)
{
    return uint8_t(std::min(255.0f, std::max(0.0f, v + 0.5f)));
}

bool writePng(const SnapshotJob &j, const std::string &path)
{
    // YCbCr -> RGB. Kr/Kb depend on the matrix (BT.601 vs BT.709) libcamera reports
    // in StreamConfiguration::colorSpace; range decides the 16..235 vs 0..255 scaling.
    const float kr = j.rec709 ? 0.2126f : 0.299f;
    const float kb = j.rec709 ? 0.0722f : 0.114f;
    const float kg = 1.0f - kr - kb;

    std::vector<uint8_t> row(j.width * 3);
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        ERR("cannot open %s", path.c_str());
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {          // libpng reports errors by longjmp
        png_destroy_write_struct(&png, &info);
        std::fclose(f);
        return false;
    }
    png_init_io(png, f);
    png_set_compression_level(png, 1);      // fast: we care about not stalling, not size
    png_set_IHDR(png, info, j.width, j.height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    const unsigned cw = j.width / 2;
    for (unsigned yy = 0; yy < j.height; yy++) {
        for (unsigned xx = 0; xx < j.width; xx++) {
            // 4:2:0 = one U and one V sample per 2x2 block of Y.
            float Y = j.y[yy * j.width + xx];
            float U = j.u[(yy / 2) * cw + xx / 2];
            float V = j.v[(yy / 2) * cw + xx / 2];
            if (j.fullRange) {
                Y = Y / 255.0f; U = (U - 128.0f) / 255.0f; V = (V - 128.0f) / 255.0f;
            } else {
                Y = (Y - 16.0f) / 219.0f; U = (U - 128.0f) / 224.0f; V = (V - 128.0f) / 224.0f;
            }
            float r = Y + 2.0f * (1.0f - kr) * V;
            float b = Y + 2.0f * (1.0f - kb) * U;
            float g = (Y - kr * r - kb * b) / kg;
            row[xx * 3 + 0] = clamp8(r * 255.0f);
            row[xx * 3 + 1] = clamp8(g * 255.0f);
            row[xx * 3 + 2] = clamp8(b * 255.0f);
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(f);
    return true;
}
