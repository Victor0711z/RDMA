# 真机测试前置清单（每次开新实例跑测试之前照这个走一遍）

这份文档是"下次要重新跑真机测试时，从零开始要做哪些准备、怎么连上机器、
要敲哪些命令"的操作清单。踩过的坑、报错原因见
[`DEPLOYMENT_LOG.md`](DEPLOYMENT_LOG.md)，这里只给"正常情况下应该走的步骤"，
遇到报错回去翻那份文档对号入座。

> 前提：实例用完之后一般会释放掉（避免持续扣费），所以**每次重新测试基本等于
> 从"买实例"这一步重新开始**，下面按顺序走。

## 0. 开始前确认

- [ ] 阿里云账号里没有残留的、上次测试忘记释放的 ECS/EIP/云盘（先去控制台确认一下，
      避免重复扣费）
- [ ] 本地（Windows）能找到上次用的 SSH 私钥文件（如果是新建的密钥对，重新下载一份）

## 1. 买实例（每次都要做）

- 地域可用区：华东2（上海），选一个明确支持 `ecs.g8i.xlarge`（或同系列支持 eRDMA
  的规格）的可用区，例如可用区 F
- 规格：`ecs.g8i.xlarge`（4 vCPU / 16 GiB）
- 镜像：Ubuntu 24.04 64 位
- 买 **两台**，同一个 VPC、同一个交换机（子网）、同一个可用区
- 网络这一栏：勾选"弹性RDMA接口"/开启 eRDMA（**这个规格一台实例只能挂 1 张
  eRDMA 网卡，不需要也不能挂 2 张**，购买页面辅助网卡不支持 eRDMA，见
  DEPLOYMENT_LOG 踩坑1）
- 记得分配公网 IP（否则连不上、驱动脚本联不了网）
- 安全组：先用默认的，后面按需放行（第 5 步会说）
- 两台实例分别当 **client** 和 **server** 用，装好后各自记录：
  - 公网 IP（SSH 登录用）
  - 内网主网卡 IP（一般用不上）
  - eRDMA 网卡（辅助网卡）的私网 IP（控制台"网络与安全组"里能看到，后面配置文件要用）

> 如果开机后 `lspci` 看不到 RDMA 设备、`ibv_devinfo` 报 "No IB devices found"，
> 大概率是后台置备偶发失败，直接重建一台实例，不用排查配置（见踩坑2）。

## 2. 怎么连上机器（WSL Ubuntu）

私钥文件一般是从浏览器下载到 Windows 那边的（比如 `Downloads`），WSL 里访问
Windows 盘是 `/mnt/c/...`，但**直接用 `/mnt/c/...` 里的私钥会因为权限太开被
ssh 拒绝**（`/mnt/c` 挂载出来的文件默认权限是 777），所以先复制一份到 WSL
自己的家目录再改权限：

```bash
# 私钥只需要复制一次，之后每次连都能用，除非重新生成了密钥对
cp /mnt/c/Users/Victor/Downloads/<你的私钥文件>.pem ~/
chmod 400 ~/<你的私钥文件>.pem
```

然后连接（client、server 各开一个 WSL 终端分别连）：

```bash
ssh -i ~/<你的私钥文件>.pem root@<实例公网IP>
```

- 如果重建过实例、公网 IP 复用了旧 IP，会报 host key 变化的警告，正常现象，按提示
  执行一遍 `ssh-keygen -f ~/.ssh/known_hosts -R '<公网IP>'` 再连
- 连不上先看安全组有没有放行 22 端口（默认安全组一般已经放行）
- 如果 `chmod` 之后还是报 "UNPROTECTED PRIVATE KEY FILE"，说明私钥还在
  `/mnt/c` 底下没复制成功，回去确认第一步真的 `cp` 到 `~/` 了

## 3. 每台机器上都要做一遍：装驱动、配网卡、装依赖、编译

按顺序，**server 和 client 都要各跑一遍**：

```bash
# 3.1 装 eRDMA 驱动（一键脚本），装完必须重启
curl -O http://mirrors.cloud.aliyuncs.com/erdma/env_setup.sh
sudo bash env_setup.sh
sudo reboot
```

重启、重新 SSH 进去之后：

```bash
# 3.2 确认设备已经识别（应该能看到 erdma_0，状态 PORT_ACTIVE）
ibv_devices
ibv_devinfo
```

```bash
# 3.3 配置 RDMA 网卡（eth1）的 IP —— 每次重启都要重新配，不是持久化配置
sudo ip link set eth1 up
sudo ip addr add <控制台看到的eRDMA网卡私网IP>/24 dev eth1
ip addr show eth1   # 确认配上了
```

```bash
# 3.4 装编译依赖 + 编译 RDMA 版本
git clone <你的仓库地址>   # 或者直接把本地这份代码传上去
cd rdma-dual-nic-transfer
sudo apt update
sudo apt install -y build-essential libibverbs-dev librdmacm-dev ibverbs-providers perftest pkg-config
make rdma
```

> `pkg-config: not found` 是常见坑（build-essential 不包含它），单独
> `sudo apt install -y pkg-config` 就好，见踩坑5。

