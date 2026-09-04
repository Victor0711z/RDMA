#!/usr/bin/env bash
# run_failover_demo.sh - 演示"其中一张网卡中途故障，传输自动转移到另一条路径且不丢数据"
#
# 原理：用 scripts/tools/flaky_proxy.py 假扮 path1 那条链路的"网卡"，
# client 实际连的是这个代理，代理转发到真正的 server 端口，并限速；
# 运行一段时间后代理会强行断开所有连接，模拟网卡故障。
# path0 是直连 server 的（代表"正常的那张网卡"），全程不受影响。
#
# 用法: ./scripts/run_failover_demo.sh [文件大小MB，默认50] [几秒后断线，默认0.3]

set -euo pipefail
cd "$(dirname "$0")/.."

SIZE_MB="${1:-50}"
KILL_AFTER="${2:-0.3}"
WORKDIR="$(mktemp -d)"
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do kill -9 "$pid" >/dev/null 2>&1 || true; done
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "== 编译 TCP 版本 =="
make tcp

echo
echo "== 生成 ${SIZE_MB}MB 测试文件 =="
head -c "$((SIZE_MB * 1024 * 1024))" /dev/urandom > "$WORKDIR/testfile.bin"
mkdir -p "$WORKDIR/out"

# server 仍然监听真实的 path0(18801)/path1(18802) 端口
cat > "$WORKDIR/server.ini" <<EOF
[general]
transport=tcp
chunk_size=65536
window=8
[path0]
local_ip=127.0.0.1
remote_ip=127.0.0.1
port=18801
weight=1.0
[path1]
local_ip=127.0.0.1
remote_ip=127.0.0.1
port=18802
weight=1.0
EOF

# client 的 path1 连的是代理端口 18902，代理再转发到 server 真正的 18802
cat > "$WORKDIR/client.ini" <<EOF
[general]
transport=tcp
chunk_size=65536
window=8
[path0]
local_ip=127.0.0.1
remote_ip=127.0.0.1
port=18801
weight=1.0
[path1]
local_ip=127.0.0.1
remote_ip=127.0.0.1
port=18902
weight=1.0
EOF

echo
echo "== 启动 server =="
./build/dp_server -c "$WORKDIR/server.ini" -d "$WORKDIR/out" -v > "$WORKDIR/server.log" 2>&1 &
PIDS+=($!)
sleep 0.3

echo "== 启动模拟故障的代理（path1，${KILL_AFTER}s 后断线）=="
python3 scripts/tools/flaky_proxy.py 18902 18802 "$KILL_AFTER" > "$WORKDIR/proxy.log" 2>&1 &
PIDS+=($!)
sleep 0.1

echo "== 开始传输（观察 path1 故障后 client 会打印\"故障转移\"日志）=="
./build/dp_client -c "$WORKDIR/client.ini" -f "$WORKDIR/testfile.bin" -o received.bin -v | tee "$WORKDIR/client.log"

wait "${PIDS[0]}" 2>/dev/null || true

echo
if cmp -s "$WORKDIR/testfile.bin" "$WORKDIR/out/received.bin"; then
    echo "[OK] path1 中途故障后，文件依然完整（故障转移生效）"
else
    echo "[FAIL] 文件不一致，故障转移没有正确工作"
    exit 1
fi

echo
echo "关键日志："
grep -E "故障|状态=|聚合总量" "$WORKDIR/client.log" || true
