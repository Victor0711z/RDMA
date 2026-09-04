#!/usr/bin/env python3
"""故障转移演示/测试工具：一个会"中途断线"的 TCP 转发代理。

没有真实双网卡时，用它模拟"其中一张网卡忽然故障"这件事：
监听 listen_port，把流量转发到 127.0.0.1:target_port（并限速，让传输过程拉长
到能观察到故障转移），运行 kill_after 秒之后强行关闭所有连接并退出。

用法: python3 flaky_proxy.py <本地监听端口> <转发目标端口> <多少秒后断线>
配合 scripts/run_failover_demo.sh 使用；本身只是测试工具，不是 client/server
架构的一部分。
"""
import socket, threading, sys, time, os

listen_port = int(sys.argv[1])
target_port = int(sys.argv[2])
kill_after = float(sys.argv[3])
rate_bytes_per_sec = 3 * 1024 * 1024  # 限速到 3MB/s，让传输过程拉长，方便中途“拔网线”

conns = []
conns_lock = threading.Lock()
stop_flag = threading.Event()

def relay(src, dst):
    try:
        while not stop_flag.is_set():
            data = src.recv(8192)
            if not data:
                break
            dst.sendall(data)
            time.sleep(len(data) / rate_bytes_per_sec)
    except OSError:
        pass
    finally:
        try: dst.shutdown(socket.SHUT_WR)
        except OSError: pass

def handle(client_sock):
    try:
        server_sock = socket.create_connection(("127.0.0.1", target_port))
    except OSError as e:
        print(f"[proxy:{listen_port}] connect to target failed: {e}", flush=True)
        client_sock.close()
        return
    with conns_lock:
        conns.append(client_sock)
        conns.append(server_sock)
    t1 = threading.Thread(target=relay, args=(client_sock, server_sock), daemon=True)
    t2 = threading.Thread(target=relay, args=(server_sock, client_sock), daemon=True)
    t1.start(); t2.start()

def killer():
    time.sleep(kill_after)
    print(f"[proxy:{listen_port}] *** 模拟网卡故障：强行断开所有连接 ***", flush=True)
    stop_flag.set()
    with conns_lock:
        for s in conns:
            try: s.close()
            except OSError: pass
    try: lsock.close()
    except OSError: pass
    time.sleep(0.05)
    os._exit(0)  # 强制退出，不依赖主线程的 accept() 自行解除阻塞（这只是测试工具，不追求优雅）

lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
lsock.bind(("127.0.0.1", listen_port))
lsock.listen(4)
print(f"[proxy:{listen_port}] forwarding to 127.0.0.1:{target_port}, will fail after {kill_after}s", flush=True)

threading.Thread(target=killer, daemon=True).start()

try:
    while not stop_flag.is_set():
        try:
            c, _ = lsock.accept()
        except OSError:
            break
        threading.Thread(target=handle, args=(c,), daemon=True).start()
except KeyboardInterrupt:
    pass
