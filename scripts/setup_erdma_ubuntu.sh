#!/usr/bin/env bash
# setup_erdma_ubuntu.sh - 在阿里云 eRDMA 机器（Ubuntu）上准备编译环境
#
# 用法：把整个项目传到两台 eRDMA 实例上，各自执行一次这个脚本，
# 然后按 README / docs/RUN_CHECKLIST.md 跑单 HCA 多 QP 实验。
#
# 这个脚本只负责"装编译依赖 + 编译 RDMA 版本"，eRDMA 驱动本身要按阿里云官方
# 文档先装好（一键安装脚本，装完需要重启）：
#   https://help.aliyun.com/zh/ecs/user-guide/erdma-usage/
# 装完驱动、重启之后再跑这个脚本。

set -euo pipefail
cd "$(dirname "$0")/.."

echo "== 检查 eRDMA 设备是否已经就绪 =="
if ! command -v ibv_devinfo >/dev/null 2>&1; then
    echo "[警告] 还没有 ibv_devinfo 命令，说明 eRDMA 驱动可能还没装/没重启。"
    echo "        请先参考阿里云官方文档装好驱动并重启，再跑这个脚本。"
fi

echo
echo "== 安装编译依赖 =="
sudo apt update
sudo apt install -y build-essential libibverbs-dev librdmacm-dev ibverbs-providers perftest

echo
echo "== ibv_devinfo（应该能看到至少 2 个 RDMA 设备，对应两张 ERI） =="
ibv_devinfo || echo "[警告] ibv_devinfo 失败，说明 eRDMA 驱动没有正常工作"

echo
echo "== 编译 RDMA 版本 =="
make rdma

echo
echo "== 完成 =="
echo "接下来："
echo "  1) 先用标准工具验证链路: ib_write_bw （server端）/ ib_write_bw <server_ip> （client端）"
echo "     用两台机器的 RDMA IP 测一次，拿到单 HCA 基线带宽"
echo "  2) 参考 config/client.example.ini / config/server.example.ini 改成两张 ERI 各自的内网 IP"
echo "  3) ./build/dp_server_rdma -c <server配置> -d <保存目录>"
echo "     ./build/dp_client_rdma -c <client配置> -f <要传的文件>"
