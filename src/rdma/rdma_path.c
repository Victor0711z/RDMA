/*
 * rdma_path.c - path.h 接口的 RDMA Verbs 实现（libibverbs + rdma_cm）
 *
 * ============================== 设计说明 ==============================
 *
 * 一条 path 对应一条 RC (Reliable Connection) Queue Pair，线路上有两类流量：
 *
 *   1) 控制消息（HELLO / HELLO_ACK / ACK / DONE / BYE / 内部的 slot 元信息）
 *      —— 用标准的"双边"操作 SEND/RECV 实现，跟 socket send/recv 心智模型
 *      基本一致，复用 protocol.h 里跟 TCP 后端完全相同的结构体和网络字节序
 *      转换函数。
 *
 *   2) 分片数据本体 —— 用 RDMA WRITE + Immediate Data 实现"单边"零拷贝写：
 *      - 连接建立后，server（接收方）注册一块大内存 recv_slots（window 个
 *        slot，每个 slot 能装一个分片），把这块内存的 (addr, rkey) 通过一条
 *        控制消息告诉 client；
 *      - client（发送方）把分片数据 RDMA WRITE 直接写到 server 那块内存里
 *        对应的 slot 上，同时在 immediate data 里带上 (slot 编号, chunk_id)，
 *        server 端不需要 CPU 参与数据搬运（数据由网卡直接 DMA 到目标内存），
 *        只需要有一个预先 post 好的 RECV 来"被通知"数据到了；
 *      - immediate data 只有 32 bit，塞不下"这一片有多少字节"，所以约定
 *        双方各自用 (chunk_id, file_size, chunk_size) 算出同样的长度
 *        （见 path.h 里 set_transfer_meta 的注释），不需要额外传输。
 *
 * slot 复用：每条 path 上，同一时刻最多有 window 个分片在途未 ACK（这是
 * scheduler.c 的 credit 机制保证的），所以 client 侧用一个简单的自增计数器
 * `next_send_slot % window` 选 slot 就足够安全——只有当某个 slot 里的分片被
 * server 处理完并 ACK 回来后，credit 才会被释放、才可能有新的分片被调度到
 * 这条 path 上，而这必然晚于 window 个分片之前那次对同一 slot 的写入完成，
 * 不会出现"新数据还没写完就被下一次写入覆盖"的情况。
 *
 * ============================== 已知限制 ==============================
 * - 控制通道走双边 SEND/RECV，量大（比如每个分片一个 ACK）时会有一些 CPU
 *   开销，工程上可以进一步优化成攒批 ACK 或者用 RDMA Write 回传，这里为了
 *   代码复杂度可控没有做。
 * - 没有做超时重传：只有连接层面报错（对端断开/QP 出错）才会触发故障转移，
 *   如果链路"静默丢包/挂起但连接不报错"，目前是检测不出来的。
 * - 本文件在没有 libibverbs/librdmacm 头文件的环境下无法编译（正常现象，
 *   这两个库只在装了 RDMA 网卡驱动的机器上才有），需要在有 RDMA 环境的机器
 *   上 `make rdma` 编译验证。
 */
#ifdef DP_ENABLE_RDMA

#include "path.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define CTRL_MSG_SIZE   512   /* 足够装下最大的控制结构体 dp_hello_t */
#define CQ_POLL_BACKOFF_US 200

