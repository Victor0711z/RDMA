/*
 * rdma_path.c - path.h 接口的 RDMA Verbs 实现（libibverbs + rdma_cm）
 *
 * ============================== 设计说明 ==============================
 *
 * 一条 path 对应一条 RC (Reliable Connection) Queue Pair，线路上有两类流量：
 *
 *   1) 控制消息（HELLO / HELLO_ACK / ACK / DONE / BYE / 内部的 slot 元信息）
 *      —— 用标准的"双边"操作 SEND/RECV 实现，跟 socket send/recv 心智模型
 *      使用 protocol.h 中的控制结构体和统一网络字节序
 *      转换函数。
 *
 *   2) 分片数据本体 —— 用 RDMA WRITE + Immediate Data 实现"单边"零拷贝写：
 *      - 连接建立后，server（接收方）注册一块大内存 recv_slots（window 个
 *        slot，每个 slot 能装一个分片），把这块内存的 (addr, rkey) 通过一条
 *        控制消息告诉 client；
 *      - client（发送方）把分片数据 RDMA WRITE 直接写到 server 那块内存里
 *        对应的 slot 上；immediate data 只携带 slot 编号，完整 chunk 元数据
 *        （chunk_id/length/CRC32C）位于 slot header；
 *        server 端不需要 CPU 参与数据搬运（数据由网卡直接 DMA 到目标内存），
 *        只需要有一个预先 post 好的 RECV 来"被通知"数据到了；
 *      - server 校验 slot header、期望长度和 CRC32C 后才写文件并回 ACK。
 *
 * slot 复用：每条 path 上，同一时刻最多有 window 个分片在途未 ACK（这是
 * scheduler.c 的 credit 机制保证的），所以 client 侧用一个简单的自增计数器
 * client 显式记录每个 slot 当前属于哪个 chunk；只有收到该 chunk 的 ACK 才释放
 * slot。这样即使 ACK 乱序，也不会用新 WRITE 覆盖仍在处理的远端 slot。
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
#include <stdatomic.h>
#include <pthread.h>

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
    _Atomic int broken;      /* sender/ACK 线程都会访问 */

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
    uint32_t         remote_slot_size;
    uint32_t         remote_num_slots;
    uint64_t         next_send_wr_id;
    pthread_mutex_t  tx_lock;     /* send CQ、WR id、控制发送缓冲区 */
    int              tx_lock_initialized;
    pthread_mutex_t  slot_lock;
    int              slot_lock_initialized;
    uint8_t         *slot_in_use;
    uint32_t        *slot_chunk;

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

static size_t local_slot_size(const rdma_impl_t *impl) {
    return sizeof(dp_rdma_slot_hdr_t) + (size_t)impl->chunk_size;
}

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

/* 同一个 send CQ 同时承载数据 WRITE 和控制 SEND，必须等到自己的 wr_id，不能把
 * 更早的数据 completion 误当成当前控制消息已经完成。 */
static int poll_send_wr(rdma_impl_t *impl, uint64_t target_wr_id) {
    for (;;) {
        struct ibv_wc wc;
        if (poll_cq_blocking(impl->send_cq, &wc) != 0) {
            impl->broken = 1;
            return -1;
        }
        if (wc.wr_id == target_wr_id) return 0;
    }
}

