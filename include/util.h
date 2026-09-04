/*
 * util.h - 通用工具函数：日志、计时、文件操作
 */
#ifndef DUALPATH_UTIL_H
#define DUALPATH_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum { DP_LOG_ERROR = 0, DP_LOG_WARN, DP_LOG_INFO, DP_LOG_DEBUG } dp_log_level_t;

void dp_set_log_level(dp_log_level_t level);
void dp_log(dp_log_level_t level, const char *fmt, ...);

#define DP_LOGE(...) dp_log(DP_LOG_ERROR, __VA_ARGS__)
#define DP_LOGW(...) dp_log(DP_LOG_WARN,  __VA_ARGS__)
#define DP_LOGI(...) dp_log(DP_LOG_INFO,  __VA_ARGS__)
#define DP_LOGD(...) dp_log(DP_LOG_DEBUG, __VA_ARGS__)

/* 单调时钟，返回秒（double 精度），用于测速 */
double dp_now_sec(void);

/* 人类可读的带宽/字节格式化，例如 "123.4 MB/s"、"1.2 GiB" */
void dp_format_bytes(double bytes, char *out, size_t out_len);
void dp_format_rate(double bytes_per_sec, char *out, size_t out_len);

/* 打开/预分配输出文件，返回 fd，失败返回 -1 */
int dp_open_output_file(const char *path, uint64_t size);

/* 简单的“文件大小 -> 分片数”换算，向上取整 */
uint32_t dp_calc_total_chunks(uint64_t file_size, uint32_t chunk_size);

#endif /* DUALPATH_UTIL_H */
