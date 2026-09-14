#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_config(const char *content, char path[]) {
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    close(fd);
    return written == (ssize_t)len ? 0 : -1;
}

static void test_valid_config(void) {
    static const char ini[] =
        "[rdma]\n"
        "local_ip=192.168.1.10\n"
        "remote_ip=192.168.1.11\n"
        "base_port=18801\n"
        "qp_count=4\n"
        "chunk_size=1048576\n"
        "window=32\n";
    char path[] = "/tmp/dp_config_valid_XXXXXX";
    assert(write_config(ini, path) == 0);

    dp_config_t cfg;
    assert(dp_config_load(path, &cfg) == 0);
    assert(strcmp(cfg.local_ip, "192.168.1.10") == 0);
    assert(strcmp(cfg.remote_ip, "192.168.1.11") == 0);
    assert(cfg.base_port == 18801);
    assert(cfg.qp_count == 4);
    assert(cfg.chunk_size == 1048576u);
    assert(cfg.window == 32u);
    unlink(path);
}

static void test_port_range_rejected(void) {
    static const char ini[] =
        "[rdma]\n"
        "local_ip=192.168.1.10\n"
        "remote_ip=192.168.1.11\n"
        "base_port=65535\n"
        "qp_count=2\n";
    char path[] = "/tmp/dp_config_invalid_XXXXXX";
    assert(write_config(ini, path) == 0);

    dp_config_t cfg;
    assert(dp_config_load(path, &cfg) != 0);
    unlink(path);
}

int main(void) {
    test_valid_config();
    test_port_range_rejected();
    puts("config tests: OK");
    return 0;
}
