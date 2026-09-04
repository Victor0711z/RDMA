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

**结论**：真机验证退化成"每台机器单网卡、单路径"，双网卡负载均衡/带宽聚合这个
设计仍然成立，但只能在本地 TCP loopback 环境完整演示，真机只验证单路径硬件正确性。
`README.md` 和 `TALKING_POINTS.md` 已同步更新这个说法。

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
- 聚合吞吐：104.45 MB/s

**观察到的现象、值得记录**：

1. 传输完成（数据已全部确认）之后，日志打印了一条 `WARN *** path 0 故障 (recv
   error) ***`，但故障转移分片数为 0，说明这是连接正常关闭时的收尾时序问题
   （客户端发完关闭信号、接收线程还阻塞在 `recv` 上，连接一断触发了这条日志），
   不是数据丢失。跟 `TALKING_POINTS.md` 第10条 SIGPIPE 踩坑属于同一类"收尾
   时序"问题，可以作为面试补充案例。
2. 应用层吞吐（104 MB/s）明显低于硬件基线（1817 MB/s），差距约 17 倍。这在
   预期内：小文件（50MB）、`window=8` 限制了同时在途的分片数、逐片 ACK 确认，
   协议开销和往返延迟主导了速度，没有跑到硬件线速。这与 `README.md` "已知
   限制"里"ACK 逐片确认、量大时有开销"的描述吻合，是真实观测数据，比纯理论
   描述更有说服力，也是"下一步能怎么优化"这个问题的现成答案（攒批确认 ACK、
   增大 window、增大 chunk_size 都是方向）。

## 待办 / 未完成

- [ ] 文件完整性校验（`md5sum` 对比 client 端原始文件和 server 端 `/data/recv/`
      下收到的文件）——已给出校验步骤，结果待确认后补充到此处
- [ ] 故障转移在真机上的实测（目前只在本地 TCP loopback 验证过，真机因为只有
      单路径，没有第二条真实链路可以模拟故障）
- [ ] 测试完成后记得释放所有测试用的 ECS 实例（包括中途失败、重建的那几批），
      避免持续计费
