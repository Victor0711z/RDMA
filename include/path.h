/*
 * path.h - 一个 RDMA RC QP 的统一抽象
 *
 * client / server / scheduler 只依赖这一层接口，RDMA CM 和 Verbs 资源管理
 * 收敛在 rdma_path.c。一个 dp_path 对应一个独立 RC QP。
 */
#ifndef DUALPATH_PATH_H
#define DUALPATH_PATH_H

#include <stdint.h>
#include <stddef.h>
#include "protocol.h"

typedef struct {
    const char *local_ip;    /* 本机 RDMA IP */
    const char *remote_ip;   /* 对端 RDMA IP（server 侧忽略） */
    int         port;
    uint32_t    window_size; /* credit 窗口 / RDMA slot 数量 */
    uint32_t    chunk_size;  /* 单个分片的最大负载字节数 */
} dp_path_cfg_t;

struct dp_path;
typedef struct dp_path dp_path_t;

typedef struct {
    const char *name; /* "rdma"，用于日志 */

    dp_path_t *(*client_connect)(const dp_path_cfg_t *cfg);
    dp_path_t *(*server_accept_one)(const dp_path_cfg_t *cfg);

    int (*send_hello)(dp_path_t *p, const dp_hello_t *hello);
    int (*recv_hello)(dp_path_t *p, dp_hello_t *hello);
    int (*send_hello_ack)(dp_path_t *p, const dp_hello_ack_t *ack);
    int (*recv_hello_ack)(dp_path_t *p, dp_hello_ack_t *ack);

    /* 通用发送接口；len 为负载字节数。 */
    int (*send_chunk)(dp_path_t *p, uint32_t chunk_id, const void *data, uint32_t len);

    /*
     * 零拷贝发送接口：先预留一个已注册 slot，调用方直接把文件读入 payload，
     * 再 commit 计算 CRC/填写 header/post WRITE。读取失败时必须 release。
     */
    int  (*acquire_chunk_buffer)(dp_path_t *p, uint32_t chunk_id, uint32_t len, void **payload);
    int  (*commit_chunk)(dp_path_t *p, uint32_t chunk_id, uint32_t len);
    void (*release_chunk_buffer)(dp_path_t *p, uint32_t chunk_id);

    /* 返回服务端注册 slot 内的 payload 视图；该指针在发送对应 ACK 前有效。 */
    int (*recv_chunk)(dp_path_t *p, uint32_t *chunk_id, const void **data, uint32_t *out_len);

    int (*send_ack)(dp_path_t *p, uint32_t chunk_id);
    /* 阻塞接收一个 ACK/DONE；*is_done 用于区分收到的是普通 ACK 还是 DONE */
    int (*recv_ack)(dp_path_t *p, uint32_t *chunk_id, int *is_done);

    int (*send_done)(dp_path_t *p);

    /* 告知 QP 本次传输的 file_size / chunk_size。 */
    void (*set_transfer_meta)(dp_path_t *p, uint64_t file_size, uint32_t chunk_size);

    /* 只中断 I/O、唤醒阻塞线程，不释放对象；随后仍由 close 统一释放。 */
    void (*shutdown)(dp_path_t *p);

    void (*close)(dp_path_t *p);
} dp_path_ops_t;

struct dp_path {
    int                  id;      /* 路径编号，方便日志 */
    const dp_path_ops_t *ops;
    void                *impl;    /* RDMA CM/Verbs 私有状态 */
};

/* 工厂函数：创建 client / server RC QP。 */
dp_path_t *dp_path_create_client(int qp_id, const dp_path_cfg_t *cfg);
dp_path_t *dp_path_create_server(int qp_id, const dp_path_cfg_t *cfg);

/* RDMA 后端注册 ops。 */
const dp_path_ops_t *dp_rdma_get_ops(void);

/* 便捷内联包装，避免上层代码到处写 p->ops->xxx(p, ...) */
static inline int dp_path_send_hello(dp_path_t *p, const dp_hello_t *h) { return p->ops->send_hello(p, h); }
static inline int dp_path_recv_hello(dp_path_t *p, dp_hello_t *h) { return p->ops->recv_hello(p, h); }
static inline int dp_path_send_hello_ack(dp_path_t *p, const dp_hello_ack_t *a) { return p->ops->send_hello_ack(p, a); }
static inline int dp_path_recv_hello_ack(dp_path_t *p, dp_hello_ack_t *a) { return p->ops->recv_hello_ack(p, a); }
static inline int dp_path_send_chunk(dp_path_t *p, uint32_t id, const void *d, uint32_t l) { return p->ops->send_chunk(p, id, d, l); }
static inline int dp_path_acquire_chunk_buffer(dp_path_t *p, uint32_t id, uint32_t len, void **payload) {
    return p->ops->acquire_chunk_buffer(p, id, len, payload);
}
static inline int dp_path_commit_chunk(dp_path_t *p, uint32_t id, uint32_t len) {
    return p->ops->commit_chunk(p, id, len);
}
static inline void dp_path_release_chunk_buffer(dp_path_t *p, uint32_t id) {
    p->ops->release_chunk_buffer(p, id);
}
static inline int dp_path_recv_chunk(dp_path_t *p, uint32_t *id, const void **data, uint32_t *ol) {
    return p->ops->recv_chunk(p, id, data, ol);
}
static inline int dp_path_send_ack(dp_path_t *p, uint32_t id) { return p->ops->send_ack(p, id); }
static inline int dp_path_recv_ack(dp_path_t *p, uint32_t *id, int *is_done) { return p->ops->recv_ack(p, id, is_done); }
static inline int dp_path_send_done(dp_path_t *p) { return p->ops->send_done(p); }
static inline void dp_path_set_transfer_meta(dp_path_t *p, uint64_t fs, uint32_t cs) {
    if (p->ops->set_transfer_meta) p->ops->set_transfer_meta(p, fs, cs);
}
static inline void dp_path_shutdown(dp_path_t *p) { if (p && p->ops->shutdown) p->ops->shutdown(p); }
static inline void dp_path_close(dp_path_t *p) { if (p) p->ops->close(p); }

#endif /* DUALPATH_PATH_H */
