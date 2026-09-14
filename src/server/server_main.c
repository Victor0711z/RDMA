/*
 * server_main.c - MultiQP-RDMA 服务端（单次传输，收完退出）
 *
 * 服务端不做调度决策，只在每个 RC QP 上独立收分片、写文件、回 ACK。
 * 分片分配与 QP 故障后的重分配由客户端调度器负责。
 */
#include "path.h"
#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <libgen.h>

typedef struct {
    pthread_mutex_t lock;
    int      fd;
    uint64_t file_size;
    uint32_t chunk_size;
    uint32_t total_chunks;
    uint32_t chunks_done;
    uint8_t *received;              /* received[chunk_id] = 1 表示已经写过（去重用） */
    uint64_t bytes_by_path[DP_MAX_PATHS];
    uint64_t chunks_by_path[DP_MAX_PATHS];
} recv_stats_t;

typedef struct {
    dp_path_t   *path;
    int          idx;
    recv_stats_t *stats;
    uint32_t     chunk_buf_cap;
} recv_ctx_t;

static void *recv_thread_fn(void *arg) {
    recv_ctx_t *ctx = (recv_ctx_t *)arg;
    uint8_t *buf = (uint8_t *)malloc(ctx->chunk_buf_cap);
    if (!buf) {
        DP_LOGE("recv: out of memory");
        dp_path_shutdown(ctx->path);
        dp_path_close(ctx->path);
        return NULL;
    }

    int graceful = 0;
    for (;;) {
        uint32_t chunk_id, len;
        int rc = dp_path_recv_chunk(ctx->path, &chunk_id, buf, ctx->chunk_buf_cap, &len);
        if (rc == 1) {
            DP_LOGI("QP %d: 收到 DONE / 对端关闭，该 QP 接收结束", ctx->idx);
            graceful = 1;
            break;
        }
        if (rc < 0) {
            DP_LOGW("QP %d: 接收出错，该 QP 提前结束（客户端会自动把剩余分片转移到其它 QP）", ctx->idx);
            break;
        }

        if (chunk_id >= ctx->stats->total_chunks) {
            DP_LOGE("QP %d: 收到越界分片 id=%u (total=%u)",
                    ctx->idx, chunk_id, ctx->stats->total_chunks);
            break;
        }
        uint64_t offset64 = (uint64_t)chunk_id * ctx->stats->chunk_size;
        uint64_t remain = ctx->stats->file_size - offset64;
        uint32_t expected_len = (uint32_t)(remain < ctx->stats->chunk_size
            ? remain : ctx->stats->chunk_size);
        if (len != expected_len) {
            DP_LOGE("QP %d: 分片 %u 长度错误 (%u != %u)",
                    ctx->idx, chunk_id, len, expected_len);
            break;
        }

        pthread_mutex_lock(&ctx->stats->lock);
        int dup = (chunk_id < ctx->stats->total_chunks && ctx->stats->received[chunk_id]);
        pthread_mutex_unlock(&ctx->stats->lock);

        if (!dup) {
            off_t offset = (off_t)chunk_id * (off_t)ctx->stats->chunk_size;
            ssize_t n = pwrite(ctx->stats->fd, buf, len, offset);
            if (n != (ssize_t)len) {
                DP_LOGE("QP %d: 写文件第 %u 片失败", ctx->idx, chunk_id);
                dp_path_shutdown(ctx->path);
                dp_path_close(ctx->path);
                free(buf);
                return NULL;
            }
            pthread_mutex_lock(&ctx->stats->lock);
            int counted = 0;
            if (chunk_id < ctx->stats->total_chunks && !ctx->stats->received[chunk_id]) {
                ctx->stats->received[chunk_id] = 1;
                ctx->stats->chunks_done++;
                counted = 1;
            }
            if (counted) {
                ctx->stats->bytes_by_path[ctx->idx] += len;
                ctx->stats->chunks_by_path[ctx->idx]++;
            }
            pthread_mutex_unlock(&ctx->stats->lock);
        } else {
            DP_LOGD("QP %d: 分片 %u 是故障转移产生的重复分片，已忽略", ctx->idx, chunk_id);
        }

        if (dp_path_send_ack(ctx->path, chunk_id) != 0) {
            DP_LOGW("QP %d: 发送 ACK 失败", ctx->idx);
            break;
        }
    }

    free(buf);
    if (!graceful) dp_path_shutdown(ctx->path);
    dp_path_close(ctx->path);
    return NULL;
}

typedef struct {
    int idx;
    dp_path_cfg_t cfg;
    dp_path_t *result;
} accept_job_t;

