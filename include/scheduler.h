/*
 * scheduler.h - 双网卡（多路径）分片调度与故障转移算法
 *
 * 这是整个项目的“大脑”：与传输层（TCP/RDMA）完全解耦，只负责回答
 * 两个问题：
 *   1) 下一个待发送的分片应该走哪条路径？ (加权轮询 / 最小虚拟完成时间)
 *   2) 某条路径挂掉之后，它身上未完成的分片应该如何重新分配？ (故障转移)
 *
 * 采用类似 WFQ (Weighted Fair Queueing) 的“虚拟进度”思想：
 * 每条路径记录 vprogress = bytes_assigned / weight，调度时总是选择
 * vprogress 最小、且仍然存活(UP)、且还有可用 credit 的路径，从而使
 * 各路径按权重（例如网卡带宽比例）分摊数据量，实现负载均衡。
 *
 * 线程安全：所有对外接口内部使用互斥锁保护，可以从多个发送线程并发调用。
 */
#ifndef DUALPATH_SCHEDULER_H
#define DUALPATH_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define SCHED_MAX_PATHS 8

typedef enum { PATH_UP = 0, PATH_DOWN = 1 } path_state_t;

typedef struct {
    int            id;
    double         weight;        /* 相对权重，例如两张网卡带宽比 25G:10G -> 2.5:1 */
    double         vprogress;     /* 虚拟进度 = bytes_assigned / weight，越小越优先 */
    path_state_t   state;
    uint32_t       credit;        /* 当前可用发送窗口（未确认分片数上限） */
    uint32_t       max_credit;
    uint32_t       inflight;      /* 当前在途未 ACK 的分片数 */
    uint64_t       bytes_assigned;
    uint64_t       bytes_acked;
    uint64_t       chunks_acked;
    uint64_t       chunks_failed_over; /* 因该路径故障被转移走的分片数（统计用） */
} sched_path_t;

typedef struct {
    pthread_mutex_t lock;
    sched_path_t    paths[SCHED_MAX_PATHS];
    int             num_paths;

    uint32_t        total_chunks;
    uint32_t        next_chunk_id;     /* 尚未分配过的最小 chunk_id（初始分配游标） */
    uint32_t        chunks_done;       /* 已经被任意路径 ACK 的分片数 */

    /* 重传/故障转移队列：一个简单的定长环形缓冲区就够用（chunk 数量有限且是 uint32） */
    uint32_t       *retry_queue;
    int             retry_head, retry_tail, retry_cap, retry_count;

    /* 记录每个 chunk 当前分配在哪条路径上，用于故障转移时批量回收 */
    int8_t         *chunk_owner;   /* chunk_owner[chunk_id] = path idx，-1 表示未分配/已完成 */
} scheduler_t;

/* 初始化调度器。weights 可以为 NULL，表示所有路径权重相等 (=1.0) */
int  scheduler_init(scheduler_t *s, int num_paths, const double *weights,
                     uint32_t total_chunks, uint32_t window_per_path);
void scheduler_destroy(scheduler_t *s);

/*
 * 尝试为“下一个待发送的分片”挑选一条路径。
 * 成功：返回 0，*out_path = 路径下标，*out_chunk = 分配到的 chunk_id
 * 失败（所有存活路径都没有可用 credit，或已经没有分片可分配）：返回 -1
 */
int scheduler_acquire(scheduler_t *s, int *out_path, uint32_t *out_chunk, uint32_t chunk_bytes);

/* 某分片被对端 ACK：释放该路径的一个 credit，更新统计 */
void scheduler_on_ack(scheduler_t *s, int path_idx, uint32_t chunk_id, uint32_t chunk_bytes);

/* 标记路径故障：将其名下所有未完成（含在途未ACK）的分片放回重传队列，供其它路径领取 */
void scheduler_mark_down(scheduler_t *s, int path_idx);

/* 是否所有分片都已经被 ACK */
bool scheduler_all_done(scheduler_t *s);

/* 是否还存在至少一条存活路径 */
bool scheduler_has_live_path(scheduler_t *s);

void scheduler_snapshot(scheduler_t *s, sched_path_t *out, int max_paths, int *out_n);

#endif /* DUALPATH_SCHEDULER_H */