/* 非阻塞地把 send_cq 里已经完成的 WR 都收走，主要用来及时发现错误、避免 CQ 堆积 */
static void drain_send_cq(rdma_impl_t *impl) {
    struct ibv_wc wc;
    for (;;) {
        int n = ibv_poll_cq(impl->send_cq, 1, &wc);
        if (n < 0) { impl->broken = 1; break; }
        if (n == 0) break;
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
    pthread_mutex_lock(&impl->tx_lock);
    if (impl->broken) {
        pthread_mutex_unlock(&impl->tx_lock);
        return -1;
    }
    memcpy(impl->ctrl_send_buf, data, len);
    struct ibv_sge sge = { .addr = (uintptr_t)impl->ctrl_send_buf, .length = len, .lkey = impl->ctrl_send_mr->lkey };
    uint64_t wr_id = ++impl->next_send_wr_id;
    struct ibv_send_wr wr = {
        .wr_id = wr_id, .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED,
        .sg_list = &sge, .num_sge = 1,
    };
    struct ibv_send_wr *bad;
    if (ibv_post_send(impl->cm_id->qp, &wr, &bad) != 0) {
        impl->broken = 1;
        pthread_mutex_unlock(&impl->tx_lock);
        return -1;
    }
    if (poll_send_wr(impl, wr_id) != 0) {
        pthread_mutex_unlock(&impl->tx_lock);
        return -1;
    }
    pthread_mutex_unlock(&impl->tx_lock);
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
            if (n == 0) {
                if (impl->broken) return -1;
                usleep(CQ_POLL_BACKOFF_US);
            }
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

static void destroy_impl(rdma_impl_t *impl, int graceful) {
    if (!impl) return;
    if (graceful && impl->is_server && impl->cm_id && impl->cm_id->qp && !impl->broken) {
        dp_ack_hdr_t bye = { .magic = DP_MAGIC, .msg_type = DP_MSG_BYE, .chunk_id = 0, .status = 0 };
        dp_ack_hdr_hton(&bye);
        (void)ctrl_send(impl, &bye, sizeof(bye));
    }
    if (impl->cm_id && impl->cm_id->qp) rdma_destroy_qp(impl->cm_id);
    if (impl->local_slots_mr) ibv_dereg_mr(impl->local_slots_mr);
    if (impl->recv_slots_mr) ibv_dereg_mr(impl->recv_slots_mr);
    if (impl->ctrl_send_mr) ibv_dereg_mr(impl->ctrl_send_mr);
    if (impl->ctrl_recv_mr) ibv_dereg_mr(impl->ctrl_recv_mr);
    free(impl->local_slots);
    free(impl->slot_in_use);
    free(impl->slot_chunk);
    free(impl->recv_slots);
    free(impl->ctrl_send_buf);
    free(impl->ctrl_recv_buf);
    if (impl->send_cq) ibv_destroy_cq(impl->send_cq);
    if (impl->recv_cq) ibv_destroy_cq(impl->recv_cq);
    if (impl->pd) ibv_dealloc_pd(impl->pd);
    if (impl->cm_id) rdma_destroy_id(impl->cm_id);
    if (impl->ec) rdma_destroy_event_channel(impl->ec);
    if (impl->slot_lock_initialized) pthread_mutex_destroy(&impl->slot_lock);
    if (impl->tx_lock_initialized) pthread_mutex_destroy(&impl->tx_lock);
    free(impl);
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
    if (pthread_mutex_init(&impl->tx_lock, NULL) != 0) {
        DP_LOGE("rdma: 初始化发送锁失败");
        return -1;
    }
    impl->tx_lock_initialized = 1;

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
    impl->ctrl_send_buf = (uint8_t *)malloc(CTRL_MSG_SIZE);
    if (!impl->ctrl_recv_buf || !impl->ctrl_send_buf) {
        DP_LOGE("rdma: 分配控制消息缓冲区失败");
        return -1;
    }
    impl->ctrl_recv_mr = ibv_reg_mr(impl->pd, impl->ctrl_recv_buf,
                                     (size_t)impl->ctrl_recv_depth * CTRL_MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
    impl->ctrl_send_mr = ibv_reg_mr(impl->pd, impl->ctrl_send_buf, CTRL_MSG_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!impl->ctrl_recv_mr || !impl->ctrl_send_mr) { DP_LOGE("rdma: 注册控制消息 MR 失败"); return -1; }

    for (uint32_t i = 0; i < impl->ctrl_recv_depth; i++) {
        if (post_ctrl_recv(impl, i) != 0) { DP_LOGE("rdma: post 初始 ctrl recv 失败"); return -1; }
    }
    impl->ctrl_recv_next = 0;

    if (impl->is_server) {
        size_t total = (size_t)impl->window * local_slot_size(impl);
        impl->recv_slots = (uint8_t *)malloc(total);
        if (!impl->recv_slots) { DP_LOGE("rdma: 分配 recv_slots 失败"); return -1; }
        impl->recv_slots_mr = ibv_reg_mr(impl->pd, impl->recv_slots, total,
                                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!impl->recv_slots_mr) {
            DP_LOGE("rdma: 注册 recv_slots MR 失败 (errno=%d: %s)", errno, strerror(errno));
            DP_LOGE("rdma: 如果是在阿里云 eRDMA 等云厂商 RDMA 网卡上，部分实现对内存注册"
                    "大小有下限/上限要求，可以尝试调大 config.ini 里的 window/chunk_size"
                    "（当前 window=%u chunk_size=%u，共 %zu 字节）", impl->window, impl->chunk_size, total);
            return -1;
        }
        /* WRITE_WITH_IMM 和 SEND 会从同一个 RQ 按序消费 WQE，不能混放零长度“门铃”
         * 与带缓冲区的控制 WQE，否则 DONE 可能命中零长度 WQE 并触发 LOC_LEN_ERR。
         * 上面的同构控制 WQE 同时承接两种 completion；WRITE 本体仍写 recv_slots。 */
    } else {
        size_t total = (size_t)impl->window * local_slot_size(impl);
        impl->local_slots = (uint8_t *)malloc(total);
        if (!impl->local_slots) { DP_LOGE("rdma: 分配 local_slots 失败"); return -1; }
        impl->local_slots_mr = ibv_reg_mr(impl->pd, impl->local_slots, total, IBV_ACCESS_LOCAL_WRITE);
        if (!impl->local_slots_mr) {
            DP_LOGE("rdma: 注册 local_slots MR 失败 (errno=%d: %s)", errno, strerror(errno));
            DP_LOGE("rdma: 如果是在阿里云 eRDMA 等云厂商 RDMA 网卡上，部分实现对内存注册"
                    "大小有下限/上限要求，可以尝试调大 config.ini 里的 window/chunk_size"
                    "（当前 window=%u chunk_size=%u，共 %zu 字节）", impl->window, impl->chunk_size, total);
            return -1;
        }
        impl->slot_in_use = (uint8_t *)calloc(impl->window, 1);
        impl->slot_chunk = (uint32_t *)calloc(impl->window, sizeof(uint32_t));
        if (!impl->slot_in_use || !impl->slot_chunk || pthread_mutex_init(&impl->slot_lock, NULL) != 0) {
            DP_LOGE("rdma: 初始化发送 slot 所有权表失败");
            return -1;
        }
        impl->slot_lock_initialized = 1;
    }
    return 0;
}

static dp_path_t *wrap(int id, rdma_impl_t *impl);

static dp_path_t *rdma_client_connect(const dp_path_cfg_t *cfg) {
    rdma_impl_t *impl = (rdma_impl_t *)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
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
    if (cfg->local_ip && cfg->local_ip[0] &&
        inet_pton(AF_INET, cfg->local_ip, &local_addr.sin_addr) != 1) {
        DP_LOGE("rdma: invalid local_ip '%s'", cfg->local_ip);
        goto fail;
    }
    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->remote_ip, &remote_addr.sin_addr) != 1) {
        DP_LOGE("rdma: invalid remote_ip '%s'", cfg->remote_ip);
        goto fail;
    }

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
    impl->remote_slot_size = ntohl(si->slot_size);
    uint32_t peer_slots = ntohl(si->num_slots);
    if (impl->remote_slot_size < local_slot_size(impl) || peer_slots == 0 || peer_slots > 256) {
        DP_LOGE("rdma: 对端 slot 参数不兼容 (slot_size=%u num_slots=%u，本地需要=%zu)",
                impl->remote_slot_size, peer_slots, local_slot_size(impl));
        goto fail;
    }
    impl->remote_num_slots = peer_slots < impl->window ? peer_slots : impl->window;

    DP_LOGI("rdma path 建链成功 (client): local=%s -> remote=%s:%d, remote slot addr=0x%lx rkey=0x%x",
            cfg->local_ip ? cfg->local_ip : "(any)", cfg->remote_ip, cfg->port,
            (unsigned long)impl->remote_addr, impl->remote_rkey);
    return wrap(0, impl);

fail:
    destroy_impl(impl, 0);
    return NULL;
}

static dp_path_t *rdma_server_accept_one(const dp_path_cfg_t *cfg) {
    rdma_impl_t *impl = (rdma_impl_t *)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
    impl->window = cfg->window_size;
    impl->chunk_size = cfg->chunk_size;
    impl->ctrl_recv_depth = cfg->window_size + 16;
    impl->is_server = 1;

    impl->ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id = NULL;
    if (!impl->ec || rdma_create_id(impl->ec, &listen_id, NULL, RDMA_PS_TCP) != 0) {
        DP_LOGE("rdma: rdma_create_id (listen) 失败"); goto fail_early;
    }

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)cfg->port);
    const char *bind_ip = (cfg->local_ip && cfg->local_ip[0]) ? cfg->local_ip : "0.0.0.0";
    if (inet_pton(AF_INET, bind_ip, &bind_addr.sin_addr) != 1) {
        DP_LOGE("rdma: invalid bind ip '%s'", bind_ip);
        goto fail_listen;
    }

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
    listen_id = NULL;

    /* 主动把接收缓冲区告诉 client */
    dp_slotinfo_t si;
    memset(&si, 0, sizeof(si));
    si.magic = htonl(DP_MAGIC);
    si.msg_type = htonl(DP_MSG_SLOTINFO);
    si.addr = htobe64((uint64_t)(uintptr_t)impl->recv_slots);
    si.rkey = htonl(impl->recv_slots_mr->rkey);
    si.slot_size = htonl((uint32_t)local_slot_size(impl));
    si.num_slots = htonl(impl->window);
    if (ctrl_send(impl, &si, sizeof(si)) != 0) { DP_LOGE("rdma: 发送 slotinfo 失败"); goto fail; }

    DP_LOGI("rdma path 建链成功 (server), 已通告接收缓冲区 addr=0x%lx rkey=0x%x",
            (unsigned long)(uintptr_t)impl->recv_slots, impl->recv_slots_mr->rkey);
    return wrap(0, impl);

