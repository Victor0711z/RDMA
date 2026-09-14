#include "util.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t reference_crc32c(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    while (len--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0x82f63b78u & mask);
        }
    }
    return ~crc;
}

int main(void) {
    static const char vector[] = "123456789";
    assert(dp_crc32c(vector, sizeof(vector) - 1) == 0xe3069283u);
    assert(dp_crc32c("", 0) == 0u);

    uint8_t data[8192 + 8];
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i < sizeof(data); i++) {
        state = state * 1664525u + 1013904223u;
        data[i] = (uint8_t)(state >> 24);
    }
    static const size_t lengths[] = {
        0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 31, 32, 63, 64,
        127, 255, 256, 511, 1024, 4095, 4096, 8192
    };
    for (size_t offset = 0; offset < 8; offset++) {
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
            size_t len = lengths[i];
            assert(dp_crc32c(data + offset, len) == reference_crc32c(data + offset, len));
        }
    }

    printf("crc32c tests: OK (backend=%s)\n", dp_crc32c_backend());
    return 0;
}
