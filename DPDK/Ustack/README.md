# DPDK 用户态协议栈（Ustack）

基于 DPDK 24.11.7 的最小用户态协议栈实验：在 VMware 虚拟机中接管一块
`vmxnet3` 虚拟网卡，轮询收包，解析 **Ethernet II + IPv4 + UDP/TCP/ICMP**，
打印 payload 并回显；实现 **ARP 应答器**（发送端可动态解析 MAC）和
**ICMP Echo Reply**（发送端可以直接 `ping`）。

> 当前状态：UDP 收包/回显、TCP 三次握手/数据回显/FIN 关闭、ARP 请求/应答、
> ICMP Echo Request/Reply（ping）均已测试通过。

这份 README 同时记录了本实验踩过的坑：网卡绑定、大页、VMnet 选择、
ARP 动态解析、混杂模式、vmxnet3 队列配置，以及 `ustack.c` 里几个典型的
收包/解析/回显 bug。

---

## 1. 路径与环境

| 项 | 值 |
|----|----|
| Windows 源码路径 | `D:\daimacheshi\cpp_internet_study\DPDK\Ustack` |
| 虚拟机共享目录 | `/mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack` |
| DPDK 源码路径 | `~/dpdk-stable-24.11.7` |
| DPDK 工具目录 | `~/dpdk-stable-24.11.7/usertools` |
| testpmd | `~/dpdk-stable-24.11.7/build/app/dpdk-testpmd` |
| 可执行文件 | `./build/ustack`（`make` 生成） |
| 网卡 PCI 地址 | `0000:0b:00.0`（按实际机器修改） |
| DPDK 网卡 MAC | 运行 ustack 时打印，例如 `00:0C:29:F4:81:B8`（不是固定值） |
| 测试网段（示例） | 发送端 `10.10.10.10/24`，目的 IP `10.10.10.100` |
| 测试端口（示例） | UDP `9999`（代码不校验端口） |

> **重要**：`shell` 里的路径以虚拟机中的实际路径为准；如果 DPDK 安装位置不同，
> 把 `~/dpdk-stable-24.11.7` 换成你的路径。

---

## 2. 快速开始

```bash
# 0) 每次开机后：大页 + vfio（见第 4、5 节）
# 1) 编译
cd /mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack
make

# 2) 接收端运行（需要 root）
sudo ./build/ustack --iova-mode=pa
#   记下打印的 MAC，例如 00:0C:29:F4:81:B8

# 3) 发送端（另一台与 DPDK 网卡同一 VMnet 的 Linux 虚拟机）
sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
ip route get 10.10.10.100     # 期望：dev ens37 src 10.10.10.10
sudo ip neigh flush dev ens37 # 清掉旧的静态/动态表项
printf 'hello-dpdk' | nc -u -w 1 10.10.10.100 9999
# 第一次发送会先触发 ARP；ustack 回 Reply 后内核自动缓存 MAC
ip neigh show 10.10.10.100    # 期望：lladdr = DPDK MAC，REACHABLE/STALE

# 4) 接收端期望输出（ustack 终端）
# length = 18, data_length = 10, payload = hello-dpdk

# 5) 回显测试：必须绑定固定源端口并等待，nc 发完就退出收不到回显
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.10.10.10", 50000))
s.settimeout(3)
s.sendto(b"hello-dpdk", ("10.10.10.100", 9999))
print(s.recvfrom(2048))
PY
# 期望： (b'hello-dpdk', ('10.10.10.100', 9999))

# 6) TCP 回显测试（ustack 作为 TCP 服务器）
python3 - <<'PY'
import socket
s = socket.create_connection(("10.10.10.100", 9999), timeout=3)
s.sendall(b"hello-tcp")
print(s.recv(2048))
s.close()
PY
# 期望： b'hello-tcp'

# 7) ICMP 测试（ping）
ping -c 4 10.10.10.100
# 第一次会先发 ARP，ustack 回 ARP Reply 后显示 4 个 reply
```

> 不再需要 `ip neigh replace ... nud permanent`；ustack 的 ARP 应答器会自动
> 回复发送端内核的 ARP 请求。想单独验证 ARP：
> `sudo arping -I ens37 -c 3 10.10.10.100`。
> 注意 arping 不写内核邻居表；要让内核自动学习，用 ping/UDP/TCP。

---

## 3. DPDK 基础知识

### 3.1 DPDK 是什么

DPDK（Data Plane Development Kit）是一套用户态高性能网络开发框架，核心思路是
**内核旁路 + 轮询 + 大页 + 零拷贝**：

- 网卡通过 `vfio-pci` / `uio` 绑定给用户态驱动，内核网络栈不再使用它；
- 用户态程序直接读写网卡 DMA 环形队列，不需要系统调用和中断；
- 使用大页内存减少 TLB miss，并保证物理地址连续，便于网卡 DMA；
- 一个 lcore 死循环轮询收包（Poll Mode Driver，PMD），换来低延迟和高吞吐。

代价是：这块网卡在内核里“消失”了——没有 `ensXX` 接口、没有 IP、没有 ARP、
不能 `ping`、不能用 `tcpdump` 直接抓（除非换回内核驱动）。
ARP 由 ustack 在用户态实现（应答器 + 学习表）。

### 3.2 关键概念

| 概念 | 说明 |
|------|------|
| EAL | Environment Abstraction Layer，初始化大页、PCI、vfio、lcore 等，`rte_eal_init()` |
| lcore | DPDK 的逻辑核（通常对应一个 CPU 核），主线程也可以直接轮询 |
| hugepage | 大页内存，DPDK 的 mempool/ring 都放在大页上 |
| mempool / mbuf | 固定大小的报文缓冲区池；`mbuf` 是收发包的基本单位 |
| PMD | Poll Mode Driver，轮询式网卡驱动（本例是 `net_vmxnet3`） |
| ethdev | DPDK 的网卡抽象，提供 port / queue / descriptor 等接口 |
| port | 一个 DPDK 网卡端口，本例 `port 0` |
| RX/TX queue | 收/发队列；每个队列有固定数量的描述符（descriptor） |
| descriptor | 环形队列中的一个槽位，指向一个 mbuf 或一块 DMA 内存 |
| burst | 一次批量收/发多个 mbuf，`rte_eth_rx_burst()` / `rte_eth_tx_burst()` |
| IOVA | I/O 虚拟地址；PA 模式直接用物理地址，VA 模式需要 IOMMU |
| VFIO / UIO | 把 PCI 设备交给用户态的两套机制；现代环境一般用 VFIO |
| promiscuous | 混杂模式，接收目的 MAC 不是自己的帧 |
| RSS | 多队列接收散列；本例只用一个队列，不需要 |
| offload | 校验和、VLAN、TSO 等硬件卸载；本例未启用 |

### 3.3 本程序的初始化流程

```text
rte_eal_init()
  ├── 初始化大页 / PCI / vfio（--iova-mode=pa）
  ├── 探测 DPDK 网卡
  ├── rte_eth_dev_count_avail()          // 确认端口存在
  ├── rte_pktmbuf_pool_create()          // 创建 mbuf 池
  ├── rte_eth_dev_configure(1 RX, 1 TX)  // 端口配置
  ├── rte_eth_rx_queue_setup(512 desc)   // RX 队列
  ├── rte_eth_tx_queue_setup(512 desc)   // TX 队列（回显发送用）
  ├── rte_eth_dev_start()                // 启动端口
  ├── rte_eth_promiscuous_enable()       // 混杂模式
  └── while (1) rte_eth_rx_burst()       // 轮询收包
        ├── ARP(0x0806) -> handle_arp()：学习 IP/MAC、回 ARP Reply
        ├── 解析 Ethernet / IPv4
        ├── TCP(6)  -> handle_tcp()：三次握手、数据回显、FIN/RST
        ├── UDP(17) -> handle_udp()：无状态回显
        └── ICMP(1) -> handle_icmp()：回 Echo Reply（ping）
```

---

## 4. 巨页/大页内存（Hugepages）

DPDK 的 mempool、ring 等必须放在大页内存里。虚拟机里通常使用 2MB 大页。

### 4.1 分配与挂载

```bash
# 分配 1024 个 2MB 大页（2GB），按需要调整
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# 挂载 hugetlbfs（没有 /mnt/huge 就先创建；已挂载则跳过）
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

# 确认
grep Huge /proc/meminfo
```

重点看：

```text
HugePages_Total:    1024
HugePages_Free:     1024
Hugepagesize:       2048 kB
```

### 4.2 常见问题

| 现象 | 原因 / 处理 |
|------|------------|
| EAL 报 `No free hugepages reported` | 没分配大页，或大页被别人占用；检查 `nr_hugepages` 和 `HugePages_Free` |
| 大页数量设置了但程序启动失败 | 内存不足 / 未以 root 运行 / hugetlbfs 没挂载 |
| 重启后失效 | `nr_hugepages` 和挂载不是持久的，每次开机都要重新执行（见第 13 节） |
| 想让 DPDK 自动挂载 | EAL 在部分环境下会自动挂载 hugetlbfs，但显式挂载更可控 |