fail:
    if (listen_id) rdma_destroy_id(listen_id);
    destroy_impl(impl, 0);
    return NULL;
fail_listen:
    if (listen_id) rdma_destroy_id(listen_id);
fail_early:
    destroy_impl(impl, 0);
    return NULL;
}

static dp_path_t *wrap(int id, rdma_impl_t *impl) {
    dp_path_t *p = (dp_path_t *)calloc(1, sizeof(dp_path_t));
    if (!p) {
        destroy_impl(impl, 0);
        return NULL;
    }
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

static int rdma_acquire_chunk_buffer(dp_path_t *p, uint32_t chunk_id, uint32_t len,
                                     void **payload) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    if (!payload || impl->is_server || !impl->slot_lock_initialized) return -1;
    if (impl->broken) return -1;
    if (len > impl->chunk_size || impl->remote_num_slots == 0) return -1;

    uint32_t slot = impl->remote_num_slots;
    pthread_mutex_lock(&impl->slot_lock);
    for (uint32_t i = 0; i < impl->remote_num_slots; i++) {
        if (!impl->slot_in_use[i]) {
            slot = i;
            impl->slot_in_use[i] = 1;
            impl->slot_chunk[i] = chunk_id;
            break;
        }
    }
    pthread_mutex_unlock(&impl->slot_lock);
    if (slot == impl->remote_num_slots) {
        DP_LOGE("rdma: QP credit 与 slot 状态不一致（没有可用 slot）");
        return -1;
    }
    uint8_t *slot_base = impl->local_slots + (size_t)slot * local_slot_size(impl);
    *payload = slot_base + sizeof(dp_rdma_slot_hdr_t);
    return 0;
}

