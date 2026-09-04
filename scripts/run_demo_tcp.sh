#!/usr/bin/env bash
# run_demo_tcp.sh - 一键跑通 TCP 模式的本地演示（不需要任何 RDMA 硬件）
#
# 做了三件事：
#   1) 编译（如果还没编译过）
#   2) 用两个本地端口模拟"双网卡"，跑一次双路径传输，校验文件完整性
#   3) 用 --single-path 只走一条路径跑一次同样的文件，作为吞吐对比基线
#
# 用法: ./scripts/run_demo_tcp.sh [文件大小MB，默认50]

set -euo pipefail
cd "$(dirname "$0")/.."

SIZE_MB="${1:-50}"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "== 1) 编译 TCP 版本 =="
make tcp

echo
echo "== 2) 生成 ${SIZE_MB}MB 随机测试文件 =="
head -c "$((SIZE_MB * 1024 * 1024))" /dev/urandom > "$WORKDIR/testfile.bin"
mkdir -p "$WORKDIR/out"

run_one() {
    local label="$1" server_cfg="$2" client_cfg="$3"; shift 3
    echo
    echo "---- $label ----"
    ./build/dp_server -c "$server_cfg" -d "$WORKDIR/out" > "$WORKDIR/server_${label}.log" 2>&1 &
    local server_pid=$!
    sleep 0.3
    ./build/dp_client -c "$client_cfg" -f "$WORKDIR/testfile.bin" -o "received_${label}.bin" "$@" \
        | tee "$WORKDIR/client_${label}.log"
    wait "$server_pid"
    if cmp -s "$WORKDIR/testfile.bin" "$WORKDIR/out/received_${label}.bin"; then
        echo "[OK] 文件完整性校验通过 ($label)"
    else
        echo "[FAIL] 文件不一致！($label)"
        exit 1
    fi
}

echo
echo "== 3) 双网卡（双路径）传输 =="
run_one dual config/demo_server.ini config/demo_client.ini

echo
echo "== 4) 单网卡基线（--single-path，仅用 path0） =="
run_one single config/demo_single_path.ini config/demo_client.ini --single-path

echo
echo "== 完成：把上面两次运行日志里的\"聚合吞吐\"数字拿来对比，就是本机上的双网卡加速比 =="
echo "   （真实网络环境下的提升幅度需要在有物理双网卡的机器上跑，见 README「真机部署」一节）"