typedef struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id         *cm_id;
    struct ibv_pd              *pd;
    struct ibv_cq              *send_cq;
    struct ibv_cq               *recv_cq;

    uint32_t window;
    uint32_t chunk_size;
    int      is_server;      /* 0 = client(发送方/WRITE发起者), 1 = server(接收方/WRITE目标) */
    volatile int broken;     /* 一旦检测到错误，后续调用直接快速失败 */

    /* --- 控制消息通道（双边 SEND/RECV），client/server 都要用到 --- */
    uint8_t        *ctrl_recv_buf;   /* ctrl_recv_depth 个 CTRL_MSG_SIZE 大小的环形缓冲区 */
    struct ibv_mr   *ctrl_recv_mr;
    uint32_t         ctrl_recv_depth;
    uint32_t         ctrl_recv_next; /* 下一个要从环里取的下标（也是下一个要重新 post 的下标） */

    uint8_t        *ctrl_send_buf;   /* 控制消息发送是同步的，一块缓冲区复用即可 */
    struct ibv_mr   *ctrl_send_mr;

    /* --- client 专用：本地发送 slot（RDMA WRITE 的数据源） --- */
    uint8_t        *local_slots;
    struct ibv_mr   *local_slots_mr;
    uint64_t         remote_addr;
    uint32_t         remote_rkey;
    uint32_t         next_send_slot;

    /* --- server 专用：接收 slot（RDMA WRITE 的目标） --- */
    uint8_t        *recv_slots;
    struct ibv_mr   *recv_slots_mr;

    /* set_transfer_meta 传入，server 用来自己算每片长度 */
    uint64_t file_size;
    uint32_t xfer_chunk_size;
} rdma_impl_t;

/* 线路上的内部消息：server 建链后主动把接收 slot 的地址/rkey 告诉 client */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t msg_type;   /* 复用 DP_MSG_HELLO_ACK 之外的一个私有类型，直接用一个新常量 */
    uint64_t addr;
    uint32_t rkey;
    uint32_t slot_size;
    uint32_t num_slots;
} dp_slotinfo_t;
#define DP_MSG_SLOTINFO 100u

/* ---------------------------------------------------------------------- */
/* 小工具                                                                  */
/* ---------------------------------------------------------------------- */

static int poll_cq_blocking(struct ibv_cq *cq, struct ibv_wc *wc) {
    for (;;) {
        int n = ibv_poll_cq(cq, 1, wc);
        if (n < 0) return -1;
        if (n == 1) return wc->status == IBV_WC_SUCCESS ? 0 : -1;
        usleep(CQ_POLL_BACKOFF_US);
    }
}

/* 非阻塞地把 send_cq 里已经完成的 WR 都收走，主要用来及时发现错误、避免 CQ 堆积 */
static void drain_send_cq(rdma_impl_t *impl) {
    struct ibv_wc wc;
    for (;;) {
        int n = ibv_poll_cq(impl->send_cq, 1, &wc);
        if (n <= 0) break;
        if (wc.status != IBV_WC_SUCCESS) {
            DP_LOGW("rdma: send completion 出错 status=%d", (int)wc.status);
            impl->broken = 1;
        }
    }
}

static int post_ctrl_recv(rdma_impl_t *impl, uint32_t ring_idx) {
    struct ibv_sge sge = {
        .addr = (uintptr_t)(impl->ctrl_recv_buf + (size_t)ring_idx * CTRL_MSG_SIZE),
        .length = CTRL_MSG_SIZE,
        .lkey = impl->ctrl_recv_mr->lkey,
    };
    struct ibv_recv_wr wr = { .wr_id = ring_idx, .sg_list = &sge, .num_sge = 1 };
    struct ibv_recv_wr *bad;
    return ibv_post_recv(impl->cm_id->qp, &wr, &bad);
}

/* 同步发送一条控制消息：拷贝进 ctrl_send_buf、post SEND、等它完成 */
static int ctrl_send(rdma_impl_t *impl, const void *data, uint32_t len) {
    if (len > CTRL_MSG_SIZE) { DP_LOGE("rdma: ctrl 消息过大 (%u > %d)", len, CTRL_MSG_SIZE); return -1; }
    memcpy(impl->ctrl_send_buf, data, len);
    struct ibv_sge sge = { .addr = (uintptr_t)impl->ctrl_send_buf, .length = len, .lkey = impl->ctrl_send_mr->lkey };
    struct ibv_send_wr wr = {
        .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED, .sg_list = &sge, .num_sge = 1,
    };
    struct ibv_send_wr *bad;
    if (ibv_post_send(impl->cm_id->qp, &wr, &bad) != 0) { impl->broken = 1; return -1; }
    struct ibv_wc wc;
    if (poll_cq_blocking(impl->send_cq, &wc) != 0) { impl->broken = 1; return -1; }
    return 0;
}

