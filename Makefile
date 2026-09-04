# DualPath-RDMA Makefile
#
#   make          -> 编译 TCP 版本 (build/dp_client, build/dp_server)
#                     不依赖任何 RDMA 库，任何机器都能编译，用于本地开发/演示
#   make rdma     -> 额外编译 RDMA 版本 (build/dp_client_rdma, build/dp_server_rdma)
#                     需要系统装有 libibverbs-dev / librdmacm-dev
#                     (Ubuntu/Debian: apt install libibverbs-dev librdmacm-dev build-essential)
#   make clean    -> 清理

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -g -std=gnu11
INCLUDES = -Iinclude
BUILD_DIR = build

COMMON_SRC = src/common/util.c src/common/protocol.c src/common/scheduler.c \
             src/common/config.c src/common/path_factory.c
TCP_SRC    = src/tcp/tcp_path.c
RDMA_SRC   = src/rdma/rdma_path.c

CLIENT_SRC = src/client/client_main.c
SERVER_SRC = src/server/server_main.c

.PHONY: all tcp rdma clean check-rdma-deps

all: tcp

tcp: $(BUILD_DIR)/dp_client $(BUILD_DIR)/dp_server
	@echo "TCP 版本编译完成: $(BUILD_DIR)/dp_client, $(BUILD_DIR)/dp_server"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/dp_client: $(CLIENT_SRC) $(COMMON_SRC) $(TCP_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lpthread

$(BUILD_DIR)/dp_server: $(SERVER_SRC) $(COMMON_SRC) $(TCP_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lpthread

RDMA_CFLAGS := $(shell pkg-config --cflags libibverbs librdmacm 2>/dev/null)
RDMA_LIBS   := $(shell pkg-config --libs libibverbs librdmacm 2>/dev/null)

check-rdma-deps:
	@pkg-config --exists libibverbs librdmacm || \
		(echo "错误: 找不到 libibverbs/librdmacm 开发包。请先执行:"; \
		 echo "  sudo apt update && sudo apt install -y libibverbs-dev librdmacm-dev build-essential"; \
		 exit 1)

rdma: check-rdma-deps $(BUILD_DIR)/dp_client_rdma $(BUILD_DIR)/dp_server_rdma
	@echo "RDMA 版本编译完成: $(BUILD_DIR)/dp_client_rdma, $(BUILD_DIR)/dp_server_rdma"

$(BUILD_DIR)/dp_client_rdma: $(CLIENT_SRC) $(COMMON_SRC) $(TCP_SRC) $(RDMA_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DDP_ENABLE_RDMA $(INCLUDES) $(RDMA_CFLAGS) -o $@ $^ -lpthread $(RDMA_LIBS)

$(BUILD_DIR)/dp_server_rdma: $(SERVER_SRC) $(COMMON_SRC) $(TCP_SRC) $(RDMA_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DDP_ENABLE_RDMA $(INCLUDES) $(RDMA_CFLAGS) -o $@ $^ -lpthread $(RDMA_LIBS)

clean:
	rm -rf $(BUILD_DIR)
