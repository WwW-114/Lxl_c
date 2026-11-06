# DPDK 实验目录

本目录记录 DPDK 用户态协议栈的学习与实验，当前包含：

```
DPDK/
├── Ustack/                                      DPDK 用户态协议栈实验
│   ├── ustack.c                                 UDP/TCP/ICMP 收包/解析/回显 + ARP 源码
│   ├── Makefile                                 从 DPDK helloworld 示例修改而来
│   ├── build/                                   编译产物
│   └── README.md                                完整实验文档（推荐先读）
├── 在共享文件夹中编译对应的dpdk协议栈文件.txt     共享文件夹编译说明
└── 虚拟机启动dpdk前置命令-每一次开机都要做.md     每次开机的大页/vfio/网卡绑定步骤
```

完整的环境、原理、排错过程、帧格式和命令速查见
[`Ustack/README.md`](Ustack/README.md)。本文件只做目录级总览和关键路径备忘。

---

## 1. 当前环境与路径

| 项 | 值 |
|----|----|
| 虚拟机 | VMware Workstation + Ubuntu |
| 虚拟网卡 | VMware `vmxnet3` |
| DPDK 版本 | 24.11.7 |
| DPDK 源码路径 | `~/dpdk-stable-24.11.7` |
| DPDK 工具目录 | `~/dpdk-stable-24.11.7/usertools` |
| testpmd | `~/dpdk-stable-24.11.7/build/app/dpdk-testpmd` |
| 源码（VM 共享目录） | `/mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack` |
| 源码（Windows） | `D:\daimacheshi\cpp_internet_study\DPDK\Ustack` |
| 网卡 PCI 地址 | `0000:0b:00.0`（按实际机器修改） |
| DPDK 网卡 MAC | 运行 `ustack` 时打印，例如 `00:0C:29:F4:81:B8`（非固定值） |
| 测试网段（示例） | 发送端 `10.10.10.10/24` → 目的 `10.10.10.100` |
| 测试端口（示例） | UDP/TCP `9999`（代码不校验目的端口） |

> 后续输出命令时默认使用以上路径；路径不同时请自行替换。

---

## 2. 快速开始

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

# ===== 发送端（另一台同一 VMnet 的 Linux 虚拟机） =====
sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
ip -br addr show ens37
ip route get 10.10.10.100
sudo ip neigh flush dev ens37          # 清掉旧表项，让内核重新 ARP
printf 'hello-dpdk' | nc -u -w 1 10.10.10.100 9999
# 第一次发送会自动完成 ARP：ustack 回 Reply 后内核缓存 MAC
ip neigh show 10.10.10.100             # 期望 lladdr = DPDK MAC

# ===== 回显测试（固定源端口并等待） =====
python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("10.10.10.10", 50000))
s.settimeout(3)
s.sendto(b"hello-dpdk", ("10.10.10.100", 9999))
print(s.recvfrom(2048))
PY
```

期望接收端输出：

```text
length = 18, data_length = 10, payload = hello-dpdk
```

回显测试期望（发送端 Python 脚本）：

```text
(b'hello-dpdk', ('10.10.10.100', 9999))
```

> 回显的目的端口是发送端的**源端口**，不是 9999。`nc -u -w 1` 发完就退出，
> 收不到回显；必须绑定固定源端口并 `recvfrom()` 等待。详见
> [`Ustack/README.md` 第 6.6 节](Ustack/README.md#66-udp-回显测试)。

TCP 回显测试：

```python
import socket