/* 阻塞等待下一条"双边"控制消息（IBV_WC_RECV），返回数据长度；用完立刻把该环形槽位重新 post 出去 */
static int ctrl_recv_wait(rdma_impl_t *impl, void *out, uint32_t out_cap, uint32_t *out_len) {
    for (;;) {
        struct ibv_wc wc;
        int n;
        do {
            n = ibv_poll_cq(impl->recv_cq, 1, &wc);
            if (n < 0) { impl->broken = 1; return -1; }
            if (n == 0) usleep(CQ_POLL_BACKOFF_US);
        } while (n == 0);

        if (wc.status != IBV_WC_SUCCESS) { impl->broken = 1; return -1; }

        if (wc.opcode == IBV_WC_RECV) {
            uint32_t idx = (uint32_t)wc.wr_id;
            uint32_t len = wc.byte_len;
            if (len > out_cap) { DP_LOGE("rdma: ctrl 消息长度超出缓冲区"); return -1; }
            memcpy(out, impl->ctrl_recv_buf + (size_t)idx * CTRL_MSG_SIZE, len);
            if (post_ctrl_recv(impl, idx) != 0) { impl->broken = 1; return -1; }
            *out_len = len;
            return 0;
        }
        /* 收到的是数据面的 WRITE_WITH_IMM 通知，但当前调用方要的是控制消息——
         * 这种情况在本项目的时序里不会发生（握手阶段还没有分片在飞），
         * 稳妥起见记录一下然后继续等待，不至于卡死 */
        DP_LOGW("rdma: ctrl_recv_wait 收到非预期的 opcode=%d，忽略", (int)wc.opcode);
    }
}

/* ---------------------------------------------------------------------- */
/* 连接建立                                                                */
/* ---------------------------------------------------------------------- */

static int wait_cm_event(struct rdma_event_channel *ec, enum rdma_cm_event_type expect,
                          struct rdma_cm_id **new_id_out) {
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ec, &ev) != 0) return -1;
    if (ev->event != expect) {
        DP_LOGE("rdma: 期望事件 %d 但收到 %d", (int)expect, (int)ev->event);
        rdma_ack_cm_event(ev);
        return -1;
    }
    if (new_id_out) *new_id_out = ev->id;
    rdma_ack_cm_event(ev);
    return 0;
}

