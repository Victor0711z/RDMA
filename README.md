# DualPath-RDMA

基于双网卡（多路径）的负载均衡与故障转移高速数据传输系统。核心调度算法用
纯 C 实现，与传输层解耦，同时提供 **TCP 后端**（不需要任何特殊硬件，随便一台
机器就能跑，用来验证正确性/本地演示）和 **RDMA Verbs 后端**（`libibverbs` +
`rdma_cm`，需要真实 RDMA 网卡，比如阿里云 eRDMA）。

## 这个项目解决什么问题

单张网卡带宽有限、单点故障会导致传输中断。这个项目让 client 把一个文件切成
很多分片，通过两条独立的网络路径（对应两张网卡）并发发送：

- **负载均衡**：按网卡带宽权重（加权公平队列）动态分配分片，而不是简单对半分
- **故障转移**：某条路径中途故障，未完成的分片自动改走另一条路径，不中断、不丢数据
- **传输层可插拔**：同一套调度算法，RDMA 硬件不可用时可以直接切到 TCP 后端

设计细节、每个决策"为什么这么做"，见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)。
准备简历/面试用的讲解材料在 [`docs/RESUME_BULLETS.md`](docs/RESUME_BULLETS.md)
和 [`docs/TALKING_POINTS.md`](docs/TALKING_POINTS.md)。真机部署的完整过程和踩坑
记录在 [`docs/DEPLOYMENT_LOG.md`](docs/DEPLOYMENT_LOG.md)。

## 快速开始（本地演示，不需要任何 RDMA 硬件）

```bash
git clone <你的仓库地址>
cd rdma-dual-nic-transfer

# 一键跑通：编译 -> 双路径传输 -> 单路径基线对比 -> 文件完整性校验
./scripts/run_demo_tcp.sh

# 演示故障转移：传输过程中人为掐断一条路径，验证自动切换、文件依然完整
./scripts/run_failover_demo.sh
```

> 本地 loopback 环境下，两条"路径"共享同一个网卡/内核协议栈，所以这个 demo
> 主要用来验证**正确性**（调度对不对、故障转移灵不灵），**不会**体现出真实的
> 吞吐提升——真实的双网卡加速比需要在有两条独立物理链路的环境下测，见下面
> 「真机部署」一节。这是诚实的局限，不是 bug。

也可以手动跑：

```bash
make tcp
./build/dp_server -c config/demo_server.ini -d /tmp/recv_dir
./build/dp_client -c config/demo_client.ini -f <你的文件>
```

## 项目结构

```
include/           公共头文件（协议、调度器、传输层接口、配置解析）
src/common/        调度算法、协议编解码、配置解析、工具函数（跟传输方式无关）
src/tcp/           TCP 传输后端实现
src/rdma/          RDMA Verbs 传输后端实现（需要 libibverbs/librdmacm）
src/client/        客户端主程序
src/server/        服务端主程序
scripts/           一键演示脚本、eRDMA 环境部署脚本、故障转移测试工具
config/            配置文件示例
docs/              架构说明、简历文案、面试讲解清单
```

## 配置文件

client 和 server 各自一份配置（各自站在自己的视角填 `local_ip`），格式和字段
说明见 [`config/client.example.ini`](config/client.example.ini) 和
[`config/server.example.ini`](config/server.example.ini) 里的注释。

## 关于兼容性：这份 RDMA 代码能直接在阿里云 eRDMA 上用吗？

能。`src/rdma/rdma_path.c` 用的是标准 `libibverbs` + `rdma_cm`，走 RC
（Reliable Connection）queue pair，这正是阿里云 eRDMA 原生支持的编程模型
（官方文档确认 eRDMA 支持 RC QP、支持标准 `rdma_cm` 连接管理，标准的
`ib_write_bw`/`perftest` 工具都能直接跑，不需要专有 SDK）——这一点和 AWS
的 EFA 不一样，EFA 底层是 SRD queue pair，标准 verbs/`rdma_cm` 代码在 EFA
上跑不通，必须换用 libfabric。也就是说这份代码**不需要为了适配 eRDMA 做
改动**，直接按下面「真机部署」的步骤编译部署即可。