## 4. 先用标准工具验证硬件基线（不涉及本项目代码）

server 端：

```bash
ib_write_bw -R
```

client 端（换成 server 的 eRDMA 网卡私网 IP）：

```bash
ib_write_bw -R -s 1048576 <server的eRDMA网卡私网IP>
```

- **必须带 `-R`**：`ibv_devinfo` 显示 `transport: iWARP`，走 `rdma_cm` 建连接，
  不带 `-R` 会报 "Failed to modify QP to RTS"（见踩坑6）
- `-s 1048576` 是把测试消息大小对齐到项目默认的 `chunk_size`（1MiB），跟默认
  64KB 的基线比意义不大，两个都测一下留档最好
- 这一步跑通说明硬件、驱动、网络都没问题，后面应用层测出问题就不用怀疑硬件了

如果这一步失败：先看 `dmesg`，再检查两台机器是不是真的用 eRDMA 网卡的 IP
互通（`ping` 一下），安全组一般不是根因（见踩坑6的说明）。

## 5.（可选，一般不必须）安全组放行

默认安全组通常已经够用（`ib_write_bw -R` 能跑通就说明够了）。如果不放心，
可以在两台实例的安全组各加一条：

- 授权对象：`192.168.0.0/24`（或者实际用的交换机网段）
- 协议端口：全部协议 / 全部端口

## 6. 改配置文件

`config/client.example.ini` 复制一份改成 `client.ini`，`server.example.ini`
复制一份改成 `server.ini`，双方各自站在自己的角度填：

```ini
[rdma]
local_ip=<本机 eRDMA 网卡私网IP>
remote_ip=<对端 eRDMA 网卡私网IP>
base_port=18801
qp_count=4
chunk_size=1048576
window=32
```

`qp_count=4` 会建立 4 个 RC QP，分别使用 18801～18804。两端的 `base_port`、
`qp_count` 和 `chunk_size` 必须一致。

## 7. 跑起来

server 先起：

```bash
./build/dp_server_rdma -c server.ini -d /data/recv
```

client 再起：

```bash
dd if=/dev/urandom of=/data/testfile bs=1M count=1024 # 至少 1GiB，避免短任务计时失真
./build/dp_client_rdma -c client.ini -f /data/testfile
```

> 阿里云 ECS 系统盘小、容易被清空/受限，**下载/生成的测试文件放在 `/data` 目录下**，
> 不要放在默认 home 目录。

## 8. 测完必须做的事

- [ ] `sha256sum` 对比 client 端原始文件和 server 端 `/data/recv/` 下收到的文件，
      确认完整性：
      ```bash
      sha256sum /data/testfile          # client 端跑
      sha256sum /data/recv/testfile     # server 端跑，两个值要一样
      ```
- [ ] 记录本次的吞吐数据（跟 `docs/DEPLOYMENT_LOG.md` 里的表格对照）
- [ ] **测完立刻释放两台实例**（连同 EIP、云盘），不是"停止"，避免持续扣费

## 9. 修复计时问题后的性能矩阵

旧的 104.45 MB/s 主要来自固定 500ms 轮询造成的计时下限，已经作废。使用至少
1GiB 文件，每个配置预热 1 次、正式测 5 次，记录中位数和 P95：

1. `chunk_size`: 64KiB / 1MiB / 4MiB。
2. `window`: 1 / 8 / 32 / 64。
3. 同时记录 client/server 的 CPU（如 `pidstat -p <pid> 1`），不能只报吞吐。
4. 使用相同消息大小跑 `ib_write_bw -R`，区分硬件上限和应用层开销。

## 10. QP 故障重分配实验

确保两端 `qp_count` 至少为 2。服务端正常启动，客户端使用内置、可重复的故障注入：

```bash
./build/dp_client_rdma -c client.ini -f /data/testfile --inject-qp-failure 1:100
```

含义是成功 post 100 个分片后关闭 QP1。记录 client 的 QP1 DOWN 日志、
`chunks_failed_over`、总耗时，并检查最终 `sha256sum`。这验证的是同一 HCA 内 QP 级
错误处理和重调度；若直接关闭 `eth1`，所有 QP 会一起失效，不能完成迁移。

## 命令速查（复制粘贴用）

| 步骤 | 命令 |
|---|---|
| 装驱动 | `curl -O http://mirrors.cloud.aliyuncs.com/erdma/env_setup.sh && sudo bash env_setup.sh && sudo reboot` |
| 配网卡 | `sudo ip link set eth1 up && sudo ip addr add <IP>/24 dev eth1` |
| 装依赖 | `sudo apt update && sudo apt install -y build-essential libibverbs-dev librdmacm-dev ibverbs-providers perftest pkg-config` |
| 编译 | `make rdma` |
| 硬件基线（server） | `ib_write_bw -R` |
| 硬件基线（client） | `ib_write_bw -R -s 1048576 <server IP>` |
| 起 server | `./build/dp_server_rdma -c server.ini -d /data/recv` |
| 起 client | `./build/dp_client_rdma -c client.ini -f /data/testfile` |
| 校验完整性 | `sha256sum /data/testfile` / `sha256sum /data/recv/testfile` |
