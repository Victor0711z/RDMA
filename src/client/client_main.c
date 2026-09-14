/*
 * client_main.c - MultiQP-RDMA 客户端
 *
 * 整体流程：
 *   1) 在同一张 RDMA 网卡上并发建立 N 个独立 RC QP
 *   2) 在控制 QP (QP0) 上做一次 HELLO/HELLO_ACK 握手，告知服务端
 *      文件名/大小/分片大小/总分片数
 *   3) 初始化调度器 (scheduler_t)，启动：
 *        - 1 个发送线程：不断向调度器申请“下一个 chunk 及目标 QP”并发送
 *        - N 个 ACK 接收线程（每个 QP 一个）：接收确认并释放 credit/slot；
 *          一旦某个 QP 读写出错，就把它标记为 DOWN，调度器会自动把它名下
 *          未完成的分片转移给存活 QP
 *          —— 这就是"故障转移"
 *   4) 所有分片被 ACK 后，向每个存活 QP 发送 DONE，打印聚合吞吐报告
 */
#include "path.h"
#include "config.h"
#include "scheduler.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <time.h>

typedef struct {
    int idx;
    dp_path_cfg_t cfg;
    dp_path_t *result;
} connect_job_t;

static void *connect_job_fn(void *arg) {
    connect_job_t *job = (connect_job_t *)arg;
    job->result = dp_path_create_client(job->idx, &job->cfg);
    return NULL;
}

typedef struct {
    dp_path_t **paths;
    scheduler_t *sched;
    int num_paths;
    int file_fd;
    uint32_t chunk_size;
    uint64_t file_size;
    pthread_mutex_t *fail_lock;
    int *failed;
    int inject_qp;
    uint32_t inject_after;
    uint64_t sends_posted;
    int injection_done;
} sender_ctx_t;

typedef struct {
    dp_path_t *path;
    int idx;
    scheduler_t *sched;
    uint32_t chunk_size;
    uint64_t file_size;
    pthread_mutex_t *fail_lock;
    int *failed;
} ack_ctx_t;

static void mark_failed(dp_path_t *path, scheduler_t *s, pthread_mutex_t *lock,
                        int *failed, int idx, const char *reason) {
    pthread_mutex_lock(lock);
    if (!failed[idx]) {
        failed[idx] = 1;
        scheduler_mark_down(s, idx);
        dp_path_shutdown(path); /* 唤醒同一 QP 上可能阻塞的另一个 I/O 线程 */
        DP_LOGW("*** QP %d 故障 (%s)，未完成分片将自动转移到其它 QP ***", idx, reason);
    }
    pthread_mutex_unlock(lock);
}