static int setup_qp_and_buffers(rdma_impl_t *impl) {
    impl->send_cq = ibv_create_cq(impl->cm_id->verbs, impl->window + impl->ctrl_recv_depth + 16, NULL, NULL, 0);
    impl->recv_cq = ibv_create_cq(impl->cm_id->verbs, impl->window + impl->ctrl_recv_depth + 16, NULL, NULL, 0);
    if (!impl->send_cq || !impl->recv_cq) { DP_LOGE("rdma: ibv_create_cq 失败"); return -1; }

    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.send_cq = impl->send_cq;
    qp_attr.recv_cq = impl->recv_cq;
    qp_attr.cap.max_send_wr = impl->window + impl->ctrl_recv_depth + 16;
    qp_attr.cap.max_recv_wr = impl->window + impl->ctrl_recv_depth + 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(impl->cm_id, impl->pd, &qp_attr) != 0) {
        DP_LOGE("rdma: rdma_create_qp 失败: %s", strerror(errno));
        return -1;
    }

    impl->ctrl_recv_buf = (uint8_t *)malloc((size_t)impl->ctrl_recv_depth * CTRL_MSG_SIZE);
    impl->ctrl_recv_mr = ibv_reg_mr(impl->pd, impl->ctrl_recv_buf,
                                     (size_t)impl->ctrl_recv_depth * CTRL_MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
    impl->ctrl_send_buf = (uint8_t *)malloc(CTRL_MSG_SIZE);
    impl->ctrl_send_mr = ibv_reg_mr(impl->pd, impl->ctrl_send_buf, CTRL_MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!impl->ctrl_recv_mr || !impl->ctrl_send_mr) { DP_LOGE("rdma: 注册控制消息 MR 失败"); return -1; }

    for (uint32_t i = 0; i < impl->ctrl_recv_depth; i++) {
        if (post_ctrl_recv(impl, i) != 0) { DP_LOGE("rdma: post 初始 ctrl recv 失败"); return -1; }
    }
    impl->ctrl_recv_next = 0;

    if (impl->is_server) {
        size_t total = (size_t)impl->window * impl->chunk_size;
        impl->recv_slots = (uint8_t *)malloc(total);
        impl->recv_slots_mr = ibv_reg_mr(impl->pd, impl->recv_slots, total,
                                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!impl->recv_slots_mr) {
            DP_LOGE("rdma: 注册 recv_slots MR 失败 (errno=%d: %s)", errno, strerror(errno));
            DP_LOGE("rdma: 如果是在阿里云 eRDMA 等云厂商 RDMA 网卡上，部分实现对内存注册"
                    "大小有下限/上限要求，可以尝试调大 config.ini 里的 window/chunk_size"
                    "（当前 window=%u chunk_size=%u，共 %zu 字节）", impl->window, impl->chunk_size, total);
            return -1;
        }
        /* 为每个 slot post 一个 0 字节的"门铃" RECV，专门用来接住 WRITE_WITH_IMM 的通知，
         * 数据本身由网卡直接 DMA 进 recv_slots，这个 RECV 不搬运任何数据 */
        for (uint32_t i = 0; i < impl->window; i++) {
            struct ibv_recv_wr wr = { .wr_id = 0xFFFF0000u | i, .sg_list = NULL, .num_sge = 0 };
            struct ibv_recv_wr *bad;
            if (ibv_post_recv(impl->cm_id->qp, &wr, &bad) != 0) {
                DP_LOGE("rdma: post 门铃 recv 失败");
                return -1;
            }
        }
    } else {
        size_t total = (size_t)impl->window * impl->chunk_size;
        impl->local_slots = (uint8_t *)malloc(total);
        impl->local_slots_mr = ibv_reg_mr(impl->pd, impl->local_slots, total, IBV_ACCESS_LOCAL_WRITE);
        if (!impl->local_slots_mr) {
            DP_LOGE("rdma: 注册 local_slots MR 失败 (errno=%d: %s)", errno, strerror(errno));
            DP_LOGE("rdma: 如果是在阿里云 eRDMA 等云厂商 RDMA 网卡上，部分实现对内存注册"
                    "大小有下限/上限要求，可以尝试调大 config.ini 里的 window/chunk_size"
                    "（当前 window=%u chunk_size=%u，共 %zu 字节）", impl->window, impl->chunk_size, total);
            return -1;
        }
    }
    return 0;
}

static dp_path_t *wrap(int id, rdma_impl_t *impl);

