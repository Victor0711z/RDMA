#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>

static dp_log_level_t g_log_level = DP_LOG_INFO;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(dp_log_level_t level) {
    switch (level) {
        case DP_LOG_ERROR: return "ERROR";
        case DP_LOG_WARN:  return "WARN ";
        case DP_LOG_INFO:  return "INFO ";
        case DP_LOG_DEBUG: return "DEBUG";
    }
    return "?????";
}

void dp_set_log_level(dp_log_level_t level) { g_log_level = level; }

void dp_log(dp_log_level_t level, const char *fmt, ...) {
    if (level > g_log_level) return;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_now;
    localtime_r(&tv.tv_sec, &tm_now);

    pthread_mutex_lock(&g_log_lock);
    fprintf(stderr, "[%02d:%02d:%02d.%03d][%s] ",
            tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, (int)(tv.tv_usec / 1000),
            level_name(level));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&g_log_lock);
}

double dp_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void dp_format_bytes(double bytes, char *out, size_t out_len) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int idx = 0;
    double v = bytes;
    while (v >= 1024.0 && idx < 4) {
        v /= 1024.0;
        idx++;
    }
    snprintf(out, out_len, "%.2f %s", v, units[idx]);
}

void dp_format_rate(double bytes_per_sec, char *out, size_t out_len) {
    static const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int idx = 0;
    double v = bytes_per_sec;
    while (v >= 1000.0 && idx < 3) {
        v /= 1000.0;
        idx++;
    }
    snprintf(out, out_len, "%.2f %s", v, units[idx]);
}

int dp_open_output_file(const char *path, uint64_t size) {
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

uint32_t dp_calc_total_chunks(uint64_t file_size, uint32_t chunk_size) {
    if (chunk_size == 0) return 0;
    return (uint32_t)((file_size + chunk_size - 1) / chunk_size);
}
