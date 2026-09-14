#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>
#endif

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

typedef uint32_t (*crc32c_fn_t)(const void *, size_t);

static pthread_once_t g_crc32c_once = PTHREAD_ONCE_INIT;
static uint32_t g_crc32c_table[256];
static crc32c_fn_t g_crc32c_fn;
static const char *g_crc32c_backend = "table";

static uint32_t crc32c_table(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    while (len--) {
        crc = g_crc32c_table[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    }
    return ~crc;
}

#if (defined(__x86_64__) || defined(__i386__)) && !defined(DP_CRC32C_FORCE_TABLE)
__attribute__((target("sse4.2")))
static uint32_t crc32c_sse42(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t crc = 0xffffffffu;

#if defined(__x86_64__)
    while (len >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, p, sizeof(word));
        crc = _mm_crc32_u64(crc, word);
        p += sizeof(word);
        len -= sizeof(word);
    }
#else
    while (len >= sizeof(uint32_t)) {
        uint32_t word;
        memcpy(&word, p, sizeof(word));
        crc = _mm_crc32_u32((uint32_t)crc, word);
        p += sizeof(word);
        len -= sizeof(word);
    }
#endif

    uint32_t crc32 = (uint32_t)crc;
    while (len--) crc32 = _mm_crc32_u8(crc32, *p++);
    return ~crc32;
}
#endif

static void crc32c_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0x82f63b78u & mask);
        }
        g_crc32c_table[i] = crc;
    }
    g_crc32c_fn = crc32c_table;

#if (defined(__x86_64__) || defined(__i386__)) && !defined(DP_CRC32C_FORCE_TABLE)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2")) {
        g_crc32c_fn = crc32c_sse42;
        g_crc32c_backend = "sse4.2";
    }
#endif
}

uint32_t dp_crc32c(const void *data, size_t len) {
    pthread_once(&g_crc32c_once, crc32c_init);
    return g_crc32c_fn(data, len);
}

const char *dp_crc32c_backend(void) {
    pthread_once(&g_crc32c_once, crc32c_init);
    return g_crc32c_backend;
}