> 本环境使用 `--iova-mode=pa` + VFIO no-IOMMU，网卡 DMA 使用物理地址，
> 大页提供的物理连续内存正是这里的关键。

---

## 5. 网卡绑定与解绑

### 5.1 绑定到 vfio-pci（DPDK 使用）

```bash
# 加载 vfio-pci，并开启 no-IOMMU 模式（虚拟机环境通常需要）
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode

# 查看网卡 PCI 地址和当前驱动
cd ~/dpdk-stable-24.11.7/usertools
sudo ./dpdk-devbind.py --status

# 绑定（把 0000:0b:00.0 换成实际地址）
sudo ./dpdk-devbind.py --bind=vfio-pci 0000:0b:00.0

# 确认：设备应出现在 "using DPDK-compatible driver" 下
sudo ./dpdk-devbind.py --status
```

绑定后：

- 内核里这块网卡没有 `ensXX` 接口；
- 内核里没有 IP、路由、ARP 应答（ARP 由 ustack 用户态实现）；
- `tcpdump -i ensXX` 抓不到它；
- 只能由 DPDK 程序使用。

### 5.2 解绑 / 换回内核驱动（做对比测试用）

```bash
cd ~/dpdk-stable-24.11.7/usertools

# 从 vfio-pci 解绑
sudo ./dpdk-devbind.py -u 0000:0b:00.0

# 绑定回内核 vmxnet3 驱动
sudo modprobe vmxnet3
sudo ./dpdk-devbind.py --bind=vmxnet3 0000:0b:00.0

# 查看接口名和 MAC
ip -br link
```

换回内核驱动后可以用 `tcpdump` 验证“虚拟交换机到底有没有把帧送过来”，
这是区分 **VMnet 不通** 和 **DPDK 收包异常** 的关键测试（见第 8 节）。

### 5.3 为什么必须加 `--iova-mode=pa`

虚拟机里 VFIO 以 **no-IOMMU（unsafe）模式**运行，没有 IOMMU 做 VA→PA 转换。
网卡 DMA 只能使用物理地址，所以 EAL 必须选择 PA 模式：

```bash
sudo ./build/ustack --iova-mode=pa
```

不加或选 VA 模式，可能出现设备初始化成功但收不到包、DMA 异常等问题。

---

## 6. 两端如何实现正确的网络连接（本实验最重要的部分）

### 6.1 核心原则：同一 VMnet，而不是同一 IP 网段

DPDK 网卡被 vfio 接管后没有内核 IP；ARP 由 ustack 自己实现。发送端要能把帧
送到它，必须满足：

1. **发送端网卡和 DPDK 网卡接在同一个 VMware VMnet（同一个虚拟二层交换机）**；
2. 发送端能解析 DPDK 网卡的 MAC：ustack 会回 ARP Reply，发送端内核自动缓存；
   手工静态邻居仍然可用，但只是可选/调试手段；
3. 发送端的路由确实从“接在同一 VMnet 的那块网卡”出去。

**仅仅 IP 同网段是不够的。** 本实验遇到过：

- 发送端唯一网卡是 **NAT（默认 VMnet8）**；
- 接收端 DPDK 网卡是 **仅主机（默认 VMnet1）**；
- 两边 IP 看起来都是 `192.168.179.x`，但 VMnet8 和 VMnet1 是两个独立的虚拟交换机，
  帧永远到不了 DPDK 网卡。
- 两台虚拟机如果不在同一台物理主机上，NAT 模式也互不相通（各自的 VMnet8 独立）。

VMware 的 VMnet 本质上是一个 **hub**：一块 vNIC 发出的帧会送到同一 VMnet 的
所有其它端口，各 vNIC 按目的 MAC 过滤。因此，只要在同一个 VMnet，
DPDK 网卡就能收到发给它的单播帧。

### 6.2 推荐拓扑

```text
发送端 VM                                         接收端 VM
┌──────────────────┐                              ┌─────────────────────────┐
│ ens33   e1000/NAT│── VMnet8（日常上网/SSH）───│ NAT 网卡（日常网络）      │
│ ens37   仅主机   │── VMnet1（仅主机测试）────│ DPDK 绑定的 vmxnet3      │
└──────────────────┘                              │   （仅主机，同一 VMnet1） │
                                                  │ 其它仅主机网卡（未绑定）  │
                                                  └─────────────────────────┘
```

- 发送端新增一块 **仅主机** 网卡（示例 `ens37`），连接方式和 DPDK 网卡相同；
- 原有 NAT 网卡（示例 `ens33`）保留，不影响日常网络；
- 接收端 DPDK 网卡保持 **仅主机**，和发送端 `ens37` 在同一个 VMnet。

> 在 VMware 设置里核对的是 **VMnet 名称**，不是 IP。DPDK 网卡是哪一块，
> 用它的 MAC（ustack 打印的 MAC）去对应适配器。

### 6.3 发送端配置（不需要 nmcli / 不需要改 NetworkManager）

在本实验环境里，直接使用 `ip addr add` 即可，**不需要 nmcli，也不需要对
NetworkManager 做任何修改**：

```bash
# 1) 给测试网卡配地址（正常配置方式）
sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
# 期望 rc=0

# 2) 确认地址真的加上了
#    注意：ip -br link 只显示链路状态，不显示 IP；必须用 ip -br addr
ip -br addr show ens37
# 期望：ens37  UP  10.10.10.10/24

# 3) 确认路由从 ens37 出去
ip route get 10.10.10.100
# 期望：10.10.10.100 dev ens37 src 10.10.10.10

# 4) 清掉旧的静态/动态邻居，让内核重新走 ARP
sudo ip neigh flush dev ens37
ip neigh show 10.10.10.100
# 期望：为空

# 5) 可选：单独验证 ARP 应答（arping 不写内核邻居表）
sudo arping -I ens37 -c 3 10.10.10.100
# 期望：3 个 Unicast reply，MAC 为 ustack 打印的 DPDK 网卡 MAC

# 6) 让内核自己解析并缓存（ICMP 已实现，ping 会收到 Reply）
sudo ip neigh flush dev ens37
ping -c 1 -W 1 10.10.10.100
ip neigh show 10.10.10.100
# 期望：1 个 reply；lladdr = DPDK MAC，状态 REACHABLE/STALE
```

> 如果 `ip -br addr show ens37` 没有 `10.10.10.10/24`，重新执行第 1 步即可。
> 不需要 `ip neigh replace ... nud permanent`；ARP 应答器会自动回复。
> 静态邻居只作为调试/对比手段保留，见 6.4。
> 这些步骤都是普通 `ip` 命令，和 NetworkManager 无关。

### 6.4 ARP 工作流程（动态解析）

ustack 已经实现 ARP 应答器，正常流程是：

1. 发送端内核要发 UDP/TCP 到 `10.10.10.100`，先查邻居表；
2. 表里没有 MAC，就广播 ARP Request：
   `who-has 10.10.10.100 tell 10.10.10.10`；
3. ustack 的 `handle_arp()` 收到请求：
   - 学习请求者的 IP/MAC；
   - 如果 `target IP == gLocalIp`（10.10.10.100），回 ARP Reply；
4. 发送端内核收到 Reply，把 `10.10.10.100 -> DPDK MAC` 写入邻居表；
5. 之后 UDP/TCP 帧以 DPDK MAC 为目的地址发出。

抓包会看到：

```text
ARP, Request who-has 10.10.10.100 tell 10.10.10.10
ARP, Reply 10.10.10.100 is-at 00:0c:29:f4:81:b8
UDP, ... 10.10.10.10.xxxxx > 10.10.10.100.9999
```

注意：

- `arping` 是用户态工具，只验证 ARP 应答，**不会更新内核邻居表**；
- 要让内核自动缓存，用 `ping`、UDP 或 TCP 触发内核自己发 ARP；
- 如果发送端仍然有 `PERMANENT` 旧表项，内核不会发 ARP，先
  `ip neigh del` / `ip neigh flush`；
- 静态邻居 `ip neigh replace ... nud permanent` 仍然可用，但不再是必需。

### 6.5 发送与验证

```bash
# 发送 10 字节 payload
printf 'hello-dpdk' | nc -u -w 1 10.10.10.100 9999
```

> 第一次发送时，发送端会先发 ARP Request；ustack 回 ARP Reply 后，UDP 才会
> 真正发出。ustack 终端会先打印 `ARP request from ... replying`，再打印 UDP
> 的 `length = ...`。

接收端期望：

```text
length = 18, data_length = 10, payload = hello-dpdk
```

说明：

- `length = 18`：UDP 头 8 + payload 10；
- `data_length = 10`：payload 长度；
- 目的 IP `10.10.10.100` 是随便选的，不需要真实存在；
- 端口 `9999` 也是随便选的，代码不校验端口；
- 以太网帧会被填充到 60 字节，payload 后面的 0 填充不影响输出。