static dp_path_t *rdma_client_connect(const dp_path_cfg_t *cfg) {
    rdma_impl_t *impl = (rdma_impl_t *)calloc(1, sizeof(*impl));
    impl->window = cfg->window_size;
    impl->chunk_size = cfg->chunk_size;
    impl->ctrl_recv_depth = cfg->window_size + 16;
    impl->is_server = 0;

    impl->ec = rdma_create_event_channel();
    if (!impl->ec || rdma_create_id(impl->ec, &impl->cm_id, NULL, RDMA_PS_TCP) != 0) {
        DP_LOGE("rdma: rdma_create_id 失败"); goto fail;
    }

    struct sockaddr_in local_addr, remote_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    if (cfg->local_ip && cfg->local_ip[0]) inet_pton(AF_INET, cfg->local_ip, &local_addr.sin_addr);
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons((uint16_t)cfg->port);
    inet_pton(AF_INET, cfg->remote_ip, &remote_addr.sin_addr);

    if (rdma_resolve_addr(impl->cm_id, (struct sockaddr *)&local_addr, (struct sockaddr *)&remote_addr, 3000) != 0) {
        DP_LOGE("rdma: rdma_resolve_addr 失败: %s", strerror(errno)); goto fail;
    }
    if (wait_cm_event(impl->ec, RDMA_CM_EVENT_ADDR_RESOLVED, NULL) != 0) goto fail;
    if (rdma_resolve_route(impl->cm_id, 3000) != 0) { DP_LOGE("rdma: rdma_resolve_route 失败"); goto fail; }
    if (wait_cm_event(impl->ec, RDMA_CM_EVENT_ROUTE_RESOLVED, NULL) != 0) goto fail;

    impl->pd = ibv_alloc_pd(impl->cm_id->verbs);
    if (!impl->pd) { DP_LOGE("rdma: ibv_alloc_pd 失败"); goto fail; }
    if (setup_qp_and_buffers(impl) != 0) goto fail;

    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 4;
    conn_param.responder_resources = 4;
    conn_param.retry_count = 5;
    conn_param.rnr_retry_count = 5;
    if (rdma_connect(impl->cm_id, &conn_param) != 0) { DP_LOGE("rdma: rdma_connect 失败: %s", strerror(errno)); goto fail; }
    if (wait_cm_event(impl->ec, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) { DP_LOGE("rdma: 建链失败"); goto fail; }

    /* server 建链后会主动发一条 slotinfo 消息过来，告诉我们它接收缓冲区的 addr/rkey */
    uint8_t buf[CTRL_MSG_SIZE];
    uint32_t len;
    if (ctrl_recv_wait(impl, buf, sizeof(buf), &len) != 0 || len != sizeof(dp_slotinfo_t)) {
        DP_LOGE("rdma: 接收 slotinfo 失败"); goto fail;
    }
    dp_slotinfo_t *si = (dp_slotinfo_t *)buf;
    if (ntohl(si->magic) != DP_MAGIC || ntohl(si->msg_type) != DP_MSG_SLOTINFO) {
        DP_LOGE("rdma: slotinfo 格式不对"); goto fail;
    }
    impl->remote_addr = be64toh(si->addr);
    impl->remote_rkey = ntohl(si->rkey);

    DP_LOGI("rdma path 建链成功 (client): local=%s -> remote=%s:%d, remote slot addr=0x%lx rkey=0x%x",
            cfg->local_ip ? cfg->local_ip : "(any)", cfg->remote_ip, cfg->port,
            (unsigned long)impl->remote_addr, impl->remote_rkey);
    return wrap(0, impl);

fail:
    /* 尽力清理，具体细节见 rdma_close */
    return NULL;
}

static dp_path_t *rdma_server_accept_one(const dp_path_cfg_t *cfg) {
    rdma_impl_t *impl = (rdma_impl_t *)calloc(1, sizeof(*impl));
    impl->window = cfg->window_size;
    impl->chunk_size = cfg->chunk_size;
    impl->ctrl_recv_depth = cfg->window_size + 16;
    impl->is_server = 1;

    impl->ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id;
    if (!impl->ec || rdma_create_id(impl->ec, &listen_id, NULL, RDMA_PS_TCP) != 0) {
        DP_LOGE("rdma: rdma_create_id (listen) 失败"); goto fail_early;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)cfg->port);
    const char *bind_ip = (cfg->local_ip && cfg->local_ip[0]) ? cfg->local_ip : "0.0.0.0";
    inet_pton(AF_INET, bind_ip, &bind_addr.sin_addr);

    if (rdma_bind_addr(listen_id, (struct sockaddr *)&bind_addr) != 0) {
        DP_LOGE("rdma: rdma_bind_addr(%s:%d) 失败: %s", bind_ip, cfg->port, strerror(errno));
        goto fail_listen;
    }
    if (rdma_listen(listen_id, 1) != 0) { DP_LOGE("rdma: rdma_listen 失败"); goto fail_listen; }
    DP_LOGI("rdma path listening on %s:%d ...", bind_ip, cfg->port);

    struct rdma_cm_id *new_id = NULL;
    if (wait_cm_event(impl->ec, RDMA_CM_EVENT_CONNECT_REQUEST, &new_id) != 0 || !new_id) {
        DP_LOGE("rdma: 等待连接请求失败"); goto fail_listen;
    }
    impl->cm_id = new_id;

    impl->pd = ibv_alloc_pd(impl->cm_id->verbs);
    if (!impl->pd) { DP_LOGE("rdma: ibv_alloc_pd 失败"); goto fail; }
    if (setup_qp_and_buffers(impl) != 0) goto fail;

    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 4;
    conn_param.responder_resources = 4;
    conn_param.rnr_retry_count = 5;
    if (rdma_accept(impl->cm_id, &conn_param) != 0) { DP_LOGE("rdma: rdma_accept 失败: %s", strerror(errno)); goto fail; }
    if (wait_cm_event(impl->ec, RDMA_CM_EVENT_ESTABLISHED, NULL) != 0) { DP_LOGE("rdma: 建链失败"); goto fail; }

    rdma_destroy_id(listen_id); /* 已经拿到子连接了，监听 id 不再需要 */

    /* 主动把接收缓冲区告诉 client */
    dp_slotinfo_t si;
    memset(&si, 0, sizeof(si));
    si.magic = htonl(DP_MAGIC);
    si.msg_type = htonl(DP_MSG_SLOTINFO);
    si.addr = htobe64((uint64_t)(uintptr_t)impl->recv_slots);
    si.rkey = htonl(impl->recv_slots_mr->rkey);
    si.slot_size = htonl(impl->chunk_size);
    si.num_slots = htonl(impl->window);
    if (ctrl_send(impl, &si, sizeof(si)) != 0) { DP_LOGE("rdma: 发送 slotinfo 失败"); goto fail; }

    DP_LOGI("rdma path 建链成功 (server), 已通告接收缓冲区 addr=0x%lx rkey=0x%x",
            (unsigned long)(uintptr_t)impl->recv_slots, impl->recv_slots_mr->rkey);
    return wrap(0, impl);

fail:
    /* 建链失败的清理没有做到完全严谨（例如 setup_qp_and_buffers 里已经注册
     * 的 MR/分配的 CQ 在这里没有逐一释放）——这条路径只在初始化阶段的极端
     * 情况下才会走到，且走到这里进程通常也要退出了，为了控制代码复杂度
     * 这里只做了最基本的清理，没有追求资源零泄漏。 */
    rdma_destroy_id(listen_id);
    free(impl);
    return NULL;
fail_listen:
    rdma_destroy_id(listen_id);
fail_early:
    free(impl);
    return NULL;
}

