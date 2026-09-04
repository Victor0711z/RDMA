/*
 * path.h - “一条网卡上的一条连接”的统一抽象 (transport-agnostic path)
 *
 * client / server / scheduler 的业务逻辑只依赖这一层接口，完全不知道
 * 底层到底是 TCP socket 还是 RDMA queue pair。这样做的好处：
 *   1) 双网卡负载均衡 + 故障转移算法可以脱离 RDMA 硬件独立开发、测试；
 *   2) 在没有 RDMA 网卡的机器上可以用 --transport=tcp 完整跑通整条链路，
 *      作为 CI / 演示 / 本地开发用的“软件模拟双网卡”；
 *   3) 在真实 RDMA 双网卡环境下切换 --transport=rdma 即可获得零拷贝、
 *      内核旁路的真正 RDMA WRITE 数据面，业务代码一行不改。
 */
#ifndef DUALPATH_PATH_H
#define DUALPATH_PATH_H

#include <stdint.h>
#include <stddef.h>
#include "protocol.h"

typedef enum { DP_TRANSPORT_TCP = 0, DP_TRANSPORT_RDMA = 1 } dp_transport_t;

typedef struct {
    const char *local_ip;    /* 绑定的本地 IP（决定走哪张网卡） */
    const char *remote_ip;   /* 对端 IP（server 侧忽略，仅用于 listen） */
    int         port;
    uint32_t    window_size; /* credit 窗口 / RDMA slot 数量 */
    uint32_t    chunk_size;  /* 单个分片的最大负载字节数 */
    int         is_control;  /* 是否为控制通道（path 0），仅控制通道传 HELLO */
} dp_path_cfg_t;

struct dp_path;
typedef struct dp_path dp_path_t;

typedef struct {
    const char *name; /* "tcp" / "rdma"，用于日志 */

    dp_path_t *(*client_connect)(const dp_path_cfg_t *cfg);
    dp_path_t *(*server_accept_one)(const dp_path_cfg_t *cfg);

    int (*send_hello)(dp_path_t *p, const dp_hello_t *hello);
    int (*recv_hello)(dp_path_t *p, dp_hello_t *hello);
    int (*send_hello_ack)(dp_path_t *p, const dp_hello_ack_t *ack);
    int (*recv_hello_ack)(dp_path_t *p, dp_hello_ack_t *ack);

    /* len 为负载字节数；返回 0 成功，<0 失败（连接已断开等） */
    int (*send_chunk)(dp_path_t *p, uint32_t chunk_id, const void *data, uint32_t len);
    /* 阻塞接收一个分片；buf_cap 必须 >= 协商好的 chunk_size */
    int (*recv_chunk)(dp_path_t *p, uint32_t *chunk_id, void *buf, uint32_t buf_cap, uint32_t *out_len);

    int (*send_ack)(dp_path_t *p, uint32_t chunk_id);
    /* 阻塞接收一个 ACK/DONE；*is_done 用于区分收到的是普通 ACK 还是 DONE */
    int (*recv_ack)(dp_path_t *p, uint32_t *chunk_id, int *is_done);

    int (*send_done)(dp_path_t *p);

    /*
     * 可选：告知该路径本次传输的 file_size / chunk_size。
     * TCP 后端不需要这个信息（分片长度已经在帧头里显式传输了），空实现即可。
     * RDMA 后端需要它：因为 RDMA WRITE 的 immediate data 只有 32 bit，
     * 塞不下"分片长度"，所以约定 server 收到分片后按 (chunk_id, file_size,
     * chunk_size) 用和 client 完全相同的公式自己算出这一片应该有多少字节
     * （公式: min(chunk_size, file_size - chunk_id*chunk_size)），
     * 双方各自算、结果必然一致，就不需要在线路上额外传这个字段了。
     */
    void (*set_transfer_meta)(dp_path_t *p, uint64_t file_size, uint32_t chunk_size);

    void (*close)(dp_path_t *p);
} dp_path_ops_t;

struct dp_path {
    dp_transport_t       type;
    int                  id;      /* 路径编号，方便日志 */
    const dp_path_ops_t *ops;
    void                *impl;    /* 具体后端(tcp/rdma)的私有状态 */
};

/* 工厂函数：按 transport 类型创建 client / server 路径 */
dp_path_t *dp_path_create_client(dp_transport_t type, int path_id, const dp_path_cfg_t *cfg);
dp_path_t *dp_path_create_server(dp_transport_t type, int path_id, const dp_path_cfg_t *cfg);

/* 各后端注册自己的 ops（tcp_path.c / rdma_path.c 中实现） */
const dp_path_ops_t *dp_tcp_get_ops(void);
#ifdef DP_ENABLE_RDMA
const dp_path_ops_t *dp_rdma_get_ops(void);
#endif

/* 便捷内联包装，避免上层代码到处写 p->ops->xxx(p, ...) */
static inline int dp_path_send_hello(dp_path_t *p, const dp_hello_t *h) { return p->ops->send_hello(p, h); }
static inline int dp_path_recv_hello(dp_path_t *p, dp_hello_t *h) { return p->ops->recv_hello(p, h); }
static inline int dp_path_send_hello_ack(dp_path_t *p, const dp_hello_ack_t *a) { return p->ops->send_hello_ack(p, a); }
static inline int dp_path_recv_hello_ack(dp_path_t *p, dp_hello_ack_t *a) { return p->ops->recv_hello_ack(p, a); }
static inline int dp_path_send_chunk(dp_path_t *p, uint32_t id, const void *d, uint32_t l) { return p->ops->send_chunk(p, id, d, l); }
static inline int dp_path_recv_chunk(dp_path_t *p, uint32_t *id, void *b, uint32_t c, uint32_t *ol) { return p->ops->recv_chunk(p, id, b, c, ol); }
static inline int dp_path_send_ack(dp_path_t *p, uint32_t id) { return p->ops->send_ack(p, id); }
static inline int dp_path_recv_ack(dp_path_t *p, uint32_t *id, int *is_done) { return p->ops->recv_ack(p, id, is_done); }
static inline int dp_path_send_done(dp_path_t *p) { return p->ops->send_done(p); }
static inline void dp_path_set_transfer_meta(dp_path_t *p, uint64_t fs, uint32_t cs) {
    if (p->ops->set_transfer_meta) p->ops->set_transfer_meta(p, fs, cs);
}
static inline void dp_path_close(dp_path_t *p) { if (p) p->ops->close(p); }

#endif /* DUALPATH_PATH_H */
