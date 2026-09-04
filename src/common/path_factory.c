/*
 * path_factory.c - 根据 transport 类型分发到具体后端 (tcp / rdma)
 */
#include "path.h"
#include "util.h"

dp_path_t *dp_path_create_client(dp_transport_t type, int path_id, const dp_path_cfg_t *cfg) {
    const dp_path_ops_t *ops = NULL;
    if (type == DP_TRANSPORT_TCP) {
        ops = dp_tcp_get_ops();
    }
#ifdef DP_ENABLE_RDMA
    else if (type == DP_TRANSPORT_RDMA) {
        ops = dp_rdma_get_ops();
    }
#endif
    if (!ops) {
        DP_LOGE("path %d: transport type %d not available in this build", path_id, (int)type);
        return NULL;
    }
    dp_path_t *p = ops->client_connect(cfg);
    if (p) p->id = path_id;
    return p;
}

dp_path_t *dp_path_create_server(dp_transport_t type, int path_id, const dp_path_cfg_t *cfg) {
    const dp_path_ops_t *ops = NULL;
    if (type == DP_TRANSPORT_TCP) {
        ops = dp_tcp_get_ops();
    }
#ifdef DP_ENABLE_RDMA
    else if (type == DP_TRANSPORT_RDMA) {
        ops = dp_rdma_get_ops();
    }
#endif
    if (!ops) {
        DP_LOGE("path %d: transport type %d not available in this build", path_id, (int)type);
        return NULL;
    }
    dp_path_t *p = ops->server_accept_one(cfg);
    if (p) p->id = path_id;
    return p;
}