static void rdma_release_chunk_buffer(dp_path_t *p, uint32_t chunk_id) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    if (impl->is_server || !impl->slot_lock_initialized) return;
    pthread_mutex_lock(&impl->slot_lock);
    for (uint32_t i = 0; i < impl->remote_num_slots; i++) {
        if (impl->slot_in_use[i] && impl->slot_chunk[i] == chunk_id) {
            impl->slot_in_use[i] = 0;
            break;
        }
    }
    pthread_mutex_unlock(&impl->slot_lock);
}

static int rdma_commit_chunk(dp_path_t *p, uint32_t chunk_id, uint32_t len) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    if (impl->is_server || !impl->slot_lock_initialized || len > impl->chunk_size) {
        rdma_release_chunk_buffer(p, chunk_id);
        return -1;
    }

    uint32_t slot = impl->remote_num_slots;
    pthread_mutex_lock(&impl->slot_lock);
    for (uint32_t i = 0; i < impl->remote_num_slots; i++) {
        if (impl->slot_in_use[i] && impl->slot_chunk[i] == chunk_id) {
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&impl->slot_lock);
    if (slot == impl->remote_num_slots) return -1;

    uint8_t *slot_base = impl->local_slots + (size_t)slot * local_slot_size(impl);
    const uint8_t *payload = slot_base + sizeof(dp_rdma_slot_hdr_t);
    dp_rdma_slot_hdr_t slot_hdr = {
        .magic = htonl(DP_MAGIC),
        .chunk_id = htonl(chunk_id),
        .length = htonl(len),
        .crc32c = htonl(dp_crc32c(payload, len)),
    };
    memcpy(slot_base, &slot_hdr, sizeof(slot_hdr));

    struct ibv_sge sge = {
        .addr = (uintptr_t)slot_base,
        .length = (uint32_t)(sizeof(slot_hdr) + len),
        .lkey = impl->local_slots_mr->lkey,
    };
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.imm_data = htonl(slot);
    wr.wr.rdma.remote_addr = impl->remote_addr + (uint64_t)slot * impl->remote_slot_size;
    wr.wr.rdma.rkey = impl->remote_rkey;

    pthread_mutex_lock(&impl->tx_lock);
    drain_send_cq(impl);
    if (impl->broken) {
        pthread_mutex_unlock(&impl->tx_lock);
        rdma_release_chunk_buffer(p, chunk_id);
        return -1;
    }
    wr.wr_id = ++impl->next_send_wr_id;
    struct ibv_send_wr *bad;
    if (ibv_post_send(impl->cm_id->qp, &wr, &bad) != 0) {
        impl->broken = 1;
        pthread_mutex_unlock(&impl->tx_lock);
        rdma_release_chunk_buffer(p, chunk_id);
        return -1;
    }
    pthread_mutex_unlock(&impl->tx_lock);
    return 0;
}