static void *sender_thread_fn(void *arg) {
    sender_ctx_t *ctx = (sender_ctx_t *)arg;
    uint8_t *buf = (uint8_t *)malloc(ctx->chunk_size);
    if (!buf) {
        DP_LOGE("sender: out of memory");
        for (int i = 0; i < ctx->num_paths; i++) {
            mark_failed(ctx->paths[i], ctx->sched, ctx->fail_lock, ctx->failed, i,
                        "sender out of memory");
        }
        return NULL;
    }

    for (;;) {
        if (scheduler_all_done(ctx->sched)) break;
        if (!scheduler_has_live_path(ctx->sched)) {
            DP_LOGE("sender: 所有 QP 均已故障，传输无法继续");
            break;
        }
        int path_idx;
        uint32_t chunk_id;
        if (scheduler_acquire(ctx->sched, &path_idx, &chunk_id, ctx->chunk_size) != 0) {
            /* 窗口满或所有 chunk 已在途时由 ACK/QP 状态变化唤醒，避免 1ms 忙等。 */
            (void)scheduler_wait_for_work(ctx->sched, 1000);
            continue;
        }

        if (!ctx->injection_done && ctx->inject_qp == path_idx &&
            ctx->sends_posted >= ctx->inject_after) {
            ctx->injection_done = 1;
            DP_LOGW("故障注入：第 %llu 次成功发送后主动关闭 QP %d",
                    (unsigned long long)ctx->sends_posted, path_idx);
            mark_failed(ctx->paths[path_idx], ctx->sched, ctx->fail_lock, ctx->failed,
                        path_idx, "requested fault injection");
            continue;
        }

        uint64_t offset = (uint64_t)chunk_id * ctx->chunk_size;
        uint64_t remain = ctx->file_size - offset;
        uint32_t len = (uint32_t)(remain < ctx->chunk_size ? remain : ctx->chunk_size);

        ssize_t n = pread(ctx->file_fd, buf, len, (off_t)offset);
        if (n != (ssize_t)len) {
            DP_LOGE("sender: 读文件第 %u 片失败 (offset=%llu len=%u)", chunk_id,
                    (unsigned long long)offset, len);
            /* 本地 I/O 是会话级错误，换 QP 重试没有意义，终止所有 QP。 */
            for (int i = 0; i < ctx->num_paths; i++) {
                mark_failed(ctx->paths[i], ctx->sched, ctx->fail_lock, ctx->failed, i,
                            "local read error");
            }
            break;
        }

        if (dp_path_send_chunk(ctx->paths[path_idx], chunk_id, buf, len) != 0) {
            mark_failed(ctx->paths[path_idx], ctx->sched, ctx->fail_lock, ctx->failed,
                        path_idx, "send error");
        } else {
            ctx->sends_posted++;
        }
    }

    free(buf);
    return NULL;
}

static void *ack_thread_fn(void *arg) {
    ack_ctx_t *ctx = (ack_ctx_t *)arg;
    for (;;) {
        uint32_t chunk_id;
        int is_done = 0;
        int rc = dp_path_recv_ack(ctx->path, &chunk_id, &is_done);
        if (rc < 0) {
            mark_failed(ctx->path, ctx->sched, ctx->fail_lock, ctx->failed, ctx->idx, "recv error");
            break;
        }
        if (rc == 1) {
            mark_failed(ctx->path, ctx->sched, ctx->fail_lock, ctx->failed, ctx->idx,
                        "connection closed unexpectedly");
            break;
        }
        if (is_done) {
            DP_LOGD("QP %d: 对端正常关闭 (BYE)", ctx->idx);
            break;
        }
        uint64_t offset = (uint64_t)chunk_id * ctx->chunk_size;
        uint64_t remain = offset < ctx->file_size ? ctx->file_size - offset : 0;
        uint32_t actual = (uint32_t)(remain < ctx->chunk_size ? remain : ctx->chunk_size);
        scheduler_on_ack(ctx->sched, ctx->idx, chunk_id, actual);
    }
    return NULL;
}

static void print_progress(scheduler_t *s, int num_paths, double t0) {
    sched_path_t snap[SCHED_MAX_PATHS];
    int n;
    scheduler_snapshot(s, snap, SCHED_MAX_PATHS, &n);

    uint64_t total_acked = 0;
    char line[512];
    int off = 0;
    for (int i = 0; i < n; i++) {
        total_acked += snap[i].bytes_acked;
        char rate[32];
        double elapsed = dp_now_sec() - t0;
        dp_format_rate(elapsed > 0 ? (double)snap[i].bytes_acked / elapsed : 0, rate, sizeof(rate));
        off += snprintf(line + off, sizeof(line) - off, "[QP%d %s %s%s] ",
                         i, snap[i].state == PATH_UP ? "UP" : "DOWN", rate,
                         snap[i].state == PATH_UP ? "" : "(已转移)");
    }
    char total_str[32];
    dp_format_bytes((double)total_acked, total_str, sizeof(total_str));
    DP_LOGI("进度: 已确认 %s  %s", total_str, line);
    (void)num_paths;
}