### 6.6 UDP 回显测试

ustack 收到 UDP 后，会把 payload 原样发回**发送端的源端口**。注意：

- 回显的目的端口不是 9999，而是发送端 socket 的源端口；
- `printf ... | nc -u -w 1 ...` 发完就退出，源端口已经关闭，内核会回
  ICMP port unreachable，**应用收不到回显**；
- 测试程序必须绑定固定源端口，并保持 socket 打开等待回显：

```python
import socket

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.10.10.10", 50000))   # 固定源端口，保持打开
s.settimeout(3)

s.sendto(b"hello-dpdk", ("10.10.10.100", 9999))

data, addr = s.recvfrom(2048)
print("echo from", addr, ":", data)
```

期望输出：

```text
echo from ('10.10.10.100', 9999): b'hello-dpdk'
```

也可以把这段保存成 `udp_echo_client.py` 反复运行；用 heredoc 直接执行也可以：

```bash
python3 - <<'PY'
# 上面的 Python 代码
PY
```

### 6.7 TCP 回显测试

TCP 与 UDP 不同：客户端必须先完成三次握手，ustack 才会把数据交上来。
推荐用 Python 客户端：

```python
import socket

s = socket.create_connection(("10.10.10.100", 9999), timeout=3)
s.sendall(b"hello-tcp")
print("echo:", s.recv(2048))
s.close()
```

ustack 预期输出：

```text
TCP SYN from 54321, new connection
TCP connection established
TCP length = 9, payload = hello-tcp
TCP FIN, connection closed
```

客户端预期输出：

```text
echo: b'hello-tcp'
```

注意：

- 客户端先发 SYN；ustack 回 SYN|ACK；客户端再发 ACK，连接才建立；
- 回显数据按 536 字节分片，带 ACK|PSH；
- 当前只支持一个 TCP 连接、按序数据，不实现重传/拥塞控制；
- 如果客户端一直重传 SYN，检查 SYN-ACK 的 IP/端口字节序和 TCP 校验和（见 Q10）。

### 6.8 ICMP（ping）测试

ICMP Echo Request/Reply 已经实现，发送端可以直接 ping：

```bash
# 可选：先清掉旧邻居，强制内核发 ARP
sudo ip neigh flush dev ens37

ping -c 4 10.10.10.100
```

预期客户端输出：

```text
64 bytes from 10.10.10.100: icmp_seq=1 ttl=64 time=...
64 bytes from 10.10.10.100: icmp_seq=2 ttl=64 time=...
...
4 packets transmitted, 4 received, 0% packet loss
```

ustack 终端会打印：

```text
ARP request from 10.10.10.10, replying
ICMP echo request id=... seq=1 len=64, replying
ICMP echo request id=... seq=2 len=64, replying
...
```

说明：

- 第一次 ping 会先触发 ARP；ustack 的 ARP 应答器回 Reply 后，ICMP 才会发出；
- ICMP 消息长度 = `IPv4 total_length - IHL*4`，**不能**用整个帧长
  （短帧有 60 字节以太网填充）；
- 回显帧 `type=0`（Echo Reply）、`code=0`；
- `identifier` / `sequence` 从请求原样拷贝（网络字节序），ping 用它匹配请求；
- payload 原样拷贝，ping 用 payload 里的时间戳计算 RTT；
- ICMP 校验和只覆盖 ICMP 头 + payload，**不含** IP 伪首部；
- 默认 56 字节 payload 不会分片；`ping -s` 超过 1472 会分片，当前不重组。

### 6.9 容易踩的坑

| 坑 | 现象 | 处理 |
|----|------|------|
| 发送端走 NAT 网卡 | testpmd 完全收不到；发送端 tcpdump 能看到帧，但帧在 VMnet8 | 让发送端从与 DPDK 网卡同 VMnet 的网卡发；`ip route get` 确认 |
| 路由走 ens33 | 内核 ARP 发到 NAT 网段，ustack 收不到 | `ip route get 10.10.10.100` 必须显示 `dev ens37` |
| 发送端仍有 PERMANENT 旧表项 | 内核不发 ARP，直接用手工 MAC | `ip neigh del` / `ip neigh flush` 后再测 |
| arping 有回复但 ip neigh 为空 | arping 不写内核邻居表 | 用 ping/UDP/TCP 触发内核 ARP |
| ARP 请求到了但没 Reply | `gLocalIp` 与 target IP 不一致 | 检查 `USTACK_IPV4_ADDR`，或目标 IP 改成 10.10.10.100 |
| 发送端没有同网段地址 | `nc` 报 Network unreachable | 先 `ip addr add 10.10.10.10/24 dev ens37` |
| 目的 IP 是本机地址 | 走了 loopback，帧没出网卡 | 目的 IP 必须是本机不存在的地址 |
| 两台 VM 不在同一主机 / 不同 VMnet | 二层不通 | 放到同一主机、同一 VMnet，或改用 Bridged |
| `ping` 无回复 | ARP 没解析，或 ICMP type/长度/校验和错误 | 先 `arping` 确认 ARP，再检查 `handle_icmp()`（见 6.8 / 8.4） |
| VLAN / IPv6 | 被代码过滤 | 当前只处理纯 IPv4 UDP/TCP/ICMP，无 VLAN |
| TCP 客户端一直重传 SYN | 连接建立不了 | 检查 SYN-ACK 的 IP/端口字节序、TCP 校验和、第三次 ACK 判断（见 Q10） |
| payload > 1472 字节 | IP 分片，解析错误 | 测试包小于 MTU |

---

## 7. 运行与验证

### 7.1 运行

```bash
cd /mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack
sudo ./build/ustack --iova-mode=pa
```

正常输出：

```text
EAL: Detected CPU lcores: 8
EAL: Detected NUMA nodes: 1
EAL: Detected shared linkage of DPDK
EAL: Selected IOVA mode 'PA'
EAL: VFIO support initialized
EAL: Using IOMMU type 8 (No-IOMMU)
nb_sys_ports = 1
Port 0 MAC: 00:0C:29:F4:81:B8
start eth success
```

程序进入死循环轮询，会占满一个 CPU 核，这是 DPDK 的正常行为。

### 7.2 用 testpmd 验证 DPDK 收包

ustack 和 testpmd 不能同时占用网卡。先停掉 ustack，再运行 testpmd：

```bash
sudo ~/dpdk-stable-24.11.7/build/app/dpdk-testpmd \
  -l 0-1 -a 0000:0b:00.0 --iova-mode=pa -- \
  -i --rxq=1 --txq=1 --disable-rss
```

在 testpmd 中：

```text
testpmd> show port info 0
testpmd> set fwd rxonly
testpmd> set verbose 1
testpmd> start
```

发送端发包后：

```text
testpmd> show port stats all
```

重点看：

- `Link status: up`（`show port info 0`）；
- `RX-packets` 是否增长；
- `RX-errors` / `RX-nombuf` 是否为 0；
- verbose 里帧的 `type`、`dst`、`L4_UDP`、`Destination UDP port`。

判读：

| testpmd 现象 | 含义 |
|--------------|------|
| `type=0x0806`、`dst=FF:FF:FF:FF:FF:FF` | 收到 ARP 请求 → 检查 ustack 是否回 Reply、目标 IP 是否匹配 |
| `type=0x0800`、`dst=<DPDK MAC>`、`L4_UDP` | UDP 单播已到达 DPDK 网卡，二层/PMD 正常 |
| `type=0x0800`、`dst=<DPDK MAC>`、`L4_ICMP` | ICMP 已到达 DPDK 网卡；ping 无回复时检查 Echo Reply/校验和 |
| RX 一直为 0，Link up | 帧没到 vNIC → 检查 VMnet / 适配器连接 |
| Link down | vNIC 未连接 → VMware 设置里勾选 Connected |

### 7.3 用内核 vmxnet3 + tcpdump 验证 VMnet

把 DPDK 网卡临时换回内核驱动，用 tcpdump 看虚拟交换机到底有没有把帧送过来：

```bash
# 停掉 ustack / testpmd
cd ~/dpdk-stable-24.11.7/usertools
sudo ./dpdk-devbind.py -u 0000:0b:00.0
sudo modprobe vmxnet3
sudo ./dpdk-devbind.py --bind=vmxnet3 0000:0b:00.0

ip -br link
# 找到 MAC 为 DPDK 口 MAC 的那个接口

sudo ip link set <接口名> up
sudo tcpdump -i <接口名> -nn -e udp port 9999
```

发送端发一次：

- tcpdump 能看到 → VMnet 二层没问题，问题在 DPDK 侧；
- tcpdump 看不到 → VMnet 选错了，或者适配器没连接。

测完换回 vfio：

```bash
sudo ./dpdk-devbind.py -u 0000:0b:00.0
sudo ./dpdk-devbind.py --bind=vfio-pci 0000:0b:00.0
```

### 7.4 用 net_tap 做纯代码验证（可选）