static int rdma_send_chunk(dp_path_t *p, uint32_t chunk_id, const void *data, uint32_t len) {
    void *payload;
    if (rdma_acquire_chunk_buffer(p, chunk_id, len, &payload) != 0) return -1;
    memcpy(payload, data, len);
    return rdma_commit_chunk(p, chunk_id, len);
}

static int rdma_recv_chunk(dp_path_t *p, uint32_t *chunk_id, const void **data,
                           uint32_t *out_len) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    for (;;) {
        struct ibv_wc wc;
        int n;
        do {
            n = ibv_poll_cq(impl->recv_cq, 1, &wc);
            if (n < 0) return -1;
            if (n == 0) {
                if (impl->broken) return -1;
                usleep(CQ_POLL_BACKOFF_US);
            }
        } while (n == 0);
        if (wc.status != IBV_WC_SUCCESS) return -1;

        if (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
            uint32_t slot = ntohl(wc.imm_data);
            if (slot >= impl->window || impl->xfer_chunk_size == 0) {
                DP_LOGE("rdma: 非法 slot 通知 slot=%u", slot);
                return -1;
            }

            uint8_t *slot_base = impl->recv_slots + (size_t)slot * local_slot_size(impl);
            dp_rdma_slot_hdr_t slot_hdr;
            memcpy(&slot_hdr, slot_base, sizeof(slot_hdr));
            uint32_t magic = ntohl(slot_hdr.magic);
            uint32_t cid = ntohl(slot_hdr.chunk_id);
            uint32_t len = ntohl(slot_hdr.length);
            uint32_t expected_crc = ntohl(slot_hdr.crc32c);
            uint64_t total_chunks = impl->file_size / impl->xfer_chunk_size +
                (impl->file_size % impl->xfer_chunk_size != 0);
            if (magic != DP_MAGIC || cid >= total_chunks || len > impl->xfer_chunk_size) {
                DP_LOGE("rdma: 非法 slot header slot=%u chunk=%u len=%u", slot, cid, len);
                return -1;
            }

            uint64_t offset = (uint64_t)cid * impl->xfer_chunk_size;
            uint64_t remain = (offset < impl->file_size) ? (impl->file_size - offset) : 0;
            uint32_t expected_len = (uint32_t)(remain < impl->xfer_chunk_size ? remain : impl->xfer_chunk_size);
            if (len != expected_len) {
                DP_LOGE("rdma: 分片 %u 长度不匹配 (%u != %u)", cid, len, expected_len);
                return -1;
            }
            const uint8_t *payload = slot_base + sizeof(slot_hdr);
            uint32_t actual_crc = dp_crc32c(payload, len);
            if (actual_crc != expected_crc) {
                DP_LOGE("rdma: 分片 %u CRC32C 错误 (%08x != %08x)", cid, actual_crc, expected_crc);
                return -1;
            }
            /* SEND 与 WRITE_WITH_IMM 共用 RQ；始终补回同构、带缓冲区的 WQE。 */
            if (post_ctrl_recv(impl, (uint32_t)wc.wr_id) != 0) return -1;

            *chunk_id = cid;
            *data = payload;
            *out_len = len;
            return 0;
        } else if (wc.opcode == IBV_WC_RECV) {
            uint32_t idx = (uint32_t)wc.wr_id;
            dp_done_hdr_t hdr;
            if (wc.byte_len < sizeof(hdr)) { post_ctrl_recv(impl, idx); continue; }
            memcpy(&hdr, impl->ctrl_recv_buf + (size_t)idx * CTRL_MSG_SIZE, sizeof(hdr));
            post_ctrl_recv(impl, idx);
            dp_done_hdr_ntoh(&hdr);
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
    if (ack.msg_type == DP_MSG_ACK) {
        pthread_mutex_lock(&impl->slot_lock);
        for (uint32_t i = 0; i < impl->remote_num_slots; i++) {
            if (impl->slot_in_use[i] && impl->slot_chunk[i] == ack.chunk_id) {
                impl->slot_in_use[i] = 0;
                break;
            }
        }
        pthread_mutex_unlock(&impl->slot_lock);
        *chunk_id = ack.chunk_id;
        *is_done = 0;
        return 0;
    }
    if (ack.msg_type == DP_MSG_BYE) { *is_done = 1; return 0; }
    return -1;
}

static int rdma_send_done(dp_path_t *p) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    dp_done_hdr_t hdr = { .magic = DP_MAGIC, .msg_type = DP_MSG_DONE };
    dp_done_hdr_hton(&hdr);
    return ctrl_send(impl, &hdr, sizeof(hdr));
}