s = socket.create_connection(("10.10.10.100", 9999), timeout=3)
s.sendall(b"hello-tcp")
print(s.recv(2048))
s.close()
```

期望客户端输出 `b'hello-tcp'`；ustack 端会依次打印 SYN、established、payload、FIN。
详见 [`Ustack/README.md` 第 6.7 节](Ustack/README.md#67-tcp-回显测试)。

ICMP 测试（ping）：

```bash
sudo ip neigh flush dev ens37   # 可选：强制先走 ARP
ping -c 4 10.10.10.100
```

期望 4 个 reply；ustack 端打印 `ICMP echo request ... replying`。
详见 [`Ustack/README.md` 第 6.8 节](Ustack/README.md#68-icmping测试)。

---

## 3. DPDK 基础知识（速览）

- **内核旁路**：网卡通过 `vfio-pci` 交给用户态，内核不再使用它；
- **轮询（PMD）**：用户态死循环调用 `rte_eth_rx_burst()`，不走中断；
- **大页**：mempool/ring 放在 hugetlbfs 上，减少 TLB miss，保证物理连续；
- **mbuf**：收发包的基本缓冲区，由 mempool 统一分配/回收；
- **port / queue / descriptor**：DPDK 网卡端口、收/发队列、环形描述符；
- **IOVA**：I/O 虚拟地址。虚拟机 no-IOMMU 环境必须使用 `--iova-mode=pa`；
- **PMD 驱动**：本例是 `net_vmxnet3`；虚拟测试还可用 `net_tap`；
- **promiscuous**：接收目的 MAC 不是自己的帧；
- **RSS/offload**：多队列散列和硬件卸载，本例未使用。

`ustack` 的初始化顺序：

```text
rte_eal_init()
  → rte_eth_dev_count_avail()
  → rte_pktmbuf_pool_create()
  → rte_eth_dev_configure(1 RX, 1 TX)
  → rte_eth_rx_queue_setup(512)
  → rte_eth_tx_queue_setup(512)
  → rte_eth_dev_start()
  → rte_eth_promiscuous_enable()
  → while (1) rte_eth_rx_burst()
        ├── ARP(0x0806) -> handle_arp()：学习 IP/MAC、回 ARP Reply
        ├── 解析 Ethernet / IPv4
        ├── TCP(6)  -> handle_tcp()：三次握手、数据回显、FIN/RST
        ├── UDP(17) -> handle_udp()：无状态回显
        └── ICMP(1) -> handle_icmp()：回 Echo Reply（ping）
```

---

## 4. 网卡绑定要点

```bash
# 加载 vfio-pci 并开启 no-IOMMU（虚拟机必需）
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode

# 绑定
cd ~/dpdk-stable-24.11.7/usertools
sudo ./dpdk-devbind.py --bind=vfio-pci 0000:0b:00.0

# 查看状态
sudo ./dpdk-devbind.py --status
```

绑定后：

- 内核里没有这块网卡的 `ensXX` 接口；
- 内核里没有 IP、路由、ARP 应答（ARP 由 ustack 用户态实现）；
- 不能 `tcpdump`，只能由 DPDK 程序使用；
- 程序运行时必须加 `--iova-mode=pa`。

需要做内核 `tcpdump` 对比时：

```bash
sudo ./dpdk-devbind.py -u 0000:0b:00.0
sudo modprobe vmxnet3
sudo ./dpdk-devbind.py --bind=vmxnet3 0000:0b:00.0
```

---

## 5. 巨页/大页设置（Hugepages）

```bash
# 1024 个 2MB 大页 = 2GB
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge

grep Huge /proc/meminfo
```

检查 `HugePages_Total` / `HugePages_Free`。重启后需要重新执行。

---

## 6. 两端网络连接（关键）

这是本实验最容易出错的地方，完整说明见
[`Ustack/README.md` 第 6 节](Ustack/README.md#6-两端如何实现正确的网络连接本实验最重要的部分)。

必须同时满足：

1. **发送端测试网卡和 DPDK 网卡在同一个 VMware VMnet**，不是“IP 同网段”；
   - NAT（VMnet8）和仅主机（VMnet1）是两个独立虚拟交换机；
   - 两台 VM 在不同物理主机上时，NAT 模式互不相通；
   - 推荐：发送端新增一块仅主机网卡，与 DPDK 网卡同一 VMnet。
2. **ARP 已由 ustack 实现**：发送端内核会发 ARP Request，ustack 回 Reply，
   内核自动缓存 MAC；不再需要手工静态邻居。手工静态邻居仍可选（调试用）：
   ```bash
   # 可选，正常不需要
   sudo ip neigh replace 10.10.10.100 lladdr <DPDK MAC> nud permanent dev ens37
   ```
3. **发送端 IP 直接用 `ip addr add` 配**，不需要 nmcli / 不需要改 NetworkManager：
   ```bash
   sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
   ip -br addr show ens37        # 注意：ip -br link 不显示 IP
   ```
4. **确认路由从测试网卡出去，并清掉旧邻居表项**：
   ```bash
   ip route get 10.10.10.100      # 期望 dev ens37 src 10.10.10.10
   sudo ip neigh flush dev ens37
   sudo arping -I ens37 -c 3 10.10.10.100   # 可选：验证 ARP 应答
   ```
5. 要填充内核邻居表，用 ping/UDP/TCP 触发内核 ARP；`arping` 不写内核邻居表。

典型排错信号：

| 现象 | 含义 |
|------|------|
| testpmd 只看到 `0x0806` 广播 ARP | ARP 请求到了；检查 ustack 是否回 Reply、目标 IP 是否匹配 |
| testpmd 看到 `0x0800` 且 `dst=<DPDK MAC>`、`L4_UDP` | UDP 单播已到达 DPDK 网卡 |
| TCP 客户端一直重传 SYN | 检查 SYN-ACK 的 IP/端口字节序、TCP 校验和、第三次 ACK 判断 |
| testpmd RX 一直为 0，Link up | VMnet 不通 / 帧没到 vNIC |
| Link down | VMware 适配器没连接 |

---

## 7. 验证工具

### testpmd

```bash
sudo ~/dpdk-stable-24.11.7/build/app/dpdk-testpmd \
  -l 0-1 -a 0000:0b:00.0 --iova-mode=pa -- \
  -i --rxq=1 --txq=1 --disable-rss
```

```text
testpmd> show port info 0
testpmd> set fwd rxonly
testpmd> set verbose 1
testpmd> start
testpmd> show port stats all
```

### 内核驱动 + tcpdump

```bash
# 绑定回 vmxnet3 后
sudo ip link set <接口名> up
sudo tcpdump -i <接口名> -nn -e udp port 9999
```

能抓到 → VMnet 正常，问题在 DPDK 侧；抓不到 → VMnet 不通。

### net_tap

完全绕开 VMware 网络，验证 ustack 代码本身：

```bash
sudo ./build/ustack --iova-mode=pa --vdev=net_tap0,iface=dtap0
```

---

## 8. Ustack Makefile 说明

`Ustack/Makefile` 复制自 DPDK 源码中的 `examples/helloworld/Makefile`，并做了
三处修改：

| # | 原内容 | 修改为 | 原因 |
|---|--------|--------|------|
| 1 | `APP = helloworld` | `APP = ustack` | 生成的可执行程序名改为 ustack |
| 2 | `SRCS-y := main.c` | `SRCS-y := ustack.c` | 源文件名必须和实际文件一致；曾误写为 `usatck.c`，make 报“没有规则可制作目标 usatck.c” |
| 3 | `ln -sf $(APP)-shared build/$(APP)` | `cp build/$(APP)-shared build/$(APP)` | VMware 共享目录 hgfs 不支持软链接，原命令报 `ln: Operation not supported` |

`static` 目标也做了同样的 `ln -sf` → `cp` 修改。`clean` 目标原有的
`rm -f build/$(APP)` 已能清理复制出来的文件。

完整说明见
[`Ustack/README.md` 第 9.1 节](Ustack/README.md#91-makefile-来源与修改) 和
[`在共享文件夹中编译对应的dpdk协议栈文件.txt`](在共享文件夹中编译对应的dpdk协议栈文件.txt)。

## 9. 相关文档

- [Ustack/README.md](Ustack/README.md)：完整实验文档（原理、排错、帧格式、命令速查）
- [在共享文件夹中编译对应的dpdk协议栈文件.txt](在共享文件夹中编译对应的dpdk协议栈文件.txt)：Makefile 来源、三处修改与 hgfs 编译注意
- [虚拟机启动dpdk前置命令-每一次开机都要做.md](虚拟机启动dpdk前置命令-每一次开机都要做.md)：大页、vfio、网卡绑定
- [../README.md](../README.md)：`cpp_internet_study` 网络编程仓库总览