如果怀疑 DPDK 侧配置，可以完全绕开 VMware 网络，用 DPDK 的 `net_tap` 虚拟设备：

```bash
# 停掉 ustack；为确保 tap 是 0 号口，可临时解绑物理网卡
cd ~/dpdk-stable-24.11.7/usertools
sudo ./dpdk-devbind.py -u 0000:0b:00.0

cd /mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack
sudo ./build/ustack --iova-mode=pa --vdev=net_tap0,iface=dtap0
```

另开终端：

```bash
sudo ip link set dtap0 up
sudo ip addr add 10.30.30.10/24 dev dtap0
sudo ip neigh replace 10.30.30.100 lladdr <ustack 打印的 MAC> nud permanent dev dtap0
printf 'hello-dpdk' | nc -u -w 1 10.30.30.100 9999
```

net_tap 能收到，说明 ustack 代码没问题；收不到再看代码或 DPDK 配置。

---

## 8. ustack.c 说明与已修复问题

### 8.1 代码结构

| 位置 | 内容 |
|------|------|
| 文件头 | 环境、网络前提、已修复问题、帧格式 |
| EAL 初始化 | `rte_eal_init()` |
| 端口检查 | `rte_eth_dev_count_avail()` |
| mbuf 池 | `rte_pktmbuf_pool_create()` |
| 端口配置 | `rte_eth_dev_configure(1 RX, 1 TX)` |
| 队列 | `rte_eth_rx_queue_setup(512)` + `rte_eth_tx_queue_setup(512)` |
| 启动 | `rte_eth_dev_start()` + `rte_eth_promiscuous_enable()` |
| 收包循环 | `rte_eth_rx_burst()` → 解析 Ethernet/IPv4 → 按协议分发 |
| TCP 状态 | `struct tcp_conn`：SYN_RCVD / ESTABLISHED / FIN |
| TCP 发送 | `tcp_send_segment()`：构造 Ethernet/IPv4/TCP 头、计算校验和 |
| TCP 回显 | `tcp_echo_payload()`：按 536 字节分片 |
| TCP 处理 | `handle_tcp()`：三次握手、数据回显、FIN/RST |
| UDP 编码 | `uStack_encode_udp_pkt()`：构造 Ethernet/IPv4/UDP 头、拷贝 payload、计算校验和 |
| UDP 发送 | `uStack_send()` 分配 TX mbuf；主循环调用 `rte_eth_tx_burst()` |
| UDP 处理 | `handle_udp()`：无状态回显 |
| ARP 学习表 | `gArpTable` / `arp_table_update()` / `arp_table_lookup()`：学习 IP-MAC |
| ARP 应答 | `arp_send_reply()`：构造 Ethernet + ARP Reply，补零到 60 字节 |
| ARP 处理 | `handle_arp()`：解析请求/应答、学习、回 Reply |
| ICMP 编码 | `icmp_send_reply()`：构造 Echo Reply、拷贝 payload、计算 ICMP 校验和 |
| ICMP 处理 | `handle_icmp()`：回 Echo Request，其它类型打印/忽略 |
| 释放 | RX mbuf 由主循环统一 `rte_pktmbuf_free()`；TX mbuf 成功后归驱动 |

### 8.2 已修复的典型 bug

#### ① 循环内误用 `rte_eth_rx_burst()` 当包长度（最隐蔽）

错误写法：

```c
for (uint16_t i = 0; i < nb_recvd; ++i) {
    uint32_t pkt_len = rte_eth_rx_burst(gDpdkPortId, 0, mbufs, BURST_SIZE);
```

问题：

- 返回值是“本次收到的包数量”（0/1/2…），不是包长度；
- `pkt_len` 恒小于 `14 + 20 = 34`，所有包都在长度检查处被 `free` 掉并 `continue`；
- 同时覆盖了 `mbufs[]`，外层收到的 mbuf 指针被冲掉，造成泄漏；
- 表现为：**testpmd 能收到，ustack 完全没输出**。

这是本次实验最终定位到的**根本原因**：A/B/C 三种 testpmd 配置（1 TX、0 TX、
128/512 描述符）都能收到 UDP，说明 VMnet 和队列配置都没问题；唯一差异就是
ustack 的收包循环里多调用了一次 `rte_eth_rx_burst()`。把它改成
`rte_pktmbuf_data_len(mbufs[i])` 后立即恢复正常。

正确写法：

```c
uint32_t pkt_len = rte_pktmbuf_data_len(mbufs[i]);
```

单段 mbuf 用 `data_len`；开启 scatter/jumbo 后应使用 `rte_pktmbuf_pkt_len()`。

#### ② `dgram_len` 没有转字节序

```c
// 错误
uint16_t length = udphdr->dgram_len;
// 正确
uint16_t length = rte_be_to_cpu_16(udphdr->dgram_len);  // 或 ntohs()
```

UDP 长度 18（`0x0012`）在小端机器上会被读成 `0x1200 = 4608`，
`data_length` 变成 4600，`%.*s` 会越界读，可能崩溃或打印垃圾。

#### ③ `sizeof` 写成了指针

```c
// 错误
length - sizeof(struct rte_udp_hdr *)
// 正确
length - sizeof(struct rte_udp_hdr)
```

x86_64 上指针和结构体都恰好是 8 字节，所以“碰巧能跑”，但语义错误。

#### ④ 固定假设 IPv4 头 20 字节

```c
// 不严谨
struct rte_udp_hdr *udphdr = (struct rte_udp_hdr *)(iphdr + 1);
// 正确：按 IHL 计算
uint16_t ihl = (iphdr->version_ihl & RTE_IPV4_HDR_IHL_MASK) * RTE_IPV4_IHL_MULTIPLIER;
struct rte_udp_hdr *udphdr = (struct rte_udp_hdr *)((uint8_t *)iphdr + ihl);
```

#### ⑤ 越界读保护

- 检查 `pkt_len >= 14 + 20`；
- 检查 `udp_off + sizeof(struct rte_udp_hdr) <= pkt_len`；
- `data_length` 不能超过 `pkt_len - udp_off - 8`；
- 用 `fwrite` 按精确长度输出，而不是依赖 `%.*s` 遇到 `\0` 停止。

#### ⑥ 端口数量判断

`rte_eth_dev_count_avail()` 返回 `uint16_t`，不能用 `< 0`；应改为 `== 0`。

#### ⑦ vmxnet3 队列配置

- RX 描述符从 128 改为 **512**：128 是 vmxnet3 PMD 的最低边界
  （源码里的错误信息甚至写着 `Min: 256`），512 与 testpmd 默认行为更接近；
- 配置 **1 个 TX 队列**：用于回显发送；同时让端口状态与 testpmd 的可用配置一致；
- TX 描述符也用 512（vmxnet3 的 TX 下限是 512）。

#### ⑧ 回显帧的 IPv4 头偏移错误（最致命）

```c
// 错误：msg + 1 只前进 1 字节
struct rte_ipv4_hdr *iphdr = (struct rte_ipv4_hdr *)(msg + 1);
// 正确：以太网头 14 字节
struct rte_ipv4_hdr *iphdr =
    (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));
```

写错后 IPv4 头覆盖了以太网头的目的 MAC 和 EtherType，回显帧变成非法帧，
发送端网卡按 MAC 过滤收不到，Wireshark 也解析不出 UDP。

#### ⑨ `version_ihl` 写成 `0x54`

`0x54` = 版本 5 / IHL 4；IPv4 应为：

```c
iphdr->version_ihl = 0x45;   // version 4, IHL 5
```

#### ⑩ UDP Length 少了 8 字节

```c
// 错误：只写了 payload 长度
udphdr->dgram_len = htons(payload_len);
// 正确：UDP 头 8 + payload
udphdr->dgram_len = htons(payload_len + sizeof(struct rte_udp_hdr));
```

#### ⑪ 回显 payload 没有拷贝

`uStack_encode_udp_pkt()` 只构造了头部，没有使用 `data`，回显出去的是 mbuf
里的旧数据。必须在 UDP 校验和之前拷贝：

```c
rte_memcpy((char *)(udphdr + 1), data, payload_len);
```

#### ⑫ TX 成功后仍然 free mbuf

```c
// 错误：TX 成功后 mbuf 归驱动所有
rte_eth_tx_burst(gDpdkPortId, 0, &txmbuf, 1);
rte_pktmbuf_free(txmbuf);
// 正确：只有没发出去才由应用释放
uint16_t nb_tx = rte_eth_tx_burst(gDpdkPortId, 0, &txmbuf, 1);
if (nb_tx != 1) {
    rte_pktmbuf_free(txmbuf);
}
```

#### ⑬ TTL 写成 0

`iphdr->time_to_live = 0` 会被部分协议栈丢弃，应设为 64。

#### ⑭ 测试端 `nc` 发完就退出

回显的目的端口是发送端的源端口。`nc -u -w 1` 发完就退出，源端口已关闭，
收不到回显。要用固定源端口并 `recvfrom()` 等待（见 6.6）。

