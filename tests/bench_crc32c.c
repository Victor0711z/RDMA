#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    const size_t bytes = 64u * 1024u * 1024u;
    const unsigned rounds = 16;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) return 1;
    for (size_t i = 0; i < bytes; i++) buf[i] = (uint8_t)(i * 131u + 17u);

    volatile uint32_t sink = 0;
    double start = now_sec();
    for (unsigned i = 0; i < rounds; i++) sink ^= dp_crc32c(buf, bytes);
    double elapsed = now_sec() - start;
    double gib = ((double)bytes * rounds) / (1024.0 * 1024.0 * 1024.0);

    printf("crc32c backend=%s throughput=%.2f GiB/s elapsed=%.3fs checksum=%08x\n",
           dp_crc32c_backend(), gib / elapsed, elapsed, (unsigned)sink);
    free(buf);
    return 0;
}
