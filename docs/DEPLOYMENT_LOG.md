# 真机部署记录（阿里云 eRDMA，2026-09-04）

这份文档记录第一次在阿里云真实 eRDMA 硬件上部署、调试、跑通这个项目的完整过程，
包括踩过的坑和最终结果。跟 [`TALKING_POINTS.md`](TALKING_POINTS.md) 配合看——
这里是"发生了什么"，那份是"怎么讲给面试官听"。

## 环境

- 地域可用区：华东2（上海）可用区 F
- 规格：`ecs.g8i.xlarge`（4 vCPU / 16 GiB）
- 镜像：Ubuntu 24.04 64 位
- VPC/交换机：`erdma-vpc` / `erdma-vswitch-01`（`192.168.0.0/24`）
- 两台实例：client、server，同 VPC 同可用区

## 踩过的坑（按遇到的顺序）

### 1. `g8i.xlarge` 一台实例只能挂 1 张 eRDMA 网卡，不是 2 张

最初按 README 旧版描述"每台机器挂 2 张 ERI"去配置，购买页面"辅助网卡"那一栏
明确提示"创建实例时只能附加 1 块弹性网卡"，且辅助网卡本身不带"弹性RDMA接口"
选项——也就是说这个规格上，一台实例最多只有 1 张网卡能开 eRDMA。

**结论**：项目已调整为与现有硬件一致的“每台单 RDMA 网卡、单 HCA 多 QP”模型。
它研究 QP 并发、流控与 QP 级故障重分配，不再宣称双网卡带宽聚合。

### 2. ERI 硬件挂载偶发失败，需要重建实例

前两次创建的实例，购买页面勾选了"开启弹性RDMA接口"，但登录后 `lspci` 只能看到
一个普通的 `Virtio network device`，看不到任何 RDMA 相关 PCI 设备，`ibv_devinfo`
报 "No IB devices found"。两次独立新建都复现了同样的问题，怀疑是后台置备的偶发
问题（不是配置错误）。第三次重新创建后才成功挂载上 `erdma_0` 设备。

**排查方法**：`lspci | grep -iE "ethernet|infiniband|erdma"` 看硬件层面有没有对应
PCI 设备，这个不依赖驱动是否安装，是最直接的判断依据。

### 3. RDMA 网卡不会自动配置 IP，且默认是 DOWN 状态

即使 `erdma_0` 设备已经识别（`ibv_devices` 能看到），对应的网卡接口（`eth1`）
在系统里可能是 `state DOWN` 且没有 IPv4 地址（`ip link show` 能看到）。控制台
"网络与安全组" → "辅助网卡" 那一栏能查到阿里云后台已经分配好的私网 IP，但操作
系统这边需要手动配置：

```bash
sudo ip link set eth1 up
sudo ip addr add <控制台看到的私网IP>/24 dev eth1
```

这个配置重启后会丢失（不是持久化配置），但因为是短期测试用完就释放实例，没有
处理成永久配置。

### 4. 实例自定义数据（cloud-init）里的驱动自动安装脚本没生效

购买时配置的"实例自定义数据"里有一段自动下载运行阿里云 eRDMA 驱动安装脚本的
逻辑，但开机时检查 `/var/log/erdma_install.log` 发现文件不存在，说明这段脚本
根本没跑起来。推测原因：实例刚启动那一刻还没有公网 IP（分配公网 IP 是后来手动
补的），驱动安装脚本需要联网下载安装包，联不上网导致静默失败。

**解决**：手动重新执行一遍官方安装脚本：

```bash
curl -O http://mirrors.cloud.aliyuncs.com/erdma/env_setup.sh
sudo bash env_setup.sh
sudo reboot   # DKMS 编译的内核模块需要重启才生效
```

### 5. 编译报错 `pkg-config: not found`

`libibverbs-dev`/`librdmacm-dev`/`build-essential` 都已经装好，但 `make rdma`
时依赖检查这一步用 `pkg-config` 判断库是否存在，而 `pkg-config` 这个工具本身不
在 `build-essential` 里，需要单独装：

