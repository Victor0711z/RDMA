/*
 * tcp_path.c - path.h 接口的 TCP 实现
 *
 * 用途有两个：
 *   1) 作为没有 RDMA 硬件时的“软件模拟双网卡”后端，用两个不同的本地 IP
 *      （或者本机两个不同端口，模拟两张网卡）分别建立 TCP 连接，
 *      从而在普通开发机 / CI 上完整跑通调度、故障转移、限速等业务逻辑；
 *   2) 作为生产环境里 RDMA 不可用时的自动降级（graceful degradation）
 *      传输后端。
 *
 * 每个分片在线路上是 [dp_chunk_hdr_t][payload]，每个 ACK 是 [dp_ack_hdr_t]，
 * 都通过 dp_*_hton/ntoh 转换为网络字节序。
 */
#include "path.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef struct {
    int listen_fd; /* 仅 server 在 accept 完成前使用，之后置 -1 */
    int fd;
    int path_id;
} tcp_path_impl_t;

static ssize_t full_write(int fd, const void *buf, size_t len) {
    size_t off = 0;
    const char *p = (const char *)buf;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return (ssize_t)off;
}

static ssize_t full_read(int fd, void *buf, size_t len) {
    size_t off = 0;
    char *p = (char *)buf;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0; /* 对端正常关闭 */
        off += (size_t)n;
    }
    return (ssize_t)off;
}

static void set_common_sockopts(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

static dp_path_t *wrap(dp_transport_t type, int id, tcp_path_impl_t *impl) {
    dp_path_t *p = (dp_path_t *)calloc(1, sizeof(dp_path_t));
    p->type = type;
    p->id = id;
    p->ops = dp_tcp_get_ops();
    p->impl = impl;
    return p;
}

static dp_path_t *tcp_client_connect(const dp_path_cfg_t *cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        DP_LOGE("tcp: socket() failed: %s", strerror(errno));
        return NULL;
    }
    set_common_sockopts(fd);

    if (cfg->local_ip && cfg->local_ip[0]) {
        struct sockaddr_in local = {0};
        local.sin_family = AF_INET;
        local.sin_port = 0; /* 让内核挑一个空闲端口，只固定 IP（即固定网卡） */
        if (inet_pton(AF_INET, cfg->local_ip, &local.sin_addr) != 1) {
            DP_LOGE("tcp: invalid local_ip '%s'", cfg->local_ip);
            close(fd);
            return NULL;
        }
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
            DP_LOGE("tcp: bind(%s) failed: %s", cfg->local_ip, strerror(errno));
            close(fd);
            return NULL;
        }
    }

    struct sockaddr_in remote = {0};
    remote.sin_family = AF_INET;
    remote.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->remote_ip, &remote.sin_addr) != 1) {
        DP_LOGE("tcp: invalid remote_ip '%s'", cfg->remote_ip);
        close(fd);
        return NULL;
    }

    /* server 可能还没起来，做几次短暂重试，方便脚本一次性启动 client+server */
    int attempts = 20;
    int rc = -1;
    while (attempts-- > 0) {
        rc = connect(fd, (struct sockaddr *)&remote, sizeof(remote));
        if (rc == 0) break;
        if (errno != ECONNREFUSED && errno != ENETUNREACH) break;
        usleep(150 * 1000);
    }
    if (rc != 0) {
        DP_LOGE("tcp: connect(%s:%d) failed: %s", cfg->remote_ip, cfg->port, strerror(errno));
        close(fd);
        return NULL;
    }

    tcp_path_impl_t *impl = (tcp_path_impl_t *)calloc(1, sizeof(*impl));
    impl->fd = fd;
    impl->listen_fd = -1;
    DP_LOGI("tcp path connected: local=%s -> remote=%s:%d",
            cfg->local_ip ? cfg->local_ip : "(any)", cfg->remote_ip, cfg->port);
    return wrap(DP_TRANSPORT_TCP, 0, impl);
}

