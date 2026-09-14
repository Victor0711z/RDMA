# 架构说明

## 目标与边界

系统运行在两台各有一张 RDMA 网卡的主机上。每个进程在同一个 RDMA 设备上建立
多个 RC QP，通过队列并行研究 WQE/CQ、窗口和调度行为。它是 multi-QP，不是
multi-NIC；所有 QP 共享 HCA 和物理链路。

## 线程和状态

```text
client                                      server
┌─────────────────────────┐                 ┌─────────────────────────┐
│ 1 sender                │                 │ QP0 recv thread         │
│ scheduler + pread       │                 │ validate + pwrite + ACK │
└────────────┬────────────┘                 ├─────────────────────────┤
             │ choose live QP with credit   │ QP1 recv thread         │
   ┌─────────┼─────────┐                    │ validate + pwrite + ACK │
   │         │         │                    ├─────────────────────────┤
  QP0       QP1      QP(N-1) ─────────────▶ │ ...                     │
   │         │         │                    └─────────────────────────┘
 ack0      ack1     ack(N-1)
   └─────────┴─────────┘
        release credit/slot
```

每个逻辑 chunk 在 scheduler 中只有三类状态：未分配、归属于某个 QP 且未 ACK、已完成。
`chunk_owner[]` 是重分配和忽略迟到 ACK 的依据。失败 QP 名下的 chunk 被放进环形
`retry_queue`，随后只能由仍为 UP 且有 credit 的 QP 获取。

当所有 QP 的窗口都满时，sender 不做固定间隔轮询，而是在 scheduler 条件变量上
休眠；ACK 释放 credit 或 QP 故障产生 retry 工作时再唤醒，减少空闲 CPU 消耗。

## 建链与内存注册

每个 QP 独立执行 RDMA CM 地址解析、路由解析、QP 创建和连接。服务端为该 QP 注册：

```text
recv_slots MR = window × (slot_header + chunk_size)
```

MR 具有 `LOCAL_WRITE | REMOTE_WRITE` 权限。服务端通过 SEND 把基址、rkey、slot 大小和
slot 数发给客户端。客户端也注册同布局的本地 staging slot，作为 RDMA WRITE 的 SGE。

## slot 协议

```text
offset  size  field
0       4     magic
4       4     chunk_id
8       4     payload length
12      4     CRC32C
16      N     payload
```

四个 header 字段使用网络字节序。WRITE_WITH_IMM 的 32 位 immediate data 只传 slot
下标，因此 chunk_id 可完整使用 32 位。服务端收到 `IBV_WC_RECV_RDMA_WITH_IMM` 后：

1. 检查 slot 下标、magic、chunk_id 范围和长度；
2. 根据文件大小计算该 chunk 的期望长度；
3. 对 payload 计算 CRC32C；
4. 校验通过后复制到线程缓冲区并 `pwrite`；
5. 去重计数并回 ACK。

## credit 与 slot 生命周期

每个 QP 初始有 `window` 个 credit。scheduler 分配一个 chunk 时消耗一个 credit；收到
该 chunk ACK 时恢复一个 credit。客户端同时维护 `slot_in_use[]` 与 `slot_chunk[]`：

- post WRITE 前原子地认领空闲 slot；
- ACK 带回 chunk_id，找到对应 slot 后释放；
- ACK 即使乱序，也不会覆盖仍未确认的 slot；
- QP 失败后其未 ACK chunk 回到 retry queue，原 QP 不再复用。

## CQ 与 RQ 注意点

- SEND 和 WRITE_WITH_IMM 共用 QP 的 Receive Queue。所有预投递 RECV WQE 都使用同构、
  足够大的控制缓冲区，避免 DONE 命中零长度 WQE导致 `LOC_LEN_ERR`。
- 数据 WRITE 使用 signaled WR；发送 CQ 会被持续 drain，控制 SEND 通过唯一 wr_id 等待
  自己的 completion，避免把更早的数据 completion 当成控制消息完成。
- client 每个 QP 有一个 ACK 线程；server 每个 QP 有一个接收线程，因此同一 CQ 不会
  被多个消费者无序竞争。

## 一致性语义

这是 at-least-once 传输加服务端幂等去重：故障边界附近，同一 chunk 可能在旧 QP 已经
到达、又在新 QP 重发。服务端 `received[chunk_id]` 只让首个成功分片计数，重复内容不
重复累计。客户端只接受与当前 `chunk_owner` 匹配的 ACK，迟到 ACK 不会重复完成。

当前没有持久化 session/chunk bitmap，所以进程崩溃后不能续传；`session_id` 和协议
版本字段为后续恢复协议保留了握手位置。
