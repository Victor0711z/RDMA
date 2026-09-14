# 简历文案（按实测结果填写）

## 当前可用版本

- 基于 C、libibverbs 与 rdma_cm 实现纯 RDMA 多 QP 文件传输引擎，在单 HCA 上管理
  1～8 个 RC QP，使用 RDMA WRITE WITH IMM 与注册内存 slot ring 构建单边数据面。
- 设计 per-QP credit 流控和显式 slot→chunk 生命周期，支持乱序 ACK；实现 QP 故障
  检测、未确认分片重调度、服务端幂等去重及迟到 ACK 过滤，并通过条件变量驱动
  发送调度，避免窗口满时忙轮询。
- 设计版本化线路协议，在 slot header 中携带 32 位 chunk_id、实际长度与 CRC32C，
  并修复 CQ completion 归属、RQ WQE 混用和短任务计时误差等并发/性能问题。
- 构建 QP 数、chunk 大小、window 深度实验矩阵，对照 `ib_write_bw -R`，记录应用吞吐、
  CPU、P50/P95 与故障迁移分片数；实测数据填写为：`待补充`。

## 使用边界

在没有完成真机复测前，不写“提升 XX%”。当前硬件是每台一张 eRDMA 网卡，所以应写
“单 HCA 多 QP”，不能写“双网卡聚合”。项目实现了逐分片 CRC32C，但最终实验仍应用
SHA-256 对比端到端文件一致性。
