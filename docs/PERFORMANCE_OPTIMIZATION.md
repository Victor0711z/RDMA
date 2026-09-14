# 性能瓶颈定位与优化

## 优化前真机基线

两台阿里云 ECS、单张 eRDMA 网卡、1 GiB 文件、1 MiB 分片、`window=32`：

| 测试 | 结果 |
|---|---:|
| `ib_write_bw -R`（5 次中位数） | 13.11 Gbit/s，约 1639 MB/s |
| 应用 1 QP（5 次中位数） | 90.48 MB/s |
| 应用 4 QP（单次功能测试） | 93.98 MB/s |
| 应用 1 QP + tmpfs | 90.78 MB/s |

tmpfs 与普通文件只相差约 0.3%，排除了物理磁盘作为首要瓶颈。单 QP 的
`ib_write_bw` 已经能跑到 13.11 Gbit/s，而应用从 1 QP 增加到 4 QP 只提升约
3.9%，说明瓶颈位于网卡之前/之后的软件数据路径。

## 已确认的主要瓶颈

1. 旧版 CRC32C 对每个字节执行 8 次逐 bit 循环。1 GiB 文件在 Client 和 Server
   各计算一次，是明显的 CPU 热点。
2. Client 只有一个 sender 线程，所有 QP 共用同一个 `pread -> CRC -> post` 流水线，
   增加 QP 不会增加数据准备并行度。
3. Client 先读入临时缓冲区再复制到注册 MR；Server 又从注册 MR 复制到临时缓冲区，
   每个方向各有一次可避免的大块 memcpy。

## 本轮代码改动

- CRC32C 在 x86 上运行时检测 SSE4.2 并使用硬件指令；其他平台使用 256 项查表回退。
- 新增 `make bench`，独立输出 CRC backend 和吞吐，避免只凭端到端结果猜测。
- Client 启动与 QP 数相同的 sender worker，通过线程安全 scheduler 并行准备数据。
- sender 直接 `pread` 到注册 MR slot，随后原地计算 CRC 并 post RDMA WRITE。
- Server 返回注册接收 slot 的只读视图，校验后直接 `pwrite`，移除中间 memcpy。
- 每个 QP 增加发送锁，保护 send CQ、WR id 和控制消息缓冲区。
- scheduler 新增 8 线程并发领取/ACK 单元测试。
- CI 在 Ubuntu 上编译 RDMA 客户端/服务端、运行全部单测和 CRC smoke benchmark。

## 暂未贸然修改的次要路径

- 每片一个 ACK：1 MiB 分片、13 Gbit/s 下约 1600 ACK/s，当前不是 17 倍差距的主因。
- 每个 WRITE 都 signaled：会增加 CQ 开销，但当前分片较大，先在主瓶颈修复后复测。
- 200 微秒 CQ 空轮询退避：可能影响低延迟和小分片，需用 CPU/吞吐数据决定改成
  completion channel 还是自适应 busy polling。
- 每个 QP 独立 PD/CQ：增加资源占用，但不会解释当前单 QP 与硬件基线的巨大差距。

这些项目应在本轮实测后按 profiler 数据继续优化，避免同时改变过多协议语义，破坏
已经验证过的故障恢复与 slot 所有权正确性。

## 复测顺序

1. `make test && make bench`，确认 `backend=sse4.2` 且所有测试通过。
2. 1 QP、1 GiB、tmpfs 跑 5 次，和 90.48 MB/s 优化前中位数对比。
3. 1/2/4/8 QP 各跑 5 次，记录中位数、P95、CPU。
4. 4 QP 执行 `--inject-qp-failure 1:100`，再次检查 SHA-256。
5. 若仍明显低于硬件基线，再分别实验 ACK batching、selective signaling 和 CQ
   polling 策略，每次只改变一个变量。
