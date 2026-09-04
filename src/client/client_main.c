/*
 * client_main.c - DualPath-RDMA 客户端
 *
 * 整体流程：
 *   1) 按配置文件并发建立 N 条路径连接（每条路径对应一张网卡）
 *   2) 在控制通道 (path0) 上做一次 HELLO/HELLO_ACK 握手，告知服务端
 *      文件名/大小/分片大小/总分片数
 *   3) 初始化调度器 (scheduler_t)，启动：
 *        - 1 个发送线程：不断向调度器要"下一个该发去哪条路径的分片"，
 *          读文件、通过该路径发送
 *        - N 个 ACK 接收线程（每条路径一个）：接收服务端的分片确认，
 *          释放该路径的 credit；一旦某条路径读/写出错，就把它标记为
 *          DOWN，调度器会自动把它名下未完成的分片转移给存活路径
 *          —— 这就是"故障转移"
 *   4) 所有分片被 ACK 后，向每条存活路径发送 DONE，打印聚合吞吐报告
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
#include <signal.h>

typedef struct {
    dp_transport_t transport;
    int idx;
    dp_path_cfg_t cfg;
    dp_path_t *result;
} connect_job_t;

static void *connect_job_fn(void *arg) {
    connect_job_t *job = (connect_job_t *)arg;
    job->result = dp_path_create_client(job->transport, job->idx, &job->cfg);
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
} sender_ctx_t;

typedef struct {
    dp_path_t *path;
    int idx;
    scheduler_t *sched;
    uint32_t chunk_size;
    pthread_mutex_t *fail_lock;
    int *failed;
} ack_ctx_t;

static void mark_failed(scheduler_t *s, pthread_mutex_t *lock, int *failed, int idx, const char *reason) {
    pthread_mutex_lock(lock);
    if (!failed[idx]) {
        failed[idx] = 1;
        scheduler_mark_down(s, idx);
        DP_LOGW("*** path %d 故障 (%s)，未完成分片将自动转移到其它路径 ***", idx, reason);
    }
    pthread_mutex_unlock(lock);
}

static void *sender_thread_fn(void *arg) {
    sender_ctx_t *ctx = (sender_ctx_t *)arg;
    uint8_t *buf = (uint8_t *)malloc(ctx->chunk_size);
    if (!buf) { DP_LOGE("sender: out of memory"); return NULL; }

    for (;;) {
        if (scheduler_all_done(ctx->sched)) break;
        if (!scheduler_has_live_path(ctx->sched)) {
            DP_LOGE("sender: 所有路径均已故障，传输无法继续");
            break;
        }
        int path_idx;
        uint32_t chunk_id;
        if (scheduler_acquire(ctx->sched, &path_idx, &chunk_id, ctx->chunk_size) != 0) {
            usleep(1000); /* 暂时没有可用 credit 或没有更多分片，稍后重试 */
            continue;
        }

        uint64_t offset = (uint64_t)chunk_id * ctx->chunk_size;
        uint64_t remain = ctx->file_size - offset;
        uint32_t len = (uint32_t)(remain < ctx->chunk_size ? remain : ctx->chunk_size);

        ssize_t n = pread(ctx->file_fd, buf, len, (off_t)offset);
        if (n != (ssize_t)len) {
            DP_LOGE("sender: 读文件第 %u 片失败 (offset=%llu len=%u)", chunk_id,
                    (unsigned long long)offset, len);
            /* 本地文件读失败与网络无关，但仍通过故障转移机制把这个分片让别的时刻重试没有意义，
             * 这里直接判定该路径异常，交给故障转移流程处理，避免整条流水线卡死 */
            mark_failed(ctx->sched, ctx->fail_lock, ctx->failed, path_idx, "local read error");
            continue;
        }

        if (dp_path_send_chunk(ctx->paths[path_idx], chunk_id, buf, len) != 0) {
            mark_failed(ctx->sched, ctx->fail_lock, ctx->failed, path_idx, "send error");
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
            mark_failed(ctx->sched, ctx->fail_lock, ctx->failed, ctx->idx, "recv error");
            break;
        }
        if (rc == 1) {
            mark_failed(ctx->sched, ctx->fail_lock, ctx->failed, ctx->idx, "connection closed unexpectedly");
            break;
        }
        if (is_done) {
            DP_LOGD("path %d: 对端正常关闭 (BYE)", ctx->idx);
            break;
        }
        scheduler_on_ack(ctx->sched, ctx->idx, chunk_id, ctx->chunk_size);
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
        off += snprintf(line + off, sizeof(line) - off, "[path%d %s %s%s] ",
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
        printf("  path%d: 状态=%-4s  传输量=%-10s  平均速率=%-12s  分片数=%-8llu  故障转移出的分片数=%llu\n",
               i, snap[i].state == PATH_UP ? "UP" : "DOWN", bytes_str, rate_str,
               (unsigned long long)snap[i].chunks_acked, (unsigned long long)snap[i].chunks_failed_over);
    }
    char total_str[32], total_rate_str[32];
    dp_format_bytes((double)total_bytes, total_str, sizeof(total_str));
    dp_format_rate(elapsed_sec > 0 ? (double)total_bytes / elapsed_sec : 0, total_rate_str, sizeof(total_rate_str));
    printf("  ------------------------------------------------------------\n");
    printf("  聚合总量=%s  总耗时=%.3fs  聚合吞吐=%s  (使用路径数=%d)\n",
           total_str, elapsed_sec, total_rate_str, num_paths);
    printf("=======================================================\n\n");
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "用法: %s -c <config.ini> -f <文件路径> [-o <远端保存文件名>] [-v] [--single-path]\n"
        "  -c, --config PATH     多路径配置文件 (见 config/config.example.ini)\n"
        "  -f, --file PATH       待发送的本地文件\n"
        "  -o, --output NAME     服务端保存的文件名，默认使用发送文件的 basename\n"
        "  -v, --verbose         打印 DEBUG 级别日志\n"
        "  --single-path         仅使用配置文件中的 path0（用于测单网卡基线，和双网卡结果对比）\n",
        argv0);
}