static dp_path_t *wrap(int id, rdma_impl_t *impl) {
    dp_path_t *p = (dp_path_t *)calloc(1, sizeof(dp_path_t));
    p->type = DP_TRANSPORT_RDMA;
    p->id = id;
    p->ops = dp_rdma_get_ops();
    p->impl = impl;
    return p;
}

/* ---------------------------------------------------------------------- */
/* path.h 接口实现                                                         */
/* ---------------------------------------------------------------------- */

static int rdma_send_hello(dp_path_t *p, const dp_hello_t *hello_in) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    dp_hello_t h = *hello_in;
    dp_hello_hton(&h);
    return ctrl_send(impl, &h, sizeof(h));
}

static int rdma_recv_hello(dp_path_t *p, dp_hello_t *hello) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    uint32_t len;
    if (ctrl_recv_wait(impl, hello, sizeof(*hello), &len) != 0 || len != sizeof(*hello)) return -1;
    dp_hello_ntoh(hello);
    if (hello->magic != DP_MAGIC || hello->msg_type != DP_MSG_HELLO) return -1;
    return 0;
}

static int rdma_send_hello_ack(dp_path_t *p, const dp_hello_ack_t *ack_in) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    dp_hello_ack_t a = *ack_in;
    dp_hello_ack_hton(&a);
    return ctrl_send(impl, &a, sizeof(a));
}

