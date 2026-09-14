# 面试讲解清单

## 30 秒版本

我实现了一个纯 RDMA 的多 QP 文件传输引擎。两台机器各用一张 RDMA 网卡，在同一
HCA 上创建多个 RC QP；数据走 RDMA WRITE WITH IMM，控制面走 SEND/RECV。系统用
注册 slot ring 和 credit 控制在途请求，逐分片 CRC32C，并在单个 QP 出错时把未 ACK
分片重新调度到剩余 QP。服务端用 bitmap 去重，保证故障边界上的重复发送不会重复计数。

## 为什么是多 QP

这两台云机每台只能挂一个 RDMA 设备，所以不伪装成双网卡项目。多 QP 的价值是学习
并验证队列并发、CQ/WQE、窗口深度、软件调度和单 QP 故障隔离。多个 QP 仍共享一张
HCA，不等于物理带宽叠加，也不提供独立链路故障域。

## 为什么用 WRITE WITH IMM

WRITE 让 NIC 把 payload DMA 到对端已经注册并授权的 MR；immediate data 又能在对端
产生 CQE，告诉 CPU 哪个 slot 已到达。相比纯 WRITE 后轮询内存，它有明确通知；相比
SEND/RECV 承载整个 payload，它展示了单边数据面和 rkey/remote address 语义。

## 为什么还要 slot header

immediate data 只有 32 位，只放 slot index。header 保存完整 32 位 chunk_id、实际长度
和 CRC32C，解决最后一片长度、应用层完整性和 chunk_id 位数限制。所有字段都使用网络序。

## 流控和乱序 ACK

每个 QP 的 credit 等于 slot 数，最多只有 window 个 chunk 在途。但仅有计数还不够：
ACK 可能乱序，简单 `next_slot % window` 会覆盖仍未确认的 slot。所以实现还维护
slot→chunk 所有权，只有该 chunk 的 ACK 到达才真正释放对应 slot。

窗口满时 sender 会等待条件变量，由 ACK 或 QP 状态变化唤醒，而不是固定 1ms 轮询，
避免高吞吐传输中的无效 CPU 消耗。

## QP 故障如何处理

调度器维护 `chunk_owner[]`。某个 QP 读写失败后被标为 DOWN，它拥有但尚未 ACK 的
chunk 全部进入 retry queue；其余 QP 继续领取。故障附近可能发生重复到达，服务端
用 bitmap 去重，客户端忽略与当前 owner 不匹配的迟到 ACK。可用
`--inject-qp-failure Q:N` 做可重复的故障实验。

## 准确描述“零拷贝”

网络数据面是单边 RDMA WRITE 和内核旁路，但当前 client 会从文件 `pread` 到用户缓冲，
再复制进注册 slot；server 也会从注册 slot 复制到 pwrite 缓冲。因此不能声称端到端
零拷贝。后续可以用注册文件缓存、固定缓冲池或存储直通方向减少复制。

## 真正踩过的坑

- 固定 500ms 轮询把短任务耗时人为抬高；改为条件变量在全部 ACK 时立即记录时间。
- SEND 与 WRITE_WITH_IMM 消费同一个 RQ；混放零长度和有缓冲 WQE 会让控制消息触发
  `LOC_LEN_ERR`，现统一为同构 RECV WQE。
- ACK 乱序使轮转 slot 可能提前复用；现用显式 slot 所有权修复。
- immediate 编码 chunk_id 会把协议限制在 24 位；现 immediate 只放 slot，chunk_id
  移入 slot header。

## 后续可继续做什么

优先级建议：批量/累计 ACK；事件 CQ 替代忙轮询；每 QP 独立 sender 或批量 post WR；
基于实时吞吐和 CQ 延迟的自适应权重；持久化 session bitmap，实现断点续传和 QP 重建。
