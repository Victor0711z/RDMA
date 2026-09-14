#include "util.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    static const char vector[] = "123456789";
    assert(dp_crc32c(vector, sizeof(vector) - 1) == 0xe3069283u);
    assert(dp_crc32c("", 0) == 0u);
    puts("crc32c tests: OK");
    return 0;
}