#### ⑮ 错误分支释放了 `pktdata` 而不是 `mbuf`

`uStack_send()` 中 `rte_pktmbuf_append()` 失败时，原代码写的是
`rte_pktmbuf_free((struct rte_mbuf *)pktdata)`，释放的是数据指针；
应改为 `rte_pktmbuf_free(mbuf)`。当前 append 不会失败，属于潜在问题。

#### ⑯ ARP opcode 判断写反（ARP 请求完全没有处理）

```c
// 错误：ARP 请求 opcode=1 时，两个分支都不执行
if (opcode != RTE_ARP_OP_REQUEST) { ... 回 Reply ... }
else if (opcode == RTE_ARP_OP_REPLY) { ... }

// 正确
if (opcode == RTE_ARP_OP_REQUEST) {
    /* 学习请求者 + 回 ARP Reply */
} else if (opcode == RTE_ARP_OP_REPLY) {
    /* 学习应答者 */
}
```

表现为发送端连发 3 个 ARP Request，ustack 终端一行 ARP 日志都没有。

#### ⑰ ARP Reply 的以太网源/目的 MAC 写反

```c
// 错误：Reply 发给了本机自己
ehdr->src_addr = req->arp_sha;
ehdr->dst_addr = gLocalAddr;

// 正确：Reply 发给请求者
ehdr->dst_addr = req->arp_sha;
ehdr->src_addr = gLocalAddr;
```

表现为 ustack 打印了 `ARP request ... replying`，但发送端 `arping` 收不到 Reply。

#### ⑱ ARP 表拷贝了 `&mac` 而不是 `mac`

```c
// 错误：mac 已经是指针，&mac 是指针的指针
rte_memcpy(&gArpTable[i].mac, &mac, sizeof(struct rte_ether_addr));
// 正确
rte_memcpy(&gArpTable[i].mac, mac, sizeof(struct rte_ether_addr));
```

#### ⑲ ARP 日志直接 fwrite 原始 IP 字节

`fwrite(&arphdr->arp_data.arp_sip, 1, 4, stdout)` 对 `10.10.10.10` 会输出
四个 `0x0A`（换行符），终端显示成乱码/空行。应使用 `inet_ntoa()`：

```c
struct in_addr sip = { .s_addr = arphdr->arp_data.arp_sip };
printf("ARP request from %s, replying\n", inet_ntoa(sip));
```

#### ⑳ arping 成功但内核 `ip neigh` 没有表项

这不是代码 bug，而是测试方法问题：`arping` 是用户态工具，不会更新内核
邻居表。要让内核自动学习，必须用 `ping`、UDP 或 TCP 触发内核自己发 ARP。
另外，如果 `arping` 显示的源 IP 是 `192.168.179.148` 而不是 `10.10.10.10`，
说明 ens37 上当前没有 `10.10.10.10/24`，先重新 `ip addr add` 并确认
`ip route get 10.10.10.100` 走 ens37。

### 8.3 TCP 实现说明

TCP 与 UDP 不同，必须先完成三次握手，并维护 seq/ack：

| 阶段 | 本端动作 | 状态 |
|------|----------|------|
| 收到 SYN | 记录 client ISN；回 SYN\|ACK（seq=iss，ack=client_isn+1） | SYN_RCVD |
| 收到第三次 ACK（纯 ACK，ack=iss+1） | 进入 ESTABLISHED | ESTABLISHED |
| 收到数据（seq=rcv_nxt） | 打印 payload；rcv_nxt += len；回显 ACK\|PSH；snd_nxt += len | ESTABLISHED |
| 收到 FIN（seq+len=rcv_nxt） | rcv_nxt += 1；回 FIN\|ACK；snd_nxt += 1；复位 | 关闭 |
| 收到 RST | 直接复位 | 关闭 |

实现要点：

1. **IP 地址不能再 `htonl`**：`conn->local_ip/remote_ip` 是从收到的 IP 头
   按网络字节序拷贝的，发送时直接赋值。
2. **端口只 `htons` 一次**：连接结构体统一存主机字节序，发送时 `htons`。
   如果存网络序又 `htons`，SYN-ACK 端口会错位，客户端会一直重传 SYN。
3. **第三次握手是纯 ACK**：不能要求 ACK 包同时带 SYN；判断条件应为
   `(flags & TCP_FLAG_ACK) && ack == snd_nxt`。
4. **payload 指针**：必须 `(uint8_t*)tcphdr + tcp_hdr_len`，
   不能 `tcphdr + tcp_hdr_len`（会按结构体大小放大 20 倍）。
5. **FIN 必须带 ACK**：回 `FIN|ACK`，否则对端可能不认。
6. **TCP 校验和**：必须计算；结果为 0 时写 `0xFFFF`。

限制：单连接、无重传、无拥塞控制、无窗口缩放/SACK/时间戳、
不支持乱序重组；SYN-ACK 固定 20 字节、无选项；回显按 536 字节分片。

### 8.4 ICMP 实现说明

| 阶段 | 本端动作 |
|------|----------|
| 收到 Echo Request（type=8, code=0） | 交换 MAC/IP；ident/seq 原样拷贝；回 Echo Reply（type=0, code=0） |
| payload | 原样拷贝，ping 用它计算 RTT |
| 其它 ICMP 类型 | 打印 type/code/len，不回复 |
| 分片 ICMP | 直接忽略（`ping -s` 超过 MTU 时不回复） |

实现要点：

1. **ICMP 长度**：`icmp_len = ip_total_len - IHL*4`，不能用 `pkt_len`；
   否则 60 字节以太网填充会被算进 payload/校验和，ping 会丢包；
2. **ICMP 校验和**：只覆盖 ICMP 头 + payload，不含 IP 伪首部；
   计算前 `icmp_cksum` 必须清零；用 `rte_raw_cksum()` 折叠，
   不能用 `rte_ipv4_udptcp_cksum()`（那个会加伪首部）；
3. **ident/seq**：从请求原样拷贝，它们是网络字节序，不要再 `htons`；
4. **type/code**：Echo Request 是 8/0，Echo Reply 是 0/0；
5. **TTL**：回复用 64；IP 首部校验和用 `rte_ipv4_cksum()`；
6. **分片**：检查 `fragment_offset` 的 MF/offset，分片包直接忽略。

#### 坑 1：最隐蔽的 bug——`iphdr + ip_hdr_len` 指针放大 20 倍

错误写法：

```c
struct rte_icmp_hdr *icmphdr =
    (struct rte_icmp_hdr*)(iphdr + ip_hdr_len);
```

`iphdr` 是 `struct rte_ipv4_hdr *`，`+20` 实际前进 `20 * 20 = 400` 字节，
读到 mbuf 里清零的内存；`icmp_type` 读到 0，程序把它当成 Echo Reply，
只打印 `ICMP echo reply id=0 seq=0`，**不会回包**，发送端 ping 100% 丢包。

正确写法：

```c
struct rte_icmp_hdr *icmphdr =
    (struct rte_icmp_hdr*)((uint8_t*)iphdr + ip_hdr_len);
```

#### 坑 2：回显 payload 拷贝来源写错

错误：

```c
rte_memcpy((uint8_t*)out_icmphdr + 8,
           (const uint8_t*)out_iphdr + 8, payload_len);
```

源指针指向新分配的 TX 缓冲区，回显 payload 是垃圾数据；
应从请求的 ICMP 头之后拷贝：

```c
rte_memcpy((uint8_t*)out_icmphdr + 8,
           (const uint8_t*)icmphdr + 8, payload_len);
```

#### 坑 3：长度检查用了 IPv4 头大小

```c
// 错误：要求 ICMP 位置至少 20 字节
if(icmp_off + sizeof(struct rte_ipv4_hdr) > pkt_len)
// 正确：ICMP 头只有 8 字节
if(icmp_off + sizeof(struct rte_icmp_hdr) > pkt_len)
```

---

## 9. 编译与构建问题总结

### 9.1 Makefile 来源与修改

Makefile 复制自 DPDK 源码中的示例程序：

```text
<DPDK 源码根目录>/examples/helloworld/Makefile
```

在本环境中 DPDK 源码根目录是 `~/dpdk-stable-24.11.7`，所以原文件是
`~/dpdk-stable-24.11.7/examples/helloworld/Makefile`。

复制后做了三处修改。

#### 修改 1：修改生成的可执行程序名

原内容：

```make
APP = helloworld
```

修改为：

```make
APP = ustack
```

#### 修改 2：修改参与编译的源文件名

原内容：

```make
SRCS-y := main.c
```

修改为：

```make
SRCS-y := ustack.c
```

说明：源文件名必须与实际文件 `ustack.c` 完全一致。此前曾误写为
`usatck.c`，导致 make 报错：

```text
make: *** 没有规则可制作目标“usatck.c”。 停止。
```

#### 修改 3：将软链接改为文件复制，以适配共享文件夹

原内容：

