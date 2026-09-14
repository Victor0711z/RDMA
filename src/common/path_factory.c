/*
 * path_factory.c - 创建纯 RDMA QP channel
 */
#include "path.h"
#include "util.h"

dp_path_t *dp_path_create_client(int path_id, const dp_path_cfg_t *cfg) {
    const dp_path_ops_t *ops = dp_rdma_get_ops();
    dp_path_t *p = ops->client_connect(cfg);
    if (p) p->id = path_id;
    return p;
}

dp_path_t *dp_path_create_server(int path_id, const dp_path_cfg_t *cfg) {
    const dp_path_ops_t *ops = dp_rdma_get_ops();
    dp_path_t *p = ops->server_accept_one(cfg);
    if (p) p->id = path_id;
    return p;
}
