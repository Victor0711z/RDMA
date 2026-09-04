/*
 * config.h - 极简 INI 风格配置文件解析
 *
 * 格式示例见 config/config.example.ini：
 *   [general]
 *   transport=tcp        ; tcp | rdma
 *   chunk_size=1048576
 *   window=8
 *
 *   [path0]
 *   local_ip=192.168.10.1
 *   remote_ip=192.168.10.2
 *   port=18801
 *   weight=1.0
 *
 *   [path1]
 *   local_ip=192.168.20.1
 *   remote_ip=192.168.20.2
 *   port=18802
 *   weight=1.0
 *
 * path0 永远是控制通道（承载握手 HELLO/HELLO_ACK）。
 */
#ifndef DUALPATH_CONFIG_H
#define DUALPATH_CONFIG_H

#include <stdint.h>
#include "path.h"
#include "protocol.h"

typedef struct {
    char local_ip[64];
    char remote_ip[64];
    int  port;
    double weight;
} dp_path_conf_t;

typedef struct {
    dp_transport_t transport;
    uint32_t       chunk_size;
    uint32_t       window;
    int            num_paths;
    dp_path_conf_t paths[DP_MAX_PATHS];
} dp_config_t;

/* 返回 0 成功，-1 失败（文件不存在/格式错误，错误信息通过 DP_LOGE 打印） */
int dp_config_load(const char *path, dp_config_t *out);

#endif /* DUALPATH_CONFIG_H */
