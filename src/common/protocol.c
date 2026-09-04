/*
 * protocol.c - 协议结构体的主机序 <-> 网络序转换
 *
 * 所有多字节字段在线路上都使用网络字节序（big-endian）传输，
 * 这样即使 client / server 跑在不同字节序的机器上（例如 x86 与某些
 * ARM/嵌入式平台）也能正确通信 —— 这是网络协议设计的基本规范。
 */
#include "protocol.h"
#include <endian.h>
#include <string.h>

void dp_hello_hton(dp_hello_t *h) {
    h->magic       = htobe32(h->magic);
    h->msg_type    = htobe32(h->msg_type);
    h->file_size   = htobe64(h->file_size);
    h->chunk_size  = htobe32(h->chunk_size);
    h->num_paths   = htobe32(h->num_paths);
    h->total_chunks= htobe32(h->total_chunks);
}

void dp_hello_ntoh(dp_hello_t *h) {
    h->magic       = be32toh(h->magic);
    h->msg_type    = be32toh(h->msg_type);
    h->file_size   = be64toh(h->file_size);
    h->chunk_size  = be32toh(h->chunk_size);
    h->num_paths   = be32toh(h->num_paths);
    h->total_chunks= be32toh(h->total_chunks);
}

void dp_hello_ack_hton(dp_hello_ack_t *a) {
    a->magic       = htobe32(a->magic);
    a->msg_type    = htobe32(a->msg_type);
    a->window_size = htobe32(a->window_size);
    a->status      = htobe32(a->status);
}

void dp_hello_ack_ntoh(dp_hello_ack_t *a) {
    a->magic       = be32toh(a->magic);
    a->msg_type    = be32toh(a->msg_type);
    a->window_size = be32toh(a->window_size);
    a->status      = be32toh(a->status);
}

void dp_chunk_hdr_hton(dp_chunk_hdr_t *c) {
    c->magic    = htobe32(c->magic);
    c->msg_type = htobe32(c->msg_type);
    c->chunk_id = htobe32(c->chunk_id);
    c->length   = htobe32(c->length);
}

void dp_chunk_hdr_ntoh(dp_chunk_hdr_t *c) {
    c->magic    = be32toh(c->magic);
    c->msg_type = be32toh(c->msg_type);
    c->chunk_id = be32toh(c->chunk_id);
    c->length   = be32toh(c->length);
}

void dp_ack_hdr_hton(dp_ack_hdr_t *a) {
    a->magic    = htobe32(a->magic);
    a->msg_type = htobe32(a->msg_type);
    a->chunk_id = htobe32(a->chunk_id);
    a->status   = htobe32(a->status);
}

void dp_ack_hdr_ntoh(dp_ack_hdr_t *a) {
    a->magic    = be32toh(a->magic);
    a->msg_type = be32toh(a->msg_type);
    a->chunk_id = be32toh(a->chunk_id);
    a->status   = be32toh(a->status);
}