```make
shared: build/$(APP)-shared
        ln -sf $(APP)-shared build/$(APP)
static: build/$(APP)-static
        ln -sf $(APP)-static build/$(APP)
```

修改为：

```make
shared: build/$(APP)-shared
        cp build/$(APP)-shared build/$(APP)
static: build/$(APP)-static
        cp build/$(APP)-static build/$(APP)
```

说明：

- 原 Makefile 用 `ln -sf` 创建软链接 `build/ustack -> ustack-shared`；
- VMware 共享文件夹（hgfs）默认不支持创建符号链接，执行时报：

  ```text
  ln: Operation not supported
  make: *** [Makefile:20：shared] 错误 1
  ```

  （部分 `ln` 版本会显示为
  `ln: failed to create symbolic link 'build/ustack': Operation not supported`。）

- 改为 `cp` 复制可执行文件，不依赖符号链接，即可在共享文件夹内正常构建；
- `clean` 目标中原有的 `rm -f build/$(APP)` 已能清理这个副本，无需额外修改。

#### 9.1.1 其他构建说明

- 本项目只使用 `make` 的默认目标 `shared`（共享链接），没有使用 `static`；
- 编译/链接参数通过 `pkg-config --cflags --libs libdpdk` 获取；
- 如果报 `no installation of DPDK found`，检查 DPDK 是否安装，以及
  `pkg-config --exists libdpdk` / `PKG_CONFIG_PATH` 是否正确；
- hgfs 的“修改时间在未来”警告不影响编译结果，必要时 `touch` 源文件或
  `Makefile` 后重编；
- 详细的共享目录编译说明见
  [`../在共享文件夹中编译对应的dpdk协议栈文件.txt`](../在共享文件夹中编译对应的dpdk协议栈文件.txt)。

### 9.2 构建命令

```bash
make          # 生成 build/ustack-shared，并复制为 build/ustack
make static   # 需要 pkg-config 支持静态链接，一般不用
make clean
```

### 9.3 常见编译/构建问题

| 问题 | 原因 / 处理 |
|------|------------|
| `no installation of DPDK found` | `pkg-config --exists libdpdk` 失败；安装 DPDK 或设置 `PKG_CONFIG_PATH` |
| `没有规则可制作目标 usatck.c` | `SRCS-y` 文件名写错，必须和实际文件一致 |
| `ln: Operation not supported` | hgfs 不支持软链接；Makefile 已改成 `cp` |
| `make: 警告：文件"Makefile"的修改时间在未来...` | hgfs 时间戳/时钟同步问题，不影响编译；可 `touch Makefile` |
| 静态链接失败 | pkg-config 的 `--static --libs libdpdk` 不完整；用默认的 shared 即可 |
| 运行提示库找不到 | 确认 `ldconfig` / `LD_LIBRARY_PATH` 包含 DPDK 共享库路径 |

> 本实验代码编译无错误。若出现函数未定义，通常是 DPDK 头文件/库版本不匹配，
> 确认 `pkg-config --modversion libdpdk` 与运行时的 DPDK 一致。

---

## 10. 以太网帧头部处理（UDP / TCP / ICMP）

### 10.1 Ethernet II 头（14 字节）

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 6 | Destination MAC | 目的 MAC |
| 6 | 6 | Source MAC | 源 MAC |
| 12 | 2 | EtherType | 大端；0x0800=IPv4，0x0806=ARP，0x86DD=IPv6，0x8100=VLAN |
| 14 | — | Payload | 上层协议数据 |

本程序对 ARP（`0x0806`）单独处理；IPv6/VLAN 仍然丢弃。

### 10.2 IPv4 头（20 字节起，长度 = IHL × 4）

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | Version + IHL | 高 4 位版本（4），低 4 位 IHL（单位 4 字节） |
| 1 | 1 | TOS | 服务类型 |
| 2 | 2 | Total Length | 大端；IPv4 头 + 数据总长度 |
| 4 | 2 | Identification | 分片标识 |
| 6 | 2 | Flags + Fragment Offset | 大端；分片相关 |
| 8 | 1 | TTL | 生存时间 |
| 9 | 1 | Protocol | 6=TCP，17=UDP，1=ICMP |
| 10 | 2 | Header Checksum | 首部校验和 |
| 12 | 4 | Source Address | 源 IP（网络字节序） |
| 16 | 4 | Destination Address | 目的 IP（网络字节序） |
| 20 | 可变 | Options | IHL > 5 时存在 |

- IPv4 头长度 = `(version_ihl & 0x0f) * 4`；
- 上层数据偏移 = 14 + IPv4 头长度；
- 分片包需要额外处理；本程序不重组，测试包请小于 MTU。

### 10.3 UDP 头（8 字节）

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 | Source Port | 大端 |
| 2 | 2 | Destination Port | 大端 |
| 4 | 2 | Length | 大端；UDP 头 + payload，最小 8 |
| 6 | 2 | Checksum | 大端；IPv4 下可为 0（表示未计算） |
| 8 | — | Payload | 应用数据 |

- payload 长度 = `Length - 8`；
- payload 在帧中的偏移 = `14 + IPv4头长度 + 8`；
- UDP 是无连接的，本程序直接按长度打印。

### 10.4 TCP 头（20 字节起，长度 = Data Offset × 4）

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 | Source Port | 大端 |
| 2 | 2 | Destination Port | 大端 |
| 4 | 4 | Sequence Number | 大端 |
| 8 | 4 | Acknowledgment Number | 大端 |
| 12 | 1 | Data Offset + Reserved | 高 4 位是数据偏移（单位 4 字节） |
| 13 | 1 | Flags | CWR/ECE/URG/ACK/PSH/RST/SYN/FIN |
| 14 | 2 | Window | 大端 |
| 16 | 2 | Checksum | 大端 |
| 18 | 2 | Urgent Pointer | 大端 |
| 20 | 可变 | Options + Payload | 数据偏移之后是应用数据 |

- TCP 头长度 = `(tcp->data_off >> 4) * 4`；
- payload 偏移 = `14 + IPv4头长度 + TCP头长度`；
- TCP 是有状态的：需要维护连接状态和 seq/ack，不能像 UDP 一样无状态处理；
- 当前 `ustack.c` 实现了一个最小 TCP 回显：
  1. 单连接状态机（SYN_RCVD / ESTABLISHED / FIN）；
  2. 三次握手、数据 ACK/回显、FIN|ACK；
  3. 按 536 字节分片回显；
  4. 发送方向计算 IPv4/TCP 校验和；
  5. 不实现重传、拥塞控制、窗口缩放、SACK、时间戳、多连接和乱序重组。

### 10.5 字节序与校验和

- Ethernet 的 EtherType、IPv4 的总长度/ID/分片/校验和、UDP/TCP 的端口/长度/校验和
  都是 **网络字节序（大端）**；
- 在 x86 上必须用 `rte_be_to_cpu_16/32` 或 `ntohs/ntohl` 转换；
- MAC 是 6 个字节的数组，不需要转换；
- IP 地址在包头里是 4 字节网络序，打印时可用 `inet_ntop()`；
- 接收方向本程序不校验 IPv4/UDP 校验和；testpmd 的 `ol_flags` 显示
  `L4_CKSUM_GOOD / IP_CKSUM_GOOD`，说明发送端内核计算的校验和是正确的；
- 回显发送方向，代码用 `rte_ipv4_cksum()` 计算 IPv4 首部校验和，
  用 `rte_ipv4_udptcp_cksum()` 计算 UDP 校验和；两者返回网络字节序，
  直接写入头部字段即可，不要再用 `htons()`；
- ICMP 校验和没有 IP 伪首部，代码用 `rte_raw_cksum()` 折叠取反后写入
  `icmp_cksum`；同样直接写入，不要再 `htons()`；
- 计算 UDP 校验和之前必须先把 UDP 头字段和 payload 都填好，并把
  `dgram_cksum` 清零。

### 10.6 短帧、填充与 MTU

- 以太网最小帧 60 字节（不含 FCS），短 UDP 包后面会有 0 填充；
- payload 长度必须用 UDP `Length` 计算，不能用整个帧长；
- ICMP 没有长度字段，ICMP 消息长度必须用 IPv4 `total_length - IHL*4` 计算；
- 本程序用 `avail = pkt_len - udp_off - 8` 做上限保护；
- MTU 1500 时，UDP payload 最大约 1472 字节（1500 - 20 - 8）；
- 超过 MTU 会触发 IP 分片，本程序不重组，解析会出错。

### 10.7 VLAN

- 带 VLAN 标签的帧：EtherType = `0x8100`，后面 4 字节是 TCI + 内层 EtherType；
- IPv4 头在 `14 + 4 = 18` 字节处；
- 本程序没有处理 VLAN，会直接丢弃；
- 如果虚拟交换机配置了 VLAN，需要先解析 0x8100 再继续。

### 10.8 UDP 回显帧的构造（本程序）

回显帧不是把收到的帧原样发回去，而是重新构造一个完整的帧：