```bash
sudo apt install -y pkg-config
```

### 6. `ib_write_bw` 报错 "Failed to modify QP to RTS / Unable to Connect the HCA's through the link"

第一反应怀疑是安全组挡了流量（默认安全组只放行了 ICMP/SSH(22)/RDP(3389)，没有
放行两台机器之间的通用流量），加了一条"允许 `192.168.0.0/24` 全部流量全部端口"
的规则后问题依旧。

**真正原因**：`ibv_devinfo` 显示 `transport: iWARP`，不是 RoCE。iWARP 这种传输
类型的连接建立方式跟 RoCE 不一样，必须用 `rdma_cm` 来建连接，`ib_write_bw`
默认走的是手动交换 QP 参数的方式（对 RoCE 有效），对 iWARP 无效。加上 `-R`
参数（强制走 `rdma_cm`）后立刻跑通：

```bash
# server
ib_write_bw -R
# client
ib_write_bw -R <server的RDMA网卡IP>
```

这一步的安全组规则本身不是必须的（真正的修复是 `-R` 参数），但保留着也没坏处，
两台机器互通更宽松一些。

## 最终结果

### 硬件基线（`ib_write_bw -R`）

| 指标 | 数值 |
|---|---|
| 平均带宽 | 1817.03 MB/s（约 14.5 Gbit/s） |
| 峰值带宽 | 3698.24 MB/s（约 29.6 Gbit/s） |

### 应用层（自己写的 `dp_client_rdma`/`dp_server_rdma`，单路径）

- 测试文件：50 MiB 随机数据
- 分片数：50（chunk_size=1MiB，window=8）
- 传输结果：全部 50 个分片成功确认，故障转移出的分片数 = 0
- 客户端旧报告值：104.45 MB/s（**该值已确认无效，必须重测**）

**观察到的现象、值得记录**：

1. 传输完成（数据已全部确认）之后，日志打印了一条 `WARN *** path 0 故障 (recv
   error) ***`，但故障转移分片数为 0，说明这是连接正常关闭时的收尾时序问题
   （客户端发完关闭信号、接收线程还阻塞在 `recv` 上，连接一断触发了这条日志），
   不是数据丢失。跟 `TALKING_POINTS.md` 第10条 SIGPIPE 踩坑属于同一类"收尾
   时序"问题，可以作为面试补充案例。
2. 旧客户端主线程每次固定睡眠 500ms 后才检查完成时间。50MiB / 0.5s 约为
   104.9 MB/s，与报告的 104.45 MB/s 几乎完全吻合，因此这个结果主要是计时器 bug，
   不能用来判断 ACK/window 是否为瓶颈。代码现已在调度器进入终态时通过条件变量
   立即唤醒并记录时间；所有性能实验都必须基于修复后的版本重跑。
3. 传完后出现的 `recv error` 与 RDMA 接收队列混放两类 WQE 有关：
   `WRITE_WITH_IMM` 与控制 `SEND` 会消费同一个 RQ，旧实现把带缓冲区的控制 WQE
   和零长度“门铃”WQE 混在一起，`DONE` 可能命中零长度 WQE。现已改为统一使用
   带控制缓冲区的 WQE，并按 completion 的 `wr_id` 原样补回。

## 待办 / 未完成

- [ ] 文件完整性校验（`md5sum` 对比 client 端原始文件和 server 端 `/data/recv/`
      下收到的文件）——已给出校验步骤，结果待确认后补充到此处
- [ ] 使用 `--inject-qp-failure 1:100` 完成多 QP 故障重分配实测，并记录
      `chunks_failed_over`、传输完成状态和 SHA-256
- [ ] 测试完成后记得释放所有测试用的 ECS 实例（包括中途失败、重建的那几批），
      避免持续计费
- [ ] 用修复后的计时逻辑重跑至少 1GiB 文件；每组参数预热一次、正式测 5 次，记录
      中位数与 P95，而不是继续引用 104.45 MB/s
- [ ] 分别以 1/2/4/8 个 QP 重测，说明单 HCA 上 QP 数对吞吐和 CPU 的影响
