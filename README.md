# MultiQP-RDMA File Transfer

一个纯 C、纯 RDMA 的学习型文件传输系统。两台主机各使用一张 RDMA 网卡，在同一
HCA 上建立 1～8 个独立 RC QP，以 `RDMA WRITE WITH IMM` 传输数据，以
`SEND/RECV` 完成握手、ACK 和关闭控制。

这个版本刻意不保留 TCP 后端。项目重点是把 RDMA 数据面做完整：MR/PD/CQ/QP 生命周期、
注册 slot ring、credit 流控、乱序 ACK 下的 slot 所有权、CRC32C、分片去重和 QP 故障重分配。

源码中的 `RDMA_PS_TCP` 是 RDMA CM 为可靠连接使用的 port-space 常量，不是 socket/TCP
后端。若 `ibv_devinfo` 显示设备 transport 为 iWARP，那么硬件/驱动本身采用 RDMA over
TCP，这是云卡属性，应用无法在保留这张卡的前提下改成 RoCE；应用层仍然只调用
RDMA CM 和 Verbs。

> 多 QP 能研究并发队列、CQ/WQE、流控和调度开销，但多个 QP 仍共享一张物理网卡，
> 不能宣称实现了多网卡带宽聚合或独立物理故障域。

## 数据路径

1. 两端通过 RDMA CM，在 `base_port ... base_port+qp_count-1` 建立多个 RC QP。
2. 服务端为每个 QP 注册 `window` 个远端可写 slot，并用 SEND 把 `addr/rkey` 发给客户端。
3. 客户端把文件分片，调度到有 credit 的 QP。
4. 每个 slot 为 `[magic | chunk_id | length | crc32c | payload]`；客户端用
   RDMA WRITE WITH IMM 写入，immediate data 仅携带 slot 编号。
5. 服务端校验 header、长度和 CRC32C，通过后用 `pwrite` 写到对应文件偏移并返回 ACK。
6. ACK 同时释放 scheduler credit 和该 QP 上对应的 slot。若某 QP 失败，未 ACK 分片
   回到 retry queue，交给剩余 QP 重发；服务端用 bitmap 去重。

详细状态和线程关系见 [架构说明](docs/ARCHITECTURE.md)。

## 构建

Ubuntu/Debian：

```bash
sudo apt update
sudo apt install -y build-essential pkg-config libibverbs-dev librdmacm-dev \
  ibverbs-providers perftest
make
```

产物：

```text
build/dp_client_rdma
build/dp_server_rdma
```

`make test` 只运行不需要 RDMA 设备的 scheduler、CRC32C、配置解析和协议网络序测试；
它不会运行网络传输。

```bash
make bench
```

`make bench` 单独测试 CRC32C。x86 CPU 支持 SSE4.2 时日志应显示
`backend=sse4.2`，否则自动使用查表实现。优化前真机数据、瓶颈证据和复测方法见
[`docs/PERFORMANCE_OPTIMIZATION.md`](docs/PERFORMANCE_OPTIMIZATION.md)。

## 配置

分别复制 [客户端示例](config/client.example.ini) 和
[服务端示例](config/server.example.ini)，替换两端 RDMA IP。核心配置：

```ini
[rdma]
local_ip=192.168.10.11
remote_ip=192.168.10.12
base_port=18801
qp_count=4
chunk_size=1048576
window=32
```

- `qp_count`：同一 RDMA 网卡上的 RC QP 数，范围 1～8。
- `chunk_size`：每个分片 payload，范围 4 KiB～64 MiB。
- `window`：每个 QP 的注册 slot 数及最大在途分片数，范围 1～256。
- 两端 `base_port/qp_count/chunk_size` 必须一致。

## 运行

服务端先启动：

```bash
mkdir -p /data/recv
./build/dp_server_rdma -c server.ini -d /data/recv -v
```

客户端再启动：

```bash
./build/dp_client_rdma -c client.ini -f /data/testfile -v
```

不用改配置即可覆盖 QP 数，方便做对照实验：

```bash
./build/dp_client_rdma -c client.ini -f /data/testfile --qp-count 1
./build/dp_client_rdma -c client.ini -f /data/testfile --qp-count 4
```

服务端的 QP 数也必须与客户端一致，所以做该对照时需要同步修改服务端配置并重启。

验证 QP 级重分配（示例：成功 post 100 个分片后关闭 QP1）：

```bash
./build/dp_client_rdma -c client.ini -f /data/testfile --inject-qp-failure 1:100
```

最后在两台机器执行 `sha256sum`，文件哈希必须一致。CRC32C 是逐分片在线校验，
SHA-256 是端到端实验校验，两者目的不同。

## 建议实验矩阵

使用至少 1 GiB 文件，每组预热 1 次、正式运行 5 次，报告中位数和 P95：

| 变量 | 建议值 |
|---|---|
| QP 数 | 1 / 2 / 4 / 8 |
| chunk | 64 KiB / 1 MiB / 4 MiB |
| window | 1 / 8 / 32 / 64 |
| 对照 | 相同消息大小的 `ib_write_bw -R` |
| 观测 | 吞吐、client/server CPU、每 QP 分片数、故障转移分片数 |

阿里云 eRDMA 若显示为 iWARP，`perftest` 需要通过 `-R` 使用 RDMA CM。完整部署步骤见
[真机运行清单](docs/RUN_CHECKLIST.md)，历史踩坑和旧数据见
[部署记录](docs/DEPLOYMENT_LOG.md)。

## 当前边界

- 当前是单会话、单文件、单发送线程；不是通用存储服务。
- QP 故障后会迁移未确认分片，但不在同一会话中重建 QP，也不支持进程重启续传。
- ACK 仍是逐分片 SEND，可继续优化为累计/批量 ACK。
- 软件 CRC32C 和两端 staging buffer 带来内存拷贝与 CPU 开销；这里准确表述为
  “单边 RDMA WRITE、内核旁路”，不是端到端零拷贝。
- 仅适合可信内网，没有加密、鉴权和访问控制。

## 项目结构

```text
include/       线路协议、QP 抽象、调度器与配置
src/common/    调度、协议编解码、CRC32C、配置解析
src/rdma/      RDMA CM + Verbs 数据面
src/client/    分片调度、ACK 线程和故障重分配
src/server/    多 QP 接收、校验、去重和并行 pwrite
tests/         不依赖 RDMA 硬件的算法单元测试
docs/          架构、部署、实验和面试材料
```

## License

MIT