static void print_final_report(scheduler_t *s, int num_paths, double elapsed_sec) {
    sched_path_t snap[SCHED_MAX_PATHS];
    int n;
    scheduler_snapshot(s, snap, SCHED_MAX_PATHS, &n);

    uint64_t total_bytes = 0;
    printf("\n================ 传输完成：性能报告 ================\n");
    for (int i = 0; i < n; i++) {
        total_bytes += snap[i].bytes_acked;
        char bytes_str[32], rate_str[32];
        dp_format_bytes((double)snap[i].bytes_acked, bytes_str, sizeof(bytes_str));
        dp_format_rate(elapsed_sec > 0 ? (double)snap[i].bytes_acked / elapsed_sec : 0, rate_str, sizeof(rate_str));
        printf("  QP%d: 状态=%-4s  传输量=%-10s  平均速率=%-12s  分片数=%-8llu  故障转移出的分片数=%llu\n",
               i, snap[i].state == PATH_UP ? "UP" : "DOWN", bytes_str, rate_str,
               (unsigned long long)snap[i].chunks_acked, (unsigned long long)snap[i].chunks_failed_over);
    }
    char total_str[32], total_rate_str[32];
    dp_format_bytes((double)total_bytes, total_str, sizeof(total_str));
    dp_format_rate(elapsed_sec > 0 ? (double)total_bytes / elapsed_sec : 0, total_rate_str, sizeof(total_rate_str));
    printf("  ------------------------------------------------------------\n");
    printf("  聚合总量=%s  总耗时=%.3fs  聚合吞吐=%s  (RC QP 数=%d)\n",
           total_str, elapsed_sec, total_rate_str, num_paths);
    printf("=======================================================\n\n");
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "用法: %s -c <config.ini> -f <文件路径> [-o <远端保存文件名>] [-v] [--qp-count N]\n"
        "  -c, --config PATH     RDMA 多 QP 配置文件\n"
        "  -f, --file PATH       待发送的本地文件\n"
        "  -o, --output NAME     服务端保存的文件名，默认使用发送文件的 basename\n"
        "  -v, --verbose         打印 DEBUG 级别日志\n"
        "  --qp-count N          覆盖配置中的 QP 数（1..%d，便于做 1/2/4/8 QP 对比）\n"
        "  --inject-qp-failure Q:N  成功发送 N 个分片后关闭 QP Q，验证重分配\n",
        argv0, DP_MAX_PATHS);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *file_path = NULL;
    const char *output_name = NULL;
    int verbose = 0;
    uint32_t qp_count_override = 0;
    int inject_qp = -1;
    uint32_t inject_after = 0;

    static struct option long_opts[] = {
        {"config", required_argument, 0, 'c'},
        {"file", required_argument, 0, 'f'},
        {"output", required_argument, 0, 'o'},
        {"verbose", no_argument, 0, 'v'},
        {"qp-count", required_argument, 0, 1000},
        {"inject-qp-failure", required_argument, 0, 1001},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "c:f:o:vh", long_opts, NULL)) != -1) {
        switch (c) {
            case 'c': config_path = optarg; break;
            case 'f': file_path = optarg; break;
            case 'o': output_name = optarg; break;
            case 'v': verbose = 1; break;
            case 1000: {
                char *end = NULL;
                unsigned long value = strtoul(optarg, &end, 10);
                if (!optarg[0] || *end != '\0' || value < 1 || value > DP_MAX_PATHS) {
                    DP_LOGE("--qp-count 必须在 1..%d 之间", DP_MAX_PATHS);
                    return 1;
                }
                qp_count_override = (uint32_t)value;
                break;
            }
            case 1001: {
                char *colon = strchr(optarg, ':');
                char *end_qp = NULL;
                char *end_count = NULL;
                if (!colon) {
                    DP_LOGE("--inject-qp-failure 格式必须是 Q:N，例如 1:100");
                    return 1;
                }
                *colon = '\0';
                unsigned long qp = strtoul(optarg, &end_qp, 10);
                unsigned long count = strtoul(colon + 1, &end_count, 10);
                if (colon == optarg || *end_qp != '\0' || !colon[1] || *end_count != '\0' ||
                    qp >= DP_MAX_PATHS || count > UINT32_MAX) {
                    DP_LOGE("--inject-qp-failure 参数非法");
                    return 1;
                }
                inject_qp = (int)qp;
                inject_after = (uint32_t)count;
                break;
            }
            case 'h': default: usage(argv[0]); return (c == 'h') ? 0 : 1;
        }
    }
    if (!config_path || !file_path) { usage(argv[0]); return 1; }
    if (verbose) dp_set_log_level(DP_LOG_DEBUG);

    dp_config_t cfg;
    if (dp_config_load(config_path, &cfg) != 0) return 1;
    if (qp_count_override) {
        cfg.qp_count = qp_count_override;
        if ((uint32_t)cfg.base_port + cfg.qp_count - 1u > 65535u) {
            DP_LOGE("--qp-count 使 base_port + qp_count 超过 65535");
            return 1;
        }
        DP_LOGI("使用命令行覆盖：qp_count=%u", cfg.qp_count);
    }
    if (inject_qp >= (int)cfg.qp_count) {
        DP_LOGE("故障注入 QP %d 不存在（当前 qp_count=%u）", inject_qp, cfg.qp_count);
        return 1;
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) { DP_LOGE("无法打开文件 '%s'", file_path); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { DP_LOGE("fstat 失败"); close(fd); return 1; }
    if (st.st_size < 0 || !S_ISREG(st.st_mode)) {
        DP_LOGE("当前只支持可随机读取的普通文件");
        close(fd);
        return 1;
    }
    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t total_chunks64 = file_size / cfg.chunk_size +
        (file_size % cfg.chunk_size != 0);
    if (total_chunks64 > UINT32_MAX) {
        DP_LOGE("文件分片数超过协议上限 (%llu > %u)",
                (unsigned long long)total_chunks64, UINT32_MAX);
        close(fd);
        return 1;
    }
    uint32_t total_chunks = (uint32_t)total_chunks64;

    char name_buf[DP_MAX_NAME];
    if (!output_name) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", file_path);
        snprintf(name_buf, sizeof(name_buf), "%s", basename(tmp));
        output_name = name_buf;
    }

    {
        char sz[32];
        dp_format_bytes((double)file_size, sz, sizeof(sz));
        DP_LOGI("文件='%s' 大小=%s 分片数=%u 分片大小=%u RC_QP=%u 传输层=RDMA",
                file_path, sz, total_chunks, cfg.chunk_size, cfg.qp_count);
    }

    /* 并发建立所有 RC QP（对应服务端的多个 listener）。 */
    dp_path_t *paths[DP_MAX_PATHS] = {0};
    pthread_t connect_tids[DP_MAX_PATHS];
    int connect_started[DP_MAX_PATHS] = {0};
    connect_job_t jobs[DP_MAX_PATHS];
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        jobs[i].idx = i;
        jobs[i].cfg.local_ip = cfg.local_ip;
        jobs[i].cfg.remote_ip = cfg.remote_ip;
        jobs[i].cfg.port = cfg.base_port + (int)i;
        jobs[i].cfg.window_size = cfg.window;
        jobs[i].cfg.chunk_size = cfg.chunk_size;
        if (pthread_create(&connect_tids[i], NULL, connect_job_fn, &jobs[i]) == 0) {
            connect_started[i] = 1;
        } else {
            DP_LOGW("QP %u: 无法创建连接线程，改为同步连接", i);
            connect_job_fn(&jobs[i]);
        }
    }
    int ok = 1;
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        if (connect_started[i]) pthread_join(connect_tids[i], NULL);
        paths[i] = jobs[i].result;
        if (!paths[i]) { DP_LOGE("QP %u 连接失败", i); ok = 0; }
    }
    if (!ok) {
        for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
        close(fd);
        return 1;
    }
    DP_LOGI("全部 %u 个 RC QP 连接建立成功", cfg.qp_count);

    /* 控制通道握手 */
    dp_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = DP_MAGIC;
    hello.msg_type = DP_MSG_HELLO;
    hello.version = DP_PROTOCOL_VERSION;
    hello.session_id = ((uint64_t)time(NULL) << 32) ^ (uint32_t)getpid();
    hello.file_size = file_size;
    hello.chunk_size = cfg.chunk_size;
    hello.num_qps = cfg.qp_count;
    hello.total_chunks = total_chunks;
    snprintf(hello.file_name, sizeof(hello.file_name), "%s", output_name);

    if (dp_path_send_hello(paths[0], &hello) != 0) {
        DP_LOGE("发送 HELLO 失败");
        goto fail_cleanup;
    }
    dp_hello_ack_t hack;
    memset(&hack, 0, sizeof(hack));
    if (dp_path_recv_hello_ack(paths[0], &hack) != 0 || hack.status != 0) {
        DP_LOGE("HELLO 握手失败 (status=%u)", hack.status);
        goto fail_cleanup;
    }
    uint32_t window = cfg.window;
    if (hack.window_size > 0 && hack.window_size < window) window = hack.window_size;
    DP_LOGI("握手成功，有效窗口大小 = %u", window);

    /* 初始化调度器 */
    scheduler_t sched;
    if (scheduler_init(&sched, (int)cfg.qp_count, NULL, total_chunks, window) != 0) {
        DP_LOGE("调度器初始化失败");
        goto fail_cleanup;
    }

    pthread_mutex_t fail_lock = PTHREAD_MUTEX_INITIALIZER;
    int failed[DP_MAX_PATHS] = {0};

    double t0 = dp_now_sec();

    pthread_t ack_tids[DP_MAX_PATHS];
    int ack_started[DP_MAX_PATHS] = {0};
    ack_ctx_t actx[DP_MAX_PATHS];
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        actx[i].path = paths[i];
        actx[i].idx = i;
        actx[i].sched = &sched;
        actx[i].chunk_size = cfg.chunk_size;
        actx[i].file_size = file_size;
        actx[i].fail_lock = &fail_lock;
        actx[i].failed = failed;
        if (pthread_create(&ack_tids[i], NULL, ack_thread_fn, &actx[i]) == 0) {
            ack_started[i] = 1;
        } else {
            mark_failed(paths[i], &sched, &fail_lock, failed, i, "cannot create ACK thread");
        }
    }

    sender_ctx_t sctx = {
        .paths = paths,
        .sched = &sched,
        .num_paths = (int)cfg.qp_count,
        .file_fd = fd,
        .chunk_size = cfg.chunk_size,
        .file_size = file_size,
        .fail_lock = &fail_lock,
        .failed = failed,
        .inject_qp = inject_qp,
        .inject_after = inject_after,
    };
    pthread_t sender_tid;
    int sender_started = pthread_create(&sender_tid, NULL, sender_thread_fn, &sctx) == 0;
    if (!sender_started) {
        DP_LOGW("无法创建 sender 线程，改为主线程同步发送");
        sender_thread_fn(&sctx);
    }

    for (;;) {
        int terminal = scheduler_wait_terminal(&sched, 500);
        if (terminal > 0) break;
        if (terminal < 0) {
            DP_LOGW("等待完成事件失败，退化为短轮询");
            usleep(1000);
            if (scheduler_all_done(&sched) || !scheduler_has_live_path(&sched)) break;
        }
        print_progress(&sched, (int)cfg.qp_count, t0);
    }

    /* 必须在终态被 ACK 线程唤醒时取时间，不能把 500ms 进度轮询算进传输耗时。 */
    double data_elapsed = dp_now_sec() - t0;

    if (sender_started) pthread_join(sender_tid, NULL);

    int success = scheduler_all_done(&sched);
    if (success) {
        for (uint32_t i = 0; i < cfg.qp_count; i++) {
            if (!failed[i]) dp_path_send_done(paths[i]);
        }
    } else {
        DP_LOGE("传输失败：所有 QP 都已故障，无法完成传输");
    }

    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        if (ack_started[i]) pthread_join(ack_tids[i], NULL);
    }

    print_final_report(&sched, (int)cfg.qp_count, data_elapsed);

    for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
    pthread_mutex_destroy(&fail_lock);
    scheduler_destroy(&sched);
    close(fd);
    return success ? 0 : 1;

fail_cleanup:
    for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
    close(fd);
    return 1;
}