static int rdma_recv_hello_ack(dp_path_t *p, dp_hello_ack_t *ack) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    uint32_t len;
    if (ctrl_recv_wait(impl, ack, sizeof(*ack), &len) != 0 || len != sizeof(*ack)) return -1;
    dp_hello_ack_ntoh(ack);
    if (ack->magic != DP_MAGIC || ack->msg_type != DP_MSG_HELLO_ACK) return -1;
    return 0;
}

static int rdma_send_chunk(dp_path_t *p, uint32_t chunk_id, const void *data, uint32_t len) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    drain_send_cq(impl);
    if (impl->broken) return -1;

    uint32_t slot = impl->next_send_slot % impl->window;
    impl->next_send_slot++;
    memcpy(impl->local_slots + (size_t)slot * impl->chunk_size, data, len);

    struct ibv_sge sge = {
        .addr = (uintptr_t)(impl->local_slots + (size_t)slot * impl->chunk_size),
        .length = len,
        .lkey = impl->local_slots_mr->lkey,
    };
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.imm_data = htonl(DP_IMM_ENCODE(slot, chunk_id));
    wr.wr.rdma.remote_addr = impl->remote_addr + (uint64_t)slot * impl->chunk_size;
    wr.wr.rdma.rkey = impl->remote_rkey;

    struct ibv_send_wr *bad;
    if (ibv_post_send(impl->cm_id->qp, &wr, &bad) != 0) {
        impl->broken = 1;
        return -1;
    }
    return 0;
}

static int rdma_recv_chunk(dp_path_t *p, uint32_t *chunk_id, void *buf, uint32_t buf_cap, uint32_t *out_len) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    for (;;) {
        struct ibv_wc wc;
        int n;
        do {
            n = ibv_poll_cq(impl->recv_cq, 1, &wc);
            if (n < 0) return -1;
            if (n == 0) usleep(CQ_POLL_BACKOFF_US);
        } while (n == 0);
        if (wc.status != IBV_WC_SUCCESS) return -1;

        if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
            uint32_t encoded = ntohl(wc.imm_data);
            uint32_t slot = DP_IMM_SLOT(encoded);
            uint32_t cid = DP_IMM_CHUNK(encoded);

            uint64_t offset = (uint64_t)cid * impl->xfer_chunk_size;
            uint64_t remain = (offset < impl->file_size) ? (impl->file_size - offset) : 0;
            uint32_t len = (uint32_t)(remain < impl->xfer_chunk_size ? remain : impl->xfer_chunk_size);
            if (len > buf_cap) { DP_LOGE("rdma: 分片 %u 长度超出缓冲区", cid); return -1; }
            memcpy(buf, impl->recv_slots + (size_t)slot * impl->chunk_size, len);

            /* 重新 post 这个门铃 slot，让它可以接住后续写到同一个 slot 的下一次 WRITE */
            struct ibv_recv_wr wr = { .wr_id = wc.wr_id, .sg_list = NULL, .num_sge = 0 };
            struct ibv_recv_wr *bad;
            if (ibv_post_recv(impl->cm_id->qp, &wr, &bad) != 0) return -1;

            *chunk_id = cid;
            *out_len = len;
            return 0;
        } else if (wc.opcode == IBV_WC_RECV) {
            uint32_t idx = (uint32_t)wc.wr_id;
            dp_chunk_hdr_t hdr;
            if (wc.byte_len < sizeof(hdr)) { post_ctrl_recv(impl, idx); continue; }
            memcpy(&hdr, impl->ctrl_recv_buf + (size_t)idx * CTRL_MSG_SIZE, sizeof(hdr));
            post_ctrl_recv(impl, idx);
            dp_chunk_hdr_ntoh(&hdr);
            if (hdr.magic == DP_MAGIC && hdr.msg_type == DP_MSG_DONE) return 1;
            /* 其它意外的控制消息，忽略继续等 */
        }
    }
}