static void rdma_set_transfer_meta(dp_path_t *p, uint64_t file_size, uint32_t chunk_size) {
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    impl->file_size = file_size;
    impl->xfer_chunk_size = chunk_size;
}

static void rdma_shutdown(dp_path_t *p) {
    if (!p || !p->impl) return;
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    impl->broken = 1;
    if (impl->tx_lock_initialized) pthread_mutex_lock(&impl->tx_lock);
    if (impl->cm_id) (void)rdma_disconnect(impl->cm_id);
    if (impl->tx_lock_initialized) pthread_mutex_unlock(&impl->tx_lock);
}

static void rdma_close(dp_path_t *p) {
    if (!p) return;
    rdma_impl_t *impl = (rdma_impl_t *)p->impl;
    destroy_impl(impl, 1);
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
    .acquire_chunk_buffer = rdma_acquire_chunk_buffer,
    .commit_chunk = rdma_commit_chunk,
    .release_chunk_buffer = rdma_release_chunk_buffer,
    .recv_chunk = rdma_recv_chunk,
    .send_ack = rdma_send_ack,
    .recv_ack = rdma_recv_ack,
    .send_done = rdma_send_done,
    .set_transfer_meta = rdma_set_transfer_meta,
    .shutdown = rdma_shutdown,
    .close = rdma_close,
};

const dp_path_ops_t *dp_rdma_get_ops(void) { return &g_rdma_ops; }
