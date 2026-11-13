#pragma once
// Tiny logging helpers. stdout goes to journald when run by systemd.
#include <cstdio>

#define LOG(fmt, ...)  std::printf("[isp] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...) std::printf("[isp] WARN: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  std::fprintf(stderr, "[isp] ERROR: " fmt "\n", ##__VA_ARGS__)
