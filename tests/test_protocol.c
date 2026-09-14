#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    dp_hello_t original;
    memset(&original, 0, sizeof(original));
    original.magic = DP_MAGIC;
    original.msg_type = DP_MSG_HELLO;
    original.version = DP_PROTOCOL_VERSION;
    original.flags = 0x01020304u;
    original.session_id = 0x0123456789abcdefULL;
    original.file_size = 0xfedcba9876543210ULL;
    original.chunk_size = 1048576u;
    original.num_qps = 4;
    original.total_chunks = 12345;
    snprintf(original.file_name, sizeof(original.file_name), "payload.bin");

    dp_hello_t wire = original;
    dp_hello_hton(&wire);
    dp_hello_ntoh(&wire);
    assert(memcmp(&wire, &original, sizeof(original)) == 0);

    dp_hello_ack_t hello_ack = {
        .magic = DP_MAGIC,
        .msg_type = DP_MSG_HELLO_ACK,
        .window_size = 32,
        .status = 0,
    };
    dp_hello_ack_t hello_ack_original = hello_ack;
    dp_hello_ack_hton(&hello_ack);
    dp_hello_ack_ntoh(&hello_ack);
    assert(memcmp(&hello_ack, &hello_ack_original, sizeof(hello_ack)) == 0);

    dp_ack_hdr_t ack = {
        .magic = DP_MAGIC,
        .msg_type = DP_MSG_ACK,
        .chunk_id = UINT32_MAX,
        .status = 0,
    };
    dp_ack_hdr_t ack_original = ack;
    dp_ack_hdr_hton(&ack);
    dp_ack_hdr_ntoh(&ack);
    assert(memcmp(&ack, &ack_original, sizeof(ack)) == 0);

    dp_rdma_slot_hdr_t slot = {
        .magic = DP_MAGIC,
        .chunk_id = UINT32_MAX,
        .length = 4096,
        .crc32c = 0xe3069283u,
    };
    assert(slot.chunk_id == UINT32_MAX);
    puts("protocol tests: OK");
    return 0;
}