| 字段 | 回显帧取值 | 来源 |
|------|-----------|------|
| 目的 MAC | 原帧的源 MAC | 发送端 ens37 |
| 源 MAC | 原帧的目的 MAC | 本 DPDK 网卡 |
| 源 IP | 原帧的目的 IP | 10.10.10.100 |
| 目的 IP | 原帧的源 IP | 10.10.10.10 |
| 源端口 | 原帧的目的端口 | 9999 |
| 目的端口 | 原帧的源端口 | 发送端 socket 的源端口 |

这些字段按原始字节拷贝，保持网络字节序，不需要 `htons/ntohl`。

构造顺序：

1. **Ethernet**：目的/源 MAC + EtherType `0x0800`（14 字节）；
2. **IPv4**：`version_ihl = 0x45`，`total_length = 20 + 8 + payload_len`，
   `TTL = 64`，`protocol = 17`，源/目的 IP，计算首部校验和；
3. **UDP**：源/目的端口，`dgram_len = 8 + payload_len`，拷贝 payload，
   计算 UDP 校验和；
4. **TX**：`rte_pktmbuf_alloc()` + `rte_pktmbuf_append()`，
   `rte_eth_tx_burst()` 发送；成功后备驱动释放，失败才由应用释放。

注意：回显的目的端口是发送端的**源端口**，不是 9999。用 `nc` 发完就退出
的测试方式收不到回显，见 6.6。

### 10.9 TCP 回显的实现

TCP 回显不是简单交换地址，而是按连接状态构造段：

| 段 | flags | seq | ack | payload |
|----|-------|-----|-----|---------|
| SYN-ACK | SYN\|ACK | iss | client_isn+1 | 0 |
| 数据回显 | ACK\|PSH | snd_nxt | rcv_nxt | payload |
| FIN | FIN\|ACK | snd_nxt | rcv_nxt | 0 |
| 纯 ACK | ACK | snd_nxt | rcv_nxt | 0 |

序号规则：

- SYN 和 FIN 各占一个序号；
- 数据占 `payload_len` 个序号；
- 本端发送后 `snd_nxt` 增加相应长度；
- 收到按序数据后 `rcv_nxt += payload_len`；
- 回显的 ack 用更新后的 `rcv_nxt`。

实现限制：单连接、按序数据、无重传、无拥塞控制、无窗口缩放/SACK/时间戳；
SYN-ACK 固定 20 字节、无选项；回显按 536 字节分片。

### 10.10 ARP 帧格式与处理

ARP 帧没有 IP 头，结构是 Ethernet II + ARP：

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 6 | 目的 MAC | Request：广播 `ff:ff:ff:ff:ff:ff`；Reply：请求者 MAC |
| 6 | 6 | 源 MAC | 发送方 MAC |
| 12 | 2 | EtherType | `0x0806` |
| 14 | 2 | Hardware Type | 1 = Ethernet |
| 16 | 2 | Protocol Type | `0x0800` = IPv4 |
| 18 | 1 | Hardware Len | 6 |
| 19 | 1 | Protocol Len | 4 |
| 20 | 2 | Opcode | 1 = Request，2 = Reply |
| 22 | 6 | Sender MAC | 发送方 MAC |
| 28 | 4 | Sender IP | 发送方 IP（网络字节序） |
| 32 | 6 | Target MAC | Request 时为 0；Reply 时为请求者 MAC |
| 38 | 4 | Target IP | 目标 IP（网络字节序） |

- ARP 部分 28 字节，加 Ethernet 头共 42 字节；实际发送补零到 60；
- Request：目的 MAC 广播，Target MAC 全 0；
- Reply：目的 MAC = 请求者 MAC，Target MAC = 请求者 MAC；
- ustack 只对 `target IP == gLocalIp`（10.10.10.100）的请求回应；
- 收到 Request/Reply 都会更新 ARP 学习表。

### 10.11 ICMP 头（8 字节）

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | Type | 8 = Echo Request，0 = Echo Reply |
| 1 | 1 | Code | Echo 为 0 |
| 2 | 2 | Checksum | 只覆盖 ICMP 头 + payload，不含 IP 伪首部 |
| 4 | 2 | Identifier | 大端；ping 用它匹配请求 |
| 6 | 2 | Sequence | 大端；每次 ping 递增 |
| 8 | — | Payload | ping 的时间戳等数据 |

- ICMP 没有端口，用 identifier 区分不同的 ping 进程；
- ICMP 校验和**不含** IP 伪首部，这一点与 UDP/TCP 不同；
- ICMP 消息长度来自 IPv4 `total_length`，ICMP 头本身没有长度字段。

### 10.12 ICMP Echo Reply 的构造（本程序）

| 字段 | 回显帧取值 | 来源 |
|------|-----------|------|
| 目的 MAC | 原帧源 MAC | 发送端 ens37 |
| 源 MAC | 原帧目的 MAC | 本 DPDK 网卡 |
| 源 IP | 原帧目的 IP | 10.10.10.100 |
| 目的 IP | 原帧源 IP | 10.10.10.10 |
| Type / Code | 0 / 0 | Echo Reply |
| Identifier / Sequence | 原请求字段 | 原样拷贝（网络字节序） |
| Payload | 原请求 payload | 原样拷贝 |

构造顺序：

1. **Ethernet**：交换 MAC + EtherType `0x0800`（14 字节）；
2. **IPv4**：`version_ihl=0x45`，`total_length=20+icmp_len`，`TTL=64`，
   `protocol=1`，交换源/目的 IP，计算首部校验和；
3. **ICMP**：type=0、code=0、ident/seq 原样、payload 原样，
   计算 ICMP 校验和；
4. **TX**：`rte_pktmbuf_alloc()` + `rte_pktmbuf_append()` +
   `rte_eth_tx_burst()`；成功后备驱动释放，失败才由应用释放。

---

## 11. 常见问题排查（FAQ）

### Q1：ustack 完全没有输出

按顺序检查：

```bash
# 1. 进程还活着吗？（排除崩溃）
ps -ef | grep ustack

# 2. 发送端地址、路由、邻居
ip -br addr show ens37
ip route get 10.10.10.100
ip neigh show 10.10.10.100

# 3. 用一个不依赖 ustack 的工具验证二层
#    停掉 ustack，跑 testpmd（见第 7.2 节）
# 4. 停掉 DPDK，换回内核 vmxnet3 驱动 + tcpdump（见第 7.3 节）
```

如果 **testpmd 能收到、ustack 收不到**，优先检查收包循环里是否误把
`rte_eth_rx_burst()` 的返回值（包数量）当成包长度——这是本次实验的实际根因，
详见 8.2 ①。

### Q2：看到 `length = 4608` 而不是 18

字节序没转。用 `rte_be_to_cpu_16()` / `ntohs()`。

### Q3：testpmd 只收到 `type=0x0806` 的广播 ARP

说明 ARP 请求已经到了 DPDK 网卡，但 ustack 没有回 Reply，或者发送端没有接受。
检查：

1. ustack 是否打印 `ARP request from ... replying`；
2. `USTACK_IPV4_ADDR` / `gLocalIp` 是否等于请求的 target IP（10.10.10.100）；
3. `arp_send_reply()` 的以太网目的 MAC 是否是请求者 MAC；
4. 发送端是否有旧的 `PERMANENT` 表项阻止内核发 ARP，先
   `ip neigh flush dev ens37`；
5. `sudo arping -I ens37 10.10.10.100` 是否收到 Reply。

### Q4：testpmd 的 RX 一直是 0，但 Link status 是 up

帧没到 vNIC。检查发送端和 DPDK 网卡是否在同一 VMnet；用内核 tcpdump 验证。

### Q5：testpmd 的 Link status 是 down

VMware 里网卡没连接。检查适配器的 `Connected` / `Connect at power on`。

### Q6：运行时报 `Could not configure eth` / `Could not start`

- 大页不足；
- vfio 没绑定；
- 没加 `--iova-mode=pa`；
- 另一个 DPDK 进程还占着网卡（ustack 和 testpmd 不能同时跑）。

### Q7：`ip addr add` 后 `ip route get` 还是走 NAT 网卡

地址没真正加上。`ip -br link` 不显示 IP，要用：

```bash
ip -br addr show ens37
ip route show dev ens37
```

如果地址不在，重新执行 `sudo ip addr add 10.10.10.10/24 dev ens37`。
如果路由不在，检查是否有同网段的多块网卡导致路由选错接口。

### Q8：改了代码但运行时行为没变

共享目录时间戳可能导致 `make` 认为文件没更新：

```bash
touch ustack.c
make clean && make
```

### Q9：ustack 能收到请求，但发送端收不到回显

按顺序检查：

1. **发送端是否还在等待**：回显目的端口是发送端的源端口；`nc -u -w 1`
   发完就退出，端口已关闭。用 6.6 的 Python 脚本绑定固定源端口并
   `recvfrom()`；