static dp_path_t *tcp_server_accept_one(const dp_path_cfg_t *cfg) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        DP_LOGE("tcp: socket() failed: %s", strerror(errno));
        return NULL;
    }
    set_common_sockopts(lfd);

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons((uint16_t)cfg->port);
    const char *bind_ip = (cfg->local_ip && cfg->local_ip[0]) ? cfg->local_ip : "0.0.0.0";
    if (inet_pton(AF_INET, bind_ip, &local.sin_addr) != 1) {
        DP_LOGE("tcp: invalid bind ip '%s'", bind_ip);
        close(lfd);
        return NULL;
    }
    if (bind(lfd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        DP_LOGE("tcp: bind(%s:%d) failed: %s", bind_ip, cfg->port, strerror(errno));
        close(lfd);
        return NULL;
    }
    if (listen(lfd, 1) != 0) {
        DP_LOGE("tcp: listen() failed: %s", strerror(errno));
        close(lfd);
        return NULL;
    }

    DP_LOGI("tcp path listening on %s:%d ...", bind_ip, cfg->port);
    struct sockaddr_in peer = {0};
    socklen_t plen = sizeof(peer);
    int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
    close(lfd);
    if (fd < 0) {
        DP_LOGE("tcp: accept() failed: %s", strerror(errno));
        return NULL;
    }
    set_common_sockopts(fd);

    char peer_str[64];
    inet_ntop(AF_INET, &peer.sin_addr, peer_str, sizeof(peer_str));
    DP_LOGI("tcp path accepted connection from %s:%d", peer_str, ntohs(peer.sin_port));

    tcp_path_impl_t *impl = (tcp_path_impl_t *)calloc(1, sizeof(*impl));
    impl->fd = fd;
    impl->listen_fd = -1;
    return wrap(DP_TRANSPORT_TCP, 0, impl);
}

static int tcp_send_hello(dp_path_t *p, const dp_hello_t *hello_in) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    dp_hello_t h = *hello_in;
    dp_hello_hton(&h);
    return full_write(impl->fd, &h, sizeof(h)) == (ssize_t)sizeof(h) ? 0 : -1;
}

static int tcp_recv_hello(dp_path_t *p, dp_hello_t *hello) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    ssize_t n = full_read(impl->fd, hello, sizeof(*hello));
    if (n != (ssize_t)sizeof(*hello)) return -1;
    dp_hello_ntoh(hello);
    if (hello->magic != DP_MAGIC || hello->msg_type != DP_MSG_HELLO) return -1;
    return 0;
}

static int tcp_send_hello_ack(dp_path_t *p, const dp_hello_ack_t *ack_in) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    dp_hello_ack_t a = *ack_in;
    dp_hello_ack_hton(&a);
    return full_write(impl->fd, &a, sizeof(a)) == (ssize_t)sizeof(a) ? 0 : -1;
}

static int tcp_recv_hello_ack(dp_path_t *p, dp_hello_ack_t *ack) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    ssize_t n = full_read(impl->fd, ack, sizeof(*ack));
    if (n != (ssize_t)sizeof(*ack)) return -1;
    dp_hello_ack_ntoh(ack);
    if (ack->magic != DP_MAGIC || ack->msg_type != DP_MSG_HELLO_ACK) return -1;
    return 0;
}

static int tcp_send_chunk(dp_path_t *p, uint32_t chunk_id, const void *data, uint32_t len) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    dp_chunk_hdr_t hdr = {
        .magic = DP_MAGIC, .msg_type = DP_MSG_CHUNK, .chunk_id = chunk_id, .length = len
    };
    dp_chunk_hdr_hton(&hdr);
    if (full_write(impl->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) return -1;
    if (len > 0 && full_write(impl->fd, data, len) != (ssize_t)len) return -1;
    return 0;
}