static int rdma_send_ack(dp_path_t *p, uint32_t chunk_id) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    dp_ack_hdr_t ack = { .magic = DP_MAGIC, .msg_type = DP_MSG_ACK, .chunk_id = chunk_id, .status = 0 };
    dp_ack_hdr_hton(&ack);
    return ctrl_send(impl, &ack, sizeof(ack));
}

static int rdma_recv_ack(dp_path_t *p, uint32_t *chunk_id, int *is_done) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    dp_ack_hdr_t ack;
    uint32_t len;
    if (ctrl_recv_wait(impl, &ack, sizeof(ack), &len) != 0 || len != sizeof(ack)) return -1;
    dp_ack_hdr_ntoh(&ack);
    if (ack.magic != DP_MAGIC) return -1;
    if (ack.msg_type == DP_MSG_ACK) { *chunk_id = ack.chunk_id; *is_done = 0; return 0; }
    if (ack.msg_type == DP_MSG_BYE) { *is_done = 1; return 0; }
    return -1;
}

static int rdma_send_done(dp_path_t *p) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    dp_chunk_hdr_t hdr = { .magic = DP_MAGIC, .msg_type = DP_MSG_DONE, .chunk_id = 0, .length = 0 };
    dp_chunk_hdr_hton(&hdr);
    return ctrl_send(impl, &hdr, sizeof(hdr));
}

static void rdma_set_transfer_meta(dp_path_t *p, uint64_t file_size, uint32_t chunk_size) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    impl->file_size = file_size;
    impl->xfer_chunk_size = chunk_size;
}

static void rdma_close(dp_path_t *p) {
    if (!p) return;
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    if (impl) {
        if (impl->cm_id && impl->cm_id->qp && !impl->broken) {
            dp_ack_hdr_t bye = { .magic = DP_MAGIC, .msg_type = DP_MSG_BYE, .chunk_id = 0, .status = 0 };
            dp_ack_hdr_hton(&bye);
            (void)ctrl_send(impl, &bye, sizeof(bye)); /* 尽力而为，失败就算了 */
        }
        if (impl->cm_id) {
            if (impl->cm_id->qp) rdma_destroy_qp(impl->cm_id);
        }
        if (impl->local_slots_mr) ibv_dereg_mr(impl->local_slots_mr);
        if (impl->recv_slots_mr) ibv_dereg_mr(impl->recv_slots_mr);
        if (impl->ctrl_send_mr) ibv_dereg_mr(impl->ctrl_send_mr);
        if (impl->ctrl_recv_mr) ibv_dereg_mr(impl->ctrl_recv_mr);
        free(impl->local_slots);
        free(impl->recv_slots);
        free(impl->ctrl_send_buf);
        free(impl->ctrl_recv_buf);
        if (impl->send_cq) ibv_destroy_cq(impl->send_cq);
        if (impl->recv_cq) ibv_destroy_cq(impl->recv_cq);
        if (impl->pd) ibv_dealloc_pd(impl->pd);
        if (impl->cm_id) rdma_destroy_id(impl->cm_id);
        if (impl->ec) rdma_destroy_event_channel(impl->ec);
        free(impl);
    }
    free(p);
}

static const dp_path_ops_t g_rdma_ops = {
    .name = "rdma",
    .client_connect = rdma_client_connect,
    .server_accept_one = rdma_server_accept_one,
    .send_hello = rdma_send_hello,
    .recv_hello = rdma_recv_hello,
    .send_hello_ack = rdma_send_hello_ack,
    .recv_hello_ack = rdma_recv_hello_ack,
    .send_chunk = rdma_send_chunk,
    .recv_chunk = rdma_recv_chunk,
    .send_ack = rdma_send_ack,
    .recv_ack = rdma_recv_ack,
    .send_done = rdma_send_done,
    .set_transfer_meta = rdma_set_transfer_meta,
    .close = rdma_close,
};

const dp_path_ops_t *dp_rdma_get_ops(void) { return &g_rdma_ops; }

#endif /* DP_ENABLE_RDMA */