2. **回显帧是否合法**：
   - IPv4 头是否从 `msg + 14` 开始（不是 `msg + 1`）；
   - `version_ihl` 是否为 `0x45`（不是 `0x54`）；
   - UDP `dgram_len` 是否等于 `8 + payload_len`；
   - `uStack_encode_udp_pkt()` 是否真的把 payload 拷贝到了 `udphdr + 1`；
   - TTL 是否为 64；
3. **TX mbuf 所有权**：`rte_eth_tx_burst()` 成功后不能再 `rte_pktmbuf_free()`；
4. **抓包位置**：回显方向是“DPDK 网卡 → 发送端”，要在发送端 `ens37`、
   同一 VMnet 的网卡或 `vmnet-sniffer` 上抓；不要在 DPDK 网卡的内核接口上抓
   （它被 vfio 接管，没有接口）；
5. **防火墙**：发送端 `sudo ufw status` 是否会拦截 UDP 50000；
6. **TX 是否成功**：检查 `rte_eth_tx_burst()` 返回值；必要时打印
   `rte_eth_stats_get()` 的 `opackets` / `oerrors`。

### Q10：TCP 客户端一直重传 SYN，连接建立不了

这是 TCP 握手阶段最常见的问题，检查 SYN-ACK：

1. **IP 地址被二次 `htonl`**：连接里的 `local_ip/remote_ip` 已经是网络
   字节序，`tcp_send_segment()` 里不能再 `htonl`；否则源/目的 IP 会字节交换；
2. **端口被二次 `htons`**：连接结构体统一存主机字节序，发送时 `htons` 一次；
   如果存网络序又 `htons`，SYN-ACK 端口会错位；
3. **第三次握手的 ACK 判断**：客户端发的是纯 ACK（没有 SYN），
   条件应为 `(flags & TCP_FLAG_ACK) && ack == snd_nxt`；
4. **TCP 校验和**：必须计算；结果为 0 时写 `0xFFFF`；
5. **Seq/Ack**：SYN-ACK 的 `seq = iss`，`ack = client_isn + 1`；
6. 用 Wireshark 抓 `tcp.port == 9999`，确认 SYN-ACK 的 IP、端口、flags、checksum。

### Q11：TCP 连接建立后，数据打印了但收不到回显

- 检查回显数据的 `seq/ack`：`seq = snd_nxt`，`ack = 更新后的 rcv_nxt`；
- 检查 payload 指针：必须是 `(uint8_t*)tcphdr + tcp_hdr_len`；
- 检查 FIN：关闭时回 `FIN|ACK`；
- 回显按 536 字节分片，确认客户端能重组；
- 用 Wireshark 看客户端是否 ACK 了回显数据。

### Q12：arping 收到回复，但 `ip neigh` 里没有 IP-MAC

`arping` 是用户态 ARP 工具，不会把结果写入内核邻居表。用内核自己发起的
流量触发 ARP：

```bash
sudo ip neigh flush dev ens37
ping -c 1 -W 1 10.10.10.100
ip neigh show 10.10.10.100
```

ping 现在应该能收到 Reply（ICMP 已实现）；关键是邻居表里应该出现
DPDK 网卡的 MAC。

如果邻居表仍为空/FAILED：

- `ip route get 10.10.10.100` 必须是 `dev ens37`；
- `ip -br addr show ens37` 必须有 `10.10.10.10/24`；
- 发送端是否有旧的 `PERMANENT` 表项；
- ustack 是否收到 ARP 请求并回复；
- `arping` 输出里的源 IP 是否是 10.10.10.10（如果是 192.168.179.148，
  说明 ens37 上没有 10.10.10.10/24）。

### Q13：ping 不通 / 收不到 ICMP 回显

1. **ARP 是否完成**：`ip neigh show 10.10.10.100` 是否显示 DPDK MAC；
   没有就先检查 `handle_arp()` / `USTACK_IPV4_ADDR`；
2. **ICMP 头指针**：`(struct rte_icmp_hdr*)((uint8_t*)iphdr + ip_hdr_len)`，
   不能写 `iphdr + ip_hdr_len`（会放大 20 倍，读到清零内存，
   type 变成 0，只打印不回复）；
3. **ICMP 类型**：只回复 type=8、code=0 的 Echo Request；
4. **ICMP 校验和**：只覆盖 ICMP 头 + payload，不含 IP 伪首部；
   计算前 `icmp_cksum` 必须清零；
5. **ICMP 长度**：用 `ip_total_len - IHL*4`，不能用 `pkt_len`；
   否则 60 字节填充会算进校验和，ping 会丢包；
6. **ident/seq**：必须原样拷贝，不能二次 `htons`；
7. **payload 来源**：回显 payload 必须从请求的 `icmphdr + 8` 拷贝，
   不能从 `out_iphdr` 拷贝；
8. **分片**：`ping -s` 超过 1472 会分片，当前不重组；
9. 用 Wireshark 抓 `icmp`，确认 Echo Request 和 Echo Reply 的
   type/checksum/ident/seq。

---

## 12. 命令速查

```bash
# ===== 每次开机 =====
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
cd ~/dpdk-stable-24.11.7/usertools
sudo ./dpdk-devbind.py --bind=vfio-pci 0000:0b:00.0

# ===== 编译与运行 =====
cd /mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack
make
sudo ./build/ustack --iova-mode=pa

# ===== 发送端 =====
sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
ip -br addr show ens37
ip route get 10.10.10.100

# ARP 测试（可选；arping 不写内核邻居表）
sudo ip neigh flush dev ens37
sudo arping -I ens37 -c 3 10.10.10.100

# 让内核自己解析并缓存（ICMP 已实现，ping 会收到 Reply）
sudo ip neigh flush dev ens37
ping -c 1 -W 1 10.10.10.100
ip neigh show 10.10.10.100

# 发送 UDP
printf 'hello-dpdk' | nc -u -w 1 10.10.10.100 9999

# 可选：手工静态邻居（调试用，正常不需要）
# sudo ip neigh replace 10.10.10.100 lladdr <DPDK MAC> nud permanent dev ens37

# ===== 回显测试（固定源端口并等待） =====
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.10.10.10", 50000))
s.settimeout(3)
s.sendto(b"hello-dpdk", ("10.10.10.100", 9999))
print(s.recvfrom(2048))
PY
# 期望： (b'hello-dpdk', ('10.10.10.100', 9999))

# ===== TCP 回显测试 =====
python3 - <<'PY'
import socket
s = socket.create_connection(("10.10.10.100", 9999), timeout=3)
s.sendall(b"hello-tcp")
print(s.recv(2048))
s.close()
PY
# 期望： b'hello-tcp'

# ===== ICMP 测试（ping） =====
sudo ip neigh flush dev ens37   # 可选：强制先走 ARP
ping -c 4 10.10.10.100
# 期望：4 packets transmitted, 4 received, 0% packet loss

# ===== testpmd =====
sudo ~/dpdk-stable-24.11.7/build/app/dpdk-testpmd \
  -l 0-1 -a 0000:0b:00.0 --iova-mode=pa -- \
  -i --rxq=1 --txq=1 --disable-rss
#   show port info 0
#   set fwd rxonly
#   set verbose 1
#   start
#   show port stats all

# ===== 换回内核驱动做 tcpdump =====
sudo ./dpdk-devbind.py -u 0000:0b:00.0
sudo modprobe vmxnet3
sudo ./dpdk-devbind.py --bind=vmxnet3 0000:0b:00.0
ip -br link
sudo ip link set <接口名> up
sudo tcpdump -i <接口名> -nn -e udp port 9999

# ===== 恢复 DPDK =====
sudo ./dpdk-devbind.py -u 0000:0b:00.0
sudo ./dpdk-devbind.py --bind=vfio-pci 0000:0b:00.0

# ===== 检查 =====
grep Huge /proc/meminfo
sudo ./dpdk-devbind.py --status
pkg-config --modversion libdpdk
```

---

## 13. 每次开机的前置步骤

1. 分配并挂载大页；
2. `modprobe vfio-pci`，开启 `enable_unsafe_noiommu_mode`；
3. 用 `dpdk-devbind.py` 把网卡绑定到 `vfio-pci`；
4. 确认绑定状态；
5. 运行 `ustack --iova-mode=pa`。

详见 `../虚拟机启动dpdk前置命令-每一次开机都要做.md`。

---

## 14. 相关文件

| 文件 | 说明 |
|------|------|
| `ustack.c` | 用户态协议栈源码（含详细注释） |
| `Makefile` | 从 DPDK helloworld 示例修改而来 |
| `build/` | 编译产物目录 |
| `../README.md` | DPDK 目录总览 |
| `../在共享文件夹中编译对应的dpdk协议栈文件.txt` | 共享文件夹编译说明 |
| `../虚拟机启动dpdk前置命令-每一次开机都要做.md` | 每次开机的前置命令 |
| `../../README.md` | `cpp_internet_study` 网络编程仓库总览 |
| `../../../README.md` | 工作区根 README（顶层目录索引） |