static void *accept_job_fn(void *arg) {
    accept_job_t *job = (accept_job_t *)arg;
    job->result = dp_path_create_server(job->idx, &job->cfg);
    return NULL;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "用法: %s -c <config.ini> -d <输出目录> [-v]\n"
        "  -c, --config PATH   RDMA 多 QP 配置文件（local_ip 是本机 RDMA IP）\n"
        "  -d, --outdir PATH   接收到的文件保存目录\n"
        "  -v, --verbose       打印 DEBUG 级别日志\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *outdir = ".";
    int verbose = 0;

    static struct option long_opts[] = {
        {"config", required_argument, 0, 'c'},
        {"outdir", required_argument, 0, 'd'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "c:d:vh", long_opts, NULL)) != -1) {
        switch (c) {
            case 'c': config_path = optarg; break;
            case 'd': outdir = optarg; break;
            case 'v': verbose = 1; break;
            case 'h': default: usage(argv[0]); return (c == 'h') ? 0 : 1;
        }
    }
    if (!config_path) { usage(argv[0]); return 1; }
    if (verbose) dp_set_log_level(DP_LOG_DEBUG);

    dp_config_t cfg;
    if (dp_config_load(config_path, &cfg) != 0) return 1;

    DP_LOGI("服务端启动：单 RDMA 设备，%u 个 RC QP，端口 %d..%d，等待连接...",
            cfg.qp_count, cfg.base_port, cfg.base_port + (int)cfg.qp_count - 1);

    /* 每个端口对应一个独立 RC QP，并发 listen/accept。 */
    dp_path_t *paths[DP_MAX_PATHS] = {0};
    pthread_t accept_tids[DP_MAX_PATHS];
    int accept_started[DP_MAX_PATHS] = {0};
    accept_job_t jobs[DP_MAX_PATHS];
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        jobs[i].idx = i;
        jobs[i].cfg.local_ip = cfg.local_ip;
        jobs[i].cfg.remote_ip = cfg.remote_ip;
        jobs[i].cfg.port = cfg.base_port + (int)i;
        jobs[i].cfg.window_size = cfg.window;
        jobs[i].cfg.chunk_size = cfg.chunk_size;
        if (pthread_create(&accept_tids[i], NULL, accept_job_fn, &jobs[i]) == 0) {
            accept_started[i] = 1;
        } else {
            DP_LOGW("QP %u: 无法创建 accept 线程，改为同步等待", i);
            accept_job_fn(&jobs[i]);
        }
    }
    int ok = 1;
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        if (accept_started[i]) pthread_join(accept_tids[i], NULL);
        paths[i] = jobs[i].result;
        if (!paths[i]) { DP_LOGE("QP %u accept 失败", i); ok = 0; }
    }
    if (!ok) {
        for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
        return 1;
    }
    DP_LOGI("全部 %u 个 RC QP 已建立连接", cfg.qp_count);

    /* 控制通道握手 */
    dp_hello_t hello;
    if (dp_path_recv_hello(paths[0], &hello) != 0) {
        DP_LOGE("接收 HELLO 失败");
        for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
        return 1;
    }
    hello.file_name[DP_MAX_NAME - 1] = '\0';


    uint32_t hello_status = 0;
    uint64_t expected_chunks = hello.chunk_size == 0 ? UINT64_MAX :
        hello.file_size / hello.chunk_size + (hello.file_size % hello.chunk_size != 0);
    if (hello.version != DP_PROTOCOL_VERSION || hello.num_qps != cfg.qp_count ||
        hello.chunk_size != cfg.chunk_size ||
        expected_chunks != hello.total_chunks) {
        DP_LOGE("HELLO 不兼容: version=%u/%u qps=%u/%u chunk_size=%u/%u chunks=%u/%llu",
                hello.version, DP_PROTOCOL_VERSION, hello.num_qps, cfg.qp_count,
                hello.chunk_size, cfg.chunk_size,
                hello.total_chunks, (unsigned long long)expected_chunks);
        hello_status = 2;
    }

    char safe_name[DP_MAX_NAME];
    {
        char tmp[DP_MAX_NAME];
        snprintf(tmp, sizeof(tmp), "%s", hello.file_name);
        snprintf(safe_name, sizeof(safe_name), "%s", basename(tmp)); /* 防路径穿越 */
    }
    if (!safe_name[0] || strcmp(safe_name, ".") == 0 || strcmp(safe_name, "..") == 0) {
        DP_LOGE("HELLO 中的输出文件名非法");
        hello_status = 3;
    }
    char outpath[2048];
    snprintf(outpath, sizeof(outpath), "%s/%s", outdir, safe_name);

    int fd = hello_status == 0 ? dp_open_output_file(outpath, hello.file_size) : -1;
    dp_hello_ack_t hack;
    memset(&hack, 0, sizeof(hack));
    hack.magic = DP_MAGIC;
    hack.msg_type = DP_MSG_HELLO_ACK;
    hack.window_size = cfg.window;
    hack.status = hello_status != 0 ? hello_status : ((fd < 0) ? 1 : 0);
    if (dp_path_send_hello_ack(paths[0], &hack) != 0) {
        DP_LOGE("发送 HELLO_ACK 失败");
        if (fd >= 0) close(fd);
        for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
        return 1;
    }

    if (fd < 0) {
        if (hello_status == 0) DP_LOGE("无法创建输出文件 '%s'", outpath);
        else DP_LOGE("拒绝不兼容的传输参数 (status=%u)", hello_status);
        for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
        return 1;
    }

    {
        char sz[32];
        dp_format_bytes((double)hello.file_size, sz, sizeof(sz));
        DP_LOGI("接收任务: session=%016llx 文件='%s' 大小=%s 分片数=%u -> '%s'",
                (unsigned long long)hello.session_id, hello.file_name, sz,
                hello.total_chunks, outpath);
    }

    /* 每个 QP 共享相同的传输元信息。 */
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        dp_path_set_transfer_meta(paths[i], hello.file_size, hello.chunk_size);
    }

    recv_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    pthread_mutex_init(&stats.lock, NULL);
    stats.fd = fd;
    stats.file_size = hello.file_size;
    stats.chunk_size = hello.chunk_size;
    stats.total_chunks = hello.total_chunks;
    stats.received = (uint8_t *)calloc(hello.total_chunks > 0 ? hello.total_chunks : 1, 1);
    if (!stats.received) {
        DP_LOGE("无法分配分片去重表");
        for (uint32_t i = 0; i < cfg.qp_count; i++) dp_path_close(paths[i]);
        pthread_mutex_destroy(&stats.lock);
        close(fd);
        return 1;
    }

    double t0 = dp_now_sec();

    pthread_t recv_tids[DP_MAX_PATHS];
    int recv_started[DP_MAX_PATHS] = {0};
    recv_ctx_t rctx[DP_MAX_PATHS];
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        rctx[i].path = paths[i];
        rctx[i].idx = i;
        rctx[i].stats = &stats;
        rctx[i].chunk_buf_cap = cfg.chunk_size;
        if (pthread_create(&recv_tids[i], NULL, recv_thread_fn, &rctx[i]) == 0) {
            recv_started[i] = 1;
        } else {
            DP_LOGE("QP %u: 无法创建接收线程，关闭该 QP", i);
            dp_path_shutdown(paths[i]);
            dp_path_close(paths[i]);
            paths[i] = NULL;
        }
    }
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        if (recv_started[i]) pthread_join(recv_tids[i], NULL);
    }

    double elapsed = dp_now_sec() - t0;

    printf("\n================ 接收完成：性能报告 ================\n");
    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < cfg.qp_count; i++) {
        total_bytes += stats.bytes_by_path[i];
        char bytes_str[32], rate_str[32];
        dp_format_bytes((double)stats.bytes_by_path[i], bytes_str, sizeof(bytes_str));
        dp_format_rate(elapsed > 0 ? (double)stats.bytes_by_path[i] / elapsed : 0, rate_str, sizeof(rate_str));
        printf("  QP%u: 接收量=%-10s  平均速率=%-12s  分片数=%llu\n",
               i, bytes_str, rate_str, (unsigned long long)stats.chunks_by_path[i]);
    }
    char total_str[32], total_rate_str[32];
    dp_format_bytes((double)total_bytes, total_str, sizeof(total_str));
    dp_format_rate(elapsed > 0 ? (double)total_bytes / elapsed : 0, total_rate_str, sizeof(total_rate_str));
    printf("  ------------------------------------------------------------\n");
    printf("  聚合总量=%s  总耗时=%.3fs  聚合吞吐=%s\n", total_str, elapsed, total_rate_str);
    printf("  完整性: %u/%u 分片已接收 -> %s\n", stats.chunks_done, stats.total_chunks,
           stats.chunks_done == stats.total_chunks ? "OK" : "不完整！");
    printf("=======================================================\n\n");

    free(stats.received);
    pthread_mutex_destroy(&stats.lock);
    close(fd);
    return (stats.chunks_done == stats.total_chunks) ? 0 : 1;
}
