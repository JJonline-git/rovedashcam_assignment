/* log.h - tiny logging macros. stdout/stderr go to journald when run by systemd. */
#ifndef LOG_H
#define LOG_H
#include <stdio.h>

#define LOG(fmt, ...)  printf("[isp] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...) printf("[isp] WARN: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...)  fprintf(stderr, "[isp] ERROR: " fmt "\n", ##__VA_ARGS__)

#endif