static int tcp_recv_chunk(dp_path_t *p, uint32_t *chunk_id, void *buf, uint32_t buf_cap, uint32_t *out_len) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    dp_chunk_hdr_t hdr;
    ssize_t n = full_read(impl->fd, &hdr, sizeof(hdr));
    if (n == 0) return 1;   /* 对端关闭 = 正常结束（应先收到 DONE，这里作为兜底） */
    if (n != (ssize_t)sizeof(hdr)) return -1;
    dp_chunk_hdr_ntoh(&hdr);
    if (hdr.magic != DP_MAGIC) return -1;
    if (hdr.msg_type == DP_MSG_DONE) return 1;
    if (hdr.msg_type != DP_MSG_CHUNK) return -1;
    if (hdr.length > buf_cap) {
        DP_LOGE("tcp: incoming chunk %u length %u exceeds buffer %u", hdr.chunk_id, hdr.length, buf_cap);
        return -1;
    }
    if (hdr.length > 0 && full_read(impl->fd, buf, hdr.length) != (ssize_t)hdr.length) return -1;
    *chunk_id = hdr.chunk_id;
    *out_len = hdr.length;
    return 0;
}

static int tcp_send_ack(dp_path_t *p, uint32_t chunk_id) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    dp_ack_hdr_t ack = { .magic = DP_MAGIC, .msg_type = DP_MSG_ACK, .chunk_id = chunk_id, .status = 0 };
    dp_ack_hdr_hton(&ack);
    return full_write(impl->fd, &ack, sizeof(ack)) == (ssize_t)sizeof(ack) ? 0 : -1;
}

static int tcp_recv_ack(dp_path_t *p, uint32_t *chunk_id, int *is_done) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    dp_ack_hdr_t ack;
    ssize_t n = full_read(impl->fd, &ack, sizeof(ack));
    if (n == 0) return 1; /* 连接关闭 */
    if (n != (ssize_t)sizeof(ack)) return -1;
    dp_ack_hdr_ntoh(&ack);
    if (ack.magic != DP_MAGIC) return -1;
    if (ack.msg_type == DP_MSG_ACK) {
        *chunk_id = ack.chunk_id;
        *is_done = 0;
        return 0;
    } else if (ack.msg_type == DP_MSG_BYE) {
        *is_done = 1;
        return 0;
    }
    return -1;
}

static void tcp_set_transfer_meta(dp_path_t *p, uint64_t file_size, uint32_t chunk_size) {
    /* TCP 帧头里已经显式带了每片的实际长度，不需要这个信息 */
    (void)p; (void)file_size; (void)chunk_size;
}

static int tcp_send_done(dp_path_t *p) {
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    dp_chunk_hdr_t hdr = { .magic = DP_MAGIC, .msg_type = DP_MSG_DONE, .chunk_id = 0, .length = 0 };
    dp_chunk_hdr_hton(&hdr);
    return full_write(impl->fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) ? 0 : -1;
}

static void tcp_close(dp_path_t *p) {
    if (!p) return;
    tcp_path_impl_t *impl = (tcp_path_impl_t *)p->impl;
    if (impl) {
        if (impl->fd >= 0) {
            /* 告知对端 ACK 流结束，便于优雅退出 recv_ack 循环 */
            dp_ack_hdr_t bye = { .magic = DP_MAGIC, .msg_type = DP_MSG_BYE, .chunk_id = 0, .status = 0 };
            dp_ack_hdr_hton(&bye);
            (void)full_write(impl->fd, &bye, sizeof(bye));
            shutdown(impl->fd, SHUT_RDWR);
            close(impl->fd);
        }
        free(impl);
    }
    free(p);
}

static const dp_path_ops_t g_tcp_ops = {
    .name = "tcp",
    .client_connect = tcp_client_connect,
    .server_accept_one = tcp_server_accept_one,
    .send_hello = tcp_send_hello,
    .recv_hello = tcp_recv_hello,
    .send_hello_ack = tcp_send_hello_ack,
    .recv_hello_ack = tcp_recv_hello_ack,
    .send_chunk = tcp_send_chunk,
    .recv_chunk = tcp_recv_chunk,
    .send_ack = tcp_send_ack,
    .recv_ack = tcp_recv_ack,
    .send_done = tcp_send_done,
    .set_transfer_meta = tcp_set_transfer_meta,
    .close = tcp_close,
};

const dp_path_ops_t *dp_tcp_get_ops(void) { return &g_tcp_ops; }
