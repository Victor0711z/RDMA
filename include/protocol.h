/*
 * protocol.h - 线路协议定义 (Wire protocol definitions)
 *
 * 数据面使用 RDMA WRITE WITH IMM；每个远端 slot 包含定长元数据头和 payload。
 * 控制消息与 ACK 使用 SEND/RECV，分片完成状态由 credit 窗口约束。
 */
#ifndef DUALPATH_PROTOCOL_H
#define DUALPATH_PROTOCOL_H

#include <stdint.h>

#define DP_MAGIC        0x44504C31u /* "DPL1" */
#define DP_PROTOCOL_VERSION 2u
#define DP_MAX_PATHS    8
#define DP_MAX_NAME     256
#define DP_DEFAULT_CHUNK_SIZE   (1u << 20)   /* 1 MiB */
#define DP_DEFAULT_SLOTS_PER_PATH 8          /* RDMA/credit 窗口大小 */

/* 消息类型 */
enum dp_msg_type {
    DP_MSG_HELLO       = 1,  /* client -> server: 传输任务元信息（仅控制通道） */
    DP_MSG_HELLO_ACK   = 2,  /* server -> client: 握手确认，附服务端准备好的窗口大小 */
    DP_MSG_ACK         = 4,  /* server -> client: 分片确认（释放 credit） */
    DP_MSG_DONE        = 5,  /* client -> server: 全部分片发送完毕（每个 QP 各发一次） */
    DP_MSG_BYE         = 6,  /* 任一方：正常关闭该 QP */
};

/* 控制通道握手：client -> server */
typedef struct __attribute__((packed)) dp_hello {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_HELLO */
    uint32_t version;
    uint32_t flags;
    uint64_t session_id;
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t num_qps;
    uint32_t total_chunks;
    char     file_name[DP_MAX_NAME];
} dp_hello_t;

typedef struct __attribute__((packed)) dp_rdma_slot_hdr {
    uint32_t magic;
    uint32_t chunk_id;
    uint32_t length;
    uint32_t crc32c;
} dp_rdma_slot_hdr_t;

typedef struct __attribute__((packed)) dp_hello_ack {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_HELLO_ACK */
    uint32_t window_size;   /* 服务端每个 QP 愿意接受的最大在途分片数 */
    uint32_t status;        /* 0 = OK, 非 0 = 错误码 */
} dp_hello_ack_t;

/* DONE 使用的 SEND/RECV 控制帧；数据分片使用 dp_rdma_slot_hdr_t。 */
typedef struct __attribute__((packed)) dp_done_hdr {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_DONE */
    uint32_t reserved0;
    uint32_t reserved1;
} dp_done_hdr_t;

typedef struct __attribute__((packed)) dp_ack_hdr {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_ACK / DP_MSG_DONE / DP_MSG_BYE */
    uint32_t chunk_id;
    uint32_t status;
} dp_ack_hdr_t;

/* 主机序 <-> 网络序转换（实现见 protocol.c） */
void dp_hello_hton(dp_hello_t *h);
void dp_hello_ntoh(dp_hello_t *h);
void dp_hello_ack_hton(dp_hello_ack_t *a);
void dp_hello_ack_ntoh(dp_hello_ack_t *a);
void dp_done_hdr_hton(dp_done_hdr_t *d);
void dp_done_hdr_ntoh(dp_done_hdr_t *d);
void dp_ack_hdr_hton(dp_ack_hdr_t *a);
void dp_ack_hdr_ntoh(dp_ack_hdr_t *a);

#endif /* DUALPATH_PROTOCOL_H */