int main(int argc, char **argv) {
    /* 关键：某条路径故障后，我们可能还会往一个已经被对端重置/关闭的 socket 上
     * 写数据（比如关闭阶段尝试发一个 BYE），默认情况下这会触发 SIGPIPE 直接
     * 杀死整个进程。忽略 SIGPIPE，改为让 write()/send() 返回 -1(EPIPE)，
     * 由我们已有的错误处理路径（故障转移）来处理，而不是让进程被信号杀死。 */
    signal(SIGPIPE, SIG_IGN);

    const char *config_path = NULL;
    const char *file_path = NULL;
    const char *output_name = NULL;
    int verbose = 0;
    int single_path = 0;

    static struct option long_opts[] = {
        {"config", required_argument, 0, 'c'},
        {"file", required_argument, 0, 'f'},
        {"output", required_argument, 0, 'o'},
        {"verbose", no_argument, 0, 'v'},
        {"single-path", no_argument, 0, 1000},
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
            case 1000: single_path = 1; break;
            case 'h': default: usage(argv[0]); return (c == 'h') ? 0 : 1;
        }
    }
    if (!config_path || !file_path) { usage(argv[0]); return 1; }
    if (verbose) dp_set_log_level(DP_LOG_DEBUG);

    dp_config_t cfg;
    if (dp_config_load(config_path, &cfg) != 0) return 1;
    if (single_path) {
        cfg.num_paths = 1;
        DP_LOGI("--single-path: 仅使用 path0，用于单网卡基线测试");
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) { DP_LOGE("无法打开文件 '%s'", file_path); return 1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { DP_LOGE("fstat 失败"); close(fd); return 1; }
    uint64_t file_size = (uint64_t)st.st_size;
    uint32_t total_chunks = dp_calc_total_chunks(file_size, cfg.chunk_size);

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
        DP_LOGI("文件='%s' 大小=%s 分片数=%u 分片大小=%u 路径数=%d 传输层=%s",
                file_path, sz, total_chunks, cfg.chunk_size, cfg.num_paths,
                cfg.transport == DP_TRANSPORT_TCP ? "tcp" : "rdma");
    }

    /* 并发建立所有路径连接（对应服务端并发 listen） */
    dp_path_t *paths[DP_MAX_PATHS] = {0};
    pthread_t connect_tids[DP_MAX_PATHS];
    connect_job_t jobs[DP_MAX_PATHS];
    for (int i = 0; i < cfg.num_paths; i++) {
        jobs[i].transport = cfg.transport;
        jobs[i].idx = i;
        jobs[i].cfg.local_ip = cfg.paths[i].local_ip;
        jobs[i].cfg.remote_ip = cfg.paths[i].remote_ip;
        jobs[i].cfg.port = cfg.paths[i].port;
        jobs[i].cfg.window_size = cfg.window;
        jobs[i].cfg.chunk_size = cfg.chunk_size;
        jobs[i].cfg.is_control = (i == 0);
        pthread_create(&connect_tids[i], NULL, connect_job_fn, &jobs[i]);
    }
    int ok = 1;
    for (int i = 0; i < cfg.num_paths; i++) {
        pthread_join(connect_tids[i], NULL);
        paths[i] = jobs[i].result;
        if (!paths[i]) { DP_LOGE("path %d 连接失败", i); ok = 0; }
    }
    if (!ok) {
        for (int i = 0; i < cfg.num_paths; i++) dp_path_close(paths[i]);
        close(fd);
        return 1;
    }
    DP_LOGI("全部 %d 条路径连接建立成功", cfg.num_paths);

    /* 控制通道握手 */
    dp_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = DP_MAGIC;
    hello.msg_type = DP_MSG_HELLO;
    hello.file_size = file_size;
    hello.chunk_size = cfg.chunk_size;
    hello.num_paths = (uint32_t)cfg.num_paths;
    hello.total_chunks = total_chunks;
    snprintf(hello.file_name, sizeof(hello.file_name), "%s", output_name);

    if (dp_path_send_hello(paths[0], &hello) != 0) {
        DP_LOGE("发送 HELLO 失败");
        goto fail_cleanup;
    }
    dp_hello_ack_t hack;
    if (dp_path_recv_hello_ack(paths[0], &hack) != 0 || hack.status != 0) {
        DP_LOGE("HELLO 握手失败 (status=%u)", hack.status);
        goto fail_cleanup;
    }
    uint32_t window = cfg.window;
    if (hack.window_size > 0 && hack.window_size < window) window = hack.window_size;
    DP_LOGI("握手成功，有效窗口大小 = %u", window);

    /* 初始化调度器 */
    scheduler_t sched;
    double weights[SCHED_MAX_PATHS];
    for (int i = 0; i < cfg.num_paths; i++) weights[i] = cfg.paths[i].weight;
    if (scheduler_init(&sched, cfg.num_paths, weights, total_chunks, window) != 0) {
        DP_LOGE("调度器初始化失败");
        goto fail_cleanup;
    }

    pthread_mutex_t fail_lock = PTHREAD_MUTEX_INITIALIZER;
    int failed[DP_MAX_PATHS] = {0};

    double t0 = dp_now_sec();

    sender_ctx_t sctx = { paths, &sched, cfg.num_paths, fd, cfg.chunk_size, file_size, &fail_lock, failed };
    pthread_t sender_tid;
    pthread_create(&sender_tid, NULL, sender_thread_fn, &sctx);

    pthread_t ack_tids[DP_MAX_PATHS];
    ack_ctx_t actx[DP_MAX_PATHS];
    for (int i = 0; i < cfg.num_paths; i++) {
        actx[i].path = paths[i];
        actx[i].idx = i;
        actx[i].sched = &sched;
        actx[i].chunk_size = cfg.chunk_size;
        actx[i].fail_lock = &fail_lock;
        actx[i].failed = failed;
        pthread_create(&ack_tids[i], NULL, ack_thread_fn, &actx[i]);
    }

    while (!scheduler_all_done(&sched) && scheduler_has_live_path(&sched)) {
        usleep(500 * 1000);
        print_progress(&sched, cfg.num_paths, t0);
    }

    pthread_join(sender_tid, NULL);

    int success = scheduler_all_done(&sched);
    if (success) {
        for (int i = 0; i < cfg.num_paths; i++) {
            if (!failed[i]) dp_path_send_done(paths[i]);
        }
    } else {
        DP_LOGE("传输失败：所有路径都已故障，无法完成传输");
    }

    for (int i = 0; i < cfg.num_paths; i++) pthread_join(ack_tids[i], NULL);

    double elapsed = dp_now_sec() - t0;
    print_final_report(&sched, cfg.num_paths, elapsed);

    for (int i = 0; i < cfg.num_paths; i++) dp_path_close(paths[i]);
    scheduler_destroy(&sched);
    close(fd);
    return success ? 0 : 1;

fail_cleanup:
    for (int i = 0; i < cfg.num_paths; i++) dp_path_close(paths[i]);
    close(fd);
    return 1;
}