参考: [Alibaba Cloud eRDMA 官方文档](https://www.alibabacloud.com/help/en/ecs/user-guide/elastic-rdma-erdma/)

**唯一需要在真机上留意的点**：eRDMA 对内存注册（`ibv_reg_mr`）可能有最小/
最大尺寸的限制（官方资料里提到的量级是几个 GB），而这份代码默认注册的缓冲区
比较小（`window × chunk_size`，默认 8×1MiB=8MiB）。如果在真机上跑的时候
`ibv_reg_mr` 报错/建链失败，优先怀疑是这个，可以把 `config.ini` 里的
`window` 和 `chunk_size` 调大再试，同时看一下 `dmesg` 里 erdma 驱动的日志。

## 真机部署（阿里云 eRDMA 双网卡）

1. 买两台带 eRDMA 的 ECS（同 VPC、同可用区，各挂 2 张 ERI），装好 eRDMA 驱动
   （阿里云官方一键脚本），重启
2. 两台机器上都执行 `./scripts/setup_erdma_ubuntu.sh`：装编译依赖、`ibv_devinfo`
   检查设备、`make rdma` 编译出 RDMA 版本
3. 先用标准工具 `ib_write_bw` 两张网卡各测一次，拿到硬件基线带宽
4. 参考 `config/client.example.ini` / `config/server.example.ini`，把
   `local_ip`/`remote_ip` 改成两张 ERI 各自的内网 IP，`transport` 改成 `rdma`
5. 分别跑：

   ```bash
   # server 机器
   ./build/dp_server_rdma -c server.ini -d /path/to/save

   # client 机器
   ./build/dp_client_rdma -c client.ini -f <要传的文件>
   # 对比基线：./build/dp_client_rdma -c client.ini -f <文件> --single-path
   ```

6. 记录：单网卡基线吞吐、双网卡聚合吞吐、故障转移测试（传输中手动断开一条
   连接/改安全组临时封端口，观察恢复耗时），多测几次取平均
7. **测完立刻释放实例**（不是停止），连同 EIP、云盘一起清理，避免持续扣费

## Benchmark 结果

真机（阿里云 `ecs.g8i.xlarge`，同 VPC 同可用区）跑出来的结果，完整过程和踩过的坑
见 [`docs/DEPLOYMENT_LOG.md`](docs/DEPLOYMENT_LOG.md)。

| 场景 | 吞吐 | 备注 |
|---|---|---|
| 单网卡硬件基线（`ib_write_bw -R`） | 平均 1817 MB/s，峰值 3698 MB/s | 约 14.5 Gbit/s 平均，29.6 Gbit/s 峰值 |
| 单网卡应用层（`dp_client_rdma`，50MiB 文件） | 聚合吞吐 104.45 MB/s | 50 个分片全部成功，0 次故障转移；应用层吞吐远低于硬件基线，主因是逐片 ACK + `window=8` 限制了在途分片数，细节见部署记录 |
| 双网卡聚合 | 未测（真机只有单网卡，见下方说明） | 双网卡负载均衡/带宽聚合已在本地 TCP loopback 环境验证正确性 |
| 故障转移恢复耗时 | 未测（真机只有单路径） | 已在本地 TCP loopback 环境验证：故障转移生效、文件完整性校验通过 |

> **关于"双网卡"在真机上的说明**：`ecs.g8i.xlarge` 这个规格一台实例最多只能挂
> 1 张开启 eRDMA 的网卡（购买页面辅助网卡不支持 eRDMA），所以真机部分只验证了
> 单路径的硬件正确性和性能基线。多路径负载均衡、故障转移的正确性在本地 TCP
> loopback 环境完整验证过（见上面"快速开始"一节），架构设计仍然成立，只是
> 这次真机没有第二张物理网卡可以拿来做真实的多路径聚合测试。

## 已知限制

- 没有超时重传：只有连接层面报错（对端断开/读写出错）才会触发故障转移，
  链路"静默丢包但连接不报错"的情况目前检测不出来
- 控制消息（尤其是 ACK）是逐个分片确认的，量大时有一定 CPU/消息开销，可以
  优化成攒批确认
- 没有做加密/鉴权，明文传输，只适合内网可信环境
- RDMA 后端在极端初始化失败路径上的资源清理不是 100% 严谨（正常传输路径没
  有这个问题），见 `src/rdma/rdma_path.c` 顶部注释

## License

MIT
