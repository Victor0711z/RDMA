/* config.h - pure-RDMA multi-QP configuration */
#ifndef DUALPATH_CONFIG_H
#define DUALPATH_CONFIG_H

#include <stdint.h>
#include "protocol.h"

typedef struct {
    char     local_ip[64];
    char     remote_ip[64];
    int      base_port;   /* QP i listens/connects on base_port + i */
    uint32_t qp_count;    /* independent RC QPs on one RDMA device */
    uint32_t chunk_size;  /* payload bytes in one logical chunk */
    uint32_t window;      /* registered slots and credits per QP */
} dp_config_t;

int dp_config_load(const char *path, dp_config_t *out);

#endif /* DUALPATH_CONFIG_H */
