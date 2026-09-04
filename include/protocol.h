/*
 * protocol.h - 线路协议定义 (Wire protocol definitions)
 *
 * DualPath-RDMA 使用的应用层协议非常简单：
 *   - 控制通道（第 0 条路径）用于一次性的握手：文件名/文件大小/分片大小/路径数
 *   - 每条数据路径上以“分片帧”为单位传输数据，每个分片被服务端 ACK 一次，
 *     从而实现基于 credit 的简单流控（滑动窗口）。
 *
 * TCP 后端和 RDMA 后端共用同一套语义：
 *   - TCP 后端：分片帧 = [chunk_header_t][payload]，ACK 帧 = [ack_header_t]
 *   - RDMA 后端：分片数据通过 RDMA WRITE 直接写入对端预先注册好的 slot，
 *     并用 imm_data 携带 chunk_header_t 的定长信息；ACK 通过一次
 *     SEND/RECV 双边操作发送 ack_header_t。
 *
 * 因此上层的 client/server 逻辑完全不关心底层是 TCP 还是 RDMA。
 */
#ifndef DUALPATH_PROTOCOL_H
#define DUALPATH_PROTOCOL_H

#include <stdint.h>

#define DP_MAGIC        0x44504C31u /* "DPL1" */
#define DP_MAX_PATHS    8
#define DP_MAX_NAME     256
#define DP_DEFAULT_CHUNK_SIZE   (1u << 20)   /* 1 MiB */
#define DP_DEFAULT_SLOTS_PER_PATH 8          /* RDMA/credit 窗口大小 */

/* 消息类型 */
enum dp_msg_type {
    DP_MSG_HELLO       = 1,  /* client -> server: 传输任务元信息（仅控制通道） */
    DP_MSG_HELLO_ACK   = 2,  /* server -> client: 握手确认，附服务端准备好的窗口大小 */
    DP_MSG_CHUNK       = 3,  /* client -> server: 数据分片 */
    DP_MSG_ACK         = 4,  /* server -> client: 分片确认（释放 credit） */
    DP_MSG_DONE        = 5,  /* client -> server: 全部分片发送完毕（每条路径各发一次） */
    DP_MSG_BYE         = 6,  /* 任一方：正常关闭该路径 */
};

/* 控制通道握手：client -> server */
typedef struct __attribute__((packed)) dp_hello {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_HELLO */
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t num_paths;
    uint32_t total_chunks;
    char     file_name[DP_MAX_NAME];
} dp_hello_t;

typedef struct __attribute__((packed)) dp_hello_ack {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_HELLO_ACK */
    uint32_t window_size;   /* 服务端每条路径愿意接受的最大在途分片数 */
    uint32_t status;        /* 0 = OK, 非 0 = 错误码 */
} dp_hello_ack_t;

/* 数据分片帧头（TCP 上位于 payload 之前；RDMA 上打包进 32bit immediate） */
typedef struct __attribute__((packed)) dp_chunk_hdr {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_CHUNK */
    uint32_t chunk_id;
    uint32_t length;        /* 本分片的实际字节数（最后一片可能 < chunk_size） */
} dp_chunk_hdr_t;

/* RDMA immediate data 的紧凑编码：高 8 位 = slot 索引，低 24 位 = chunk_id */
#define DP_IMM_ENCODE(slot, chunk_id)  ((((uint32_t)(slot) & 0xFFu) << 24) | ((uint32_t)(chunk_id) & 0x00FFFFFFu))
#define DP_IMM_SLOT(imm)                (((uint32_t)(imm) >> 24) & 0xFFu)
#define DP_IMM_CHUNK(imm)               ((uint32_t)(imm) & 0x00FFFFFFu)

typedef struct __attribute__((packed)) dp_ack_hdr {
    uint32_t magic;
    uint32_t msg_type;      /* DP_MSG_ACK / DP_MSG_DONE / DP_MSG_BYE */
    uint32_t chunk_id;
    uint32_t status;
} dp_ack_hdr_t;

/* 主机序 <-> 网络序转换（实现见 protocol.c），TCP 后端在收发时调用 */
void dp_hello_hton(dp_hello_t *h);
void dp_hello_ntoh(dp_hello_t *h);
void dp_hello_ack_hton(dp_hello_ack_t *a);
void dp_hello_ack_ntoh(dp_hello_ack_t *a);
void dp_chunk_hdr_hton(dp_chunk_hdr_t *c);
void dp_chunk_hdr_ntoh(dp_chunk_hdr_t *c);
void dp_ack_hdr_hton(dp_ack_hdr_t *a);
void dp_ack_hdr_ntoh(dp_ack_hdr_t *a);

#endif /* DUALPATH_PROTOCOL_H */
