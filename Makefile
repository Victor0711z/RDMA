# MultiQP-RDMA Makefile (pure RDMA; no TCP data/control backend)

CC        ?= gcc
CFLAGS    ?= -Wall -Wextra -O2 -g -std=gnu11
INCLUDES   = -Iinclude
BUILD_DIR  = build

COMMON_SRC = src/common/util.c src/common/protocol.c src/common/scheduler.c \
             src/common/config.c src/common/path_factory.c
RDMA_SRC   = src/rdma/rdma_path.c
CLIENT_SRC = src/client/client_main.c
SERVER_SRC = src/server/server_main.c

RDMA_CFLAGS := $(shell pkg-config --cflags libibverbs librdmacm 2>/dev/null)
RDMA_LIBS   := $(shell pkg-config --libs libibverbs librdmacm 2>/dev/null)

.PHONY: all rdma test clean check-rdma-deps

all: rdma

check-rdma-deps:
	@pkg-config --exists libibverbs librdmacm || \
		(echo "错误: 找不到 libibverbs/librdmacm 开发包。请先执行:"; \
		 echo "  sudo apt update && sudo apt install -y libibverbs-dev librdmacm-dev build-essential pkg-config"; \
		 exit 1)

rdma: check-rdma-deps $(BUILD_DIR)/dp_client_rdma $(BUILD_DIR)/dp_server_rdma
	@echo "纯 RDMA 多 QP 版本编译完成: $(BUILD_DIR)/dp_client_rdma, $(BUILD_DIR)/dp_server_rdma"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/dp_client_rdma: $(CLIENT_SRC) $(COMMON_SRC) $(RDMA_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(RDMA_CFLAGS) -o $@ $^ -lpthread $(RDMA_LIBS)

$(BUILD_DIR)/dp_server_rdma: $(SERVER_SRC) $(COMMON_SRC) $(RDMA_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(RDMA_CFLAGS) -o $@ $^ -lpthread $(RDMA_LIBS)

$(BUILD_DIR)/test_scheduler: tests/test_scheduler.c src/common/scheduler.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lpthread

$(BUILD_DIR)/test_crc32c: tests/test_crc32c.c src/common/util.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lpthread

$(BUILD_DIR)/test_config: tests/test_config.c src/common/config.c src/common/util.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lpthread

$(BUILD_DIR)/test_protocol: tests/test_protocol.c src/common/protocol.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test: $(BUILD_DIR)/test_scheduler $(BUILD_DIR)/test_crc32c $(BUILD_DIR)/test_config $(BUILD_DIR)/test_protocol
	./$(BUILD_DIR)/test_scheduler
	./$(BUILD_DIR)/test_crc32c
	./$(BUILD_DIR)/test_config
	./$(BUILD_DIR)/test_protocol

clean:
	rm -rf $(BUILD_DIR)
