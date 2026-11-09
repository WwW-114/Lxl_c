# cpp_internet_study —— Linux 下的 C/C++ 网络编程学习

记录在 **Linux** 系统上学习 **C/C++ 网络编程** 的过程。主线是：从裸 socket 出发，
经过 IO 多路复用（epoll），再到应用层协议（HTTP）、可靠 UDP（KCP）、协程化网络 I/O，
以及基于 **io_uring** 的完成式异步 I/O。
除保留的第三方 `NtyCo-master`、`DPDK/Ustack` 使用 Makefile，`netio/io_uring`
使用 gcc + 自带的 liburing 源码，以及 `test/` 使用 gcc + pthread 外，其余子项目
统一使用 **CMake** 构建。

此外还有一个 **DPDK 用户态协议栈**分支（`DPDK/Ustack`）：用 DPDK 接管虚拟网卡，
在用户态轮询收包、解析 Ethernet/IPv4/UDP/TCP/ICMP 并回显给发送端，同时实现 ARP
应答器和 ICMP Echo Reply（支持 ping），使用 Makefile + pkg-config 构建。

此外，`pool_struct/` 是池化结构与并发模式的学习分支（当前包含线程池），
与网络主线相对独立。

---

## 目录结构

```
cpp_internet_study/
├── netio/                        网络 IO
│   ├── multi_io/                 [C++17] 完整的 Reactor 网络库 + HTTP 服务器
│   ├── Mymulti_io/               [C++17] 对照 multi_io 手写的练习版（进行中）
│   ├── tcp_p2p/                  [C11]   Reactor + TCP 点对点通信
│   ├── udp_kcp/                  [C++17] 基于 KCP 的 UDP 聊天室
│   └── io_uring/                 [C]     基于 liburing 的 TCP Echo Server（SQ/CQ 完成通知）
├── coroutine/                    [C++17 / C] 基于 ucontext + epoll 的协程网络 I/O 框架
│   └── NtyCo-master/             [C]     第三方 NtyCo 源码快照（参考实现，Makefile 构建）
├── test/                         [C]     TCP 回显服务器 QPS 压测客户端（多线程 / 多连接 / pipeline）
├── pool_struct/                  池化结构（非网络主线）
│   └── threadpool/               [C11] 队列与线程池分离的有界阻塞线程池（CMake）
└── DPDK/                         DPDK 用户态协议栈
    └── Ustack/                   [C]     轮询 vmxnet3，解析 IPv4 + UDP/TCP，Makefile 构建
```

---

## 子项目一览

| 项目 | 语言 | 平台 | 构建 | 一句话说明 |
|------|------|------|------|-----------|
| [`netio/multi_io`](netio/multi_io/) | C++17 | Linux / Windows | CMake | select / poll / epoll 三种后端统一抽象，上面架 Reactor 的 `TcpServer`，再往上是自写的 HTTP 服务器 |
| [`netio/Mymulti_io`](netio/Mymulti_io/) | C++17 | Linux | 暂无 | 不看原码、照着自己的理解重写 `multi_io`，用来检验是否真的掌握了 |
| [`netio/tcp_p2p`](netio/tcp_p2p/) | C11 | Linux | CMake | 用 C 写的 epoll reactor，上面做一个**对等**的 TCP 通信程序，两端都能监听也能连接 |
| [`netio/udp_kcp`](netio/udp_kcp/) | C++17 | Linux | CMake | 基于 skywind3000 的 KCP 实现的文字聊天室：昵称登录、群聊广播、心跳断线检测 |
| [`netio/io_uring`](netio/io_uring/) | C | Linux（内核 ≥ 5.6） | gcc + 仓库内 liburing | 用 liburing 的 accept/recv/send 实现 TCP 回显；单次 shot 模型，演示 SQE/CQE、`user_data` 编码、短写与缓冲区生命周期 |
| [`test`](test/) | C | Linux | gcc + pthread | 面向 `io_uring/echo_server` 的 QPS 压测客户端：v1 一线程一连接同步发收；v2 多线程 × 多连接 × pipeline、应用层分帧，统计 QPS / RTT / 吞吐并验证拆包与粘包 |
| [`coroutine`](coroutine/) | C++17 | Linux | CMake | 用 ucontext 保存协程栈、epoll 唤醒协程，把同步写法跑在非阻塞 socket 上 |
| [`coroutine/NtyCo-master`](coroutine/NtyCo-master/) | C | Linux | Makefile | 第三方 NtyCo 源码快照：ucontext 协程 + epoll + dlsym hook，附 echo / HTTP / WebSocket / MySQL / Redis / 压测示例，作为自写框架的参考实现 |
| [`pool_struct/threadpool`](pool_struct/threadpool/) | C11 | Linux | CMake | 队列与线程池分离的有界阻塞线程池：mutex + 条件变量、背压、优雅/立即关闭、pending 统计 |
| [`DPDK/Ustack`](DPDK/Ustack/) | C | Linux（VMware 虚拟机） | Makefile | DPDK 24.11.7 接管 vmxnet3，轮询收包，解析 Ethernet/IPv4/UDP/TCP/ICMP，打印 payload 并回显；实现 ARP 应答器和 ICMP Echo Reply（ping）；含大页、vfio、VMnet、ARP 动态解析的完整排错记录 |

> `coroutine/` 目录还保留了 `setjmp/longjmp` 与 `ucontext` 的最小实验，作为理解协程切换的前置材料，详见它自己的 README。
>
> `coroutine/` 下有两套实现：主线是自己用 C++17 写的框架（显式 `co::` API、每协程独立栈）；
> `NtyCo-master/` 是原样保留的第三方 C 参考实现（`dlsym` hook libc、调度器共享栈、红黑树定时器）。
> 两者解决同一个问题，取舍不同，详见各自的 README。

---

## 六条学习主线

按推荐阅读顺序，每一层解决上一层留下的问题：

| # | 项目 | 解决的问题 | 关键技术 |
|---|------|-----------|---------|
| 1 | `multi_io` | **一个线程怎么盯住成千上万条连接？** | 五种 IO 模型、select / poll / epoll、LT 与 ET、Reactor 模式 |
| 2 | `multi_io` 的 HTTP 层 | **TCP 是字节流，没有"消息"这个概念，协议怎么解析？** | 增量解析状态机、粘包/拆包、keep-alive、管线化、请求走私防护、静态文件安全 |
| 3 | `tcp_p2p` | **不用 C++ 的类，纯 C 怎么把 reactor 搭起来？两端对等又如何组织？** | 回调表（`slots[fd]`）、连接状态机、非阻塞 connect、stdin 也注册进事件循环 |
| 4 | `udp_kcp` | **UDP 不可靠，怎么做可靠传输？** | KCP 协议、ARQ 重传、`ikcp_check` 定时驱动、会话管理与心跳超时 |
| 5 | `coroutine` | **同步代码最好写，但一个线程阻塞一次就停住了，怎么兼顾？** | ucontext、协程栈切换、非抢占调度、非阻塞 socket、epoll 挂起/唤醒 |
| 6 | `io_uring` | **epoll 的「就绪通知」仍要应用自己发起 IO，能不能把操作本身交给内核？** | SQ/CQ 环形队列、SQE/CQE、`user_data`、完成通知、one-shot 与 multishot 的区别 |

`Mymulti_io` 不占主线，它是第 1、2 层学完之后的**自测**：

> 合上 `multi_io` 的代码，从零写一遍。写不出来的地方，就是没真懂的地方。

---

## 压测验证（`test/`）

`test/` 不占学习主线，用来验证 `netio/io_uring/echo_server` 的回显性能，
也可以压任何“收到多少回显多少”的 TCP 服务：

| 文件 | 说明 |
|------|------|
| `test_qps_tcpclient.c` | v1 简单版：每个线程一条连接，同步 `send` 一包、`recv` 一包，`strcmp` 校验；只能做冒烟测试 |
| `test_qps_tcpclient_v2.c` | v2 完整版：多线程 × 多连接 × pipeline，12 字节帧头（magic/seq/len）+ 逐字节 payload 校验，处理短写、半包、`EAGAIN`、连接失败，统计 QPS / RTT / 吞吐 / 拆包次数 / 粘包批数 |

```bash
# 终端 1：服务端 stdout 必须重定向，否则测到的是终端 I/O 速度
cd netio/io_uring && ./echo_server > /dev/null

# 终端 2：编译并压测
cd test
gcc -O2 -Wall -Wextra -D_GNU_SOURCE -pthread \
    test_qps_tcpclient_v2.c -o test_qps_tcpclient_v2
./test_qps_tcpclient_v2 -s 127.0.0.1 -p 9999 -t 4 -c 64 -n 200000 -b 1920 -P 4
```

细节、参数、指标解释和拆包/粘包复现方法见 [`test/README.md`](test/README.md)。

---

## DPDK 用户态协议栈（分支）

`DPDK/Ustack` 是另一条线：不走内核协议栈，而是用 DPDK 把一块 VMware
`vmxnet3` 虚拟网卡绑定到用户态，轮询收包、解析 Ethernet II / IPv4 / UDP/TCP/ICMP，
把 payload 原样回显给发送端，实现 ARP 应答器（发送端可动态解析 MAC）
和 ICMP Echo Reply（发送端可以直接 ping）。
它和上面的 socket 项目形成对照：

> 上面的项目都在使用内核协议栈；这里把网卡从内核手里拿走，自己处理二层/三层/四层。

> **DPDK 核心概念**：EAL（环境抽象层）、lcore、hugepage（巨页）、mempool/mbuf、
> PMD（轮询驱动）、port/queue/descriptor、burst、IOVA（no-IOMMU 下用 PA）、
> promiscuous。完整说明见 [`DPDK/Ustack/README.md`](DPDK/Ustack/README.md#3-dpdk-基础知识)。

### 快速入口

```bash
# 每次开机：大页 + vfio（完整命令见 DPDK/Ustack/README.md）
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
sudo mkdir -p /mnt/huge && sudo mount -t hugetlbfs nodev /mnt/huge
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
cd ~/dpdk-stable-24.11.7/usertools
sudo ./dpdk-devbind.py --bind=vfio-pci 0000:0b:00.0

# 编译与运行
cd /mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack
make
sudo ./build/ustack --iova-mode=pa

# 发送端（另一台与 DPDK 网卡同一 VMnet 的 Linux 虚拟机）
sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
ip route get 10.10.10.100
sudo ip neigh flush dev ens37
printf 'hello-dpdk' | nc -u -w 1 10.10.10.100 9999
```

> ARP 由 ustack 应答，发送端不再需要静态邻居；第一次发送会自动解析 MAC。
> 单独验证 ARP 可用 `sudo arping -I ens37 -c 3 10.10.10.100`。
> 回显测试要绑定固定源端口并等待；`nc` 发完就退出，源端口关闭，收不到回显。
> 详见
> [`DPDK/Ustack/README.md` 第 6.6 节](DPDK/Ustack/README.md#66-udp-回显测试)。

TCP 回显测试（客户端先完成三次握手，再发数据）：

```python
import socket

s = socket.create_connection(("10.10.10.100", 9999), timeout=3)
s.sendall(b"hello-tcp")
print("echo:", s.recv(2048))
s.close()
```

详见 [`DPDK/Ustack/README.md` 第 6.7 节](DPDK/Ustack/README.md#67-tcp-回显测试)。

ICMP 测试（ping）：

```bash
sudo ip neigh flush dev ens37   # 可选：强制先走 ARP
ping -c 4 10.10.10.100
```

期望 4 个 reply；详见
[`DPDK/Ustack/README.md` 第 6.8 节](DPDK/Ustack/README.md#68-icmping测试)。

### 环境与路径

| 项 | 值 |
|----|----|
| DPDK 版本 / 源码路径 | 24.11.7 / `~/dpdk-stable-24.11.7` |
| 工具目录 | `~/dpdk-stable-24.11.7/usertools` |
| testpmd | `~/dpdk-stable-24.11.7/build/app/dpdk-testpmd` |
| 源码（VM 共享目录） | `/mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack` |
| 源码（Windows） | `D:\daimacheshi\cpp_internet_study\DPDK\Ustack` |
| 网卡 PCI | `0000:0b:00.0`（按实际机器修改） |

### 必须记住的几点

1. **VMnet 比 IP 网段更重要**：发送端测试网卡和 DPDK 网卡必须在同一个
   VMware VMnet（同一个虚拟二层交换机）。NAT（VMnet8）和仅主机（VMnet1）
   是两个独立交换机，IP 同网段也收不到。
2. **ARP 已实现**：发送端内核通过 ARP Request/Reply 自动解析 DPDK 网卡 MAC，
   不再需要手工静态邻居；`arping` 只验证 ARP 应答，不写内核邻居表，
   要让内核学习请用 ping/UDP/TCP。
3. **发送端 IP 直接用 `ip addr add` 配**，不需要 nmcli，也不需要修改
   NetworkManager：
   ```bash
   sudo ip addr add 10.10.10.10/24 dev ens37; echo "rc=$?"
   ip -br addr show ens37   # ip -br link 不显示 IP
   ```
4. **必须加 `--iova-mode=pa`**：虚拟机 vfio 以 no-IOMMU 模式运行，网卡 DMA
   只能用物理地址。
5. **大页和 vfio 绑定每次开机都要重新做**。
6. **TCP 是最小实现**：单连接、按序数据、无重传/拥塞控制；客户端必须先完成
   三次握手才能发数据；SYN-ACK 的 IP/端口字节序和 TCP 校验和要正确，
   否则客户端会一直重传 SYN。

完整的 DPDK 基础、UDP/TCP 帧头解析、testpmd/内核 tcpdump 排错、
`ustack.c` 已修复 bug 总结，见 [`DPDK/Ustack/README.md`](DPDK/Ustack/README.md)。

---

## 构建方式

除 `NtyCo-master`、`DPDK/Ustack`、`netio/io_uring` 和 `test/` 外，其余子项目使用
**CMake** 构建（`Mymulti_io` 暂未提供 `CMakeLists.txt`），**在各自的子目录里**执行：

```bash
cd netio/<项目名>
cmake -S . -B build
cmake --build build -j
```

`coroutine` 在仓库根目录下，构建方式相同：

```bash
cd coroutine
cmake -S . -B build
cmake --build build -j
./build/test_coroutine
```

第三方快照 `coroutine/NtyCo-master` 使用 Makefile，不参与上面的 CMake 构建：

```bash
cd coroutine/NtyCo-master
make           # 生成 objs/*.o 与静态库 libntyco.a
make bin       # 生成 bin/ 下的全部示例
```

`make bin` 中的 MySQL / Redis / WebSocket 示例分别需要 `libmysqlclient-dev`、`hiredis`、
`libssl-dev`；只想跑基础示例时可以用 `make nty_server nty_client` 单独链接。
详见 [`coroutine/NtyCo-master/README.md`](coroutine/NtyCo-master/README.md)。

`DPDK/Ustack` 在 **虚拟机中的共享目录**里用 Makefile 构建（依赖 `pkg-config libdpdk`）：

```bash
cd /mnt/hgfs/daimacheshi/cpp_internet_study/DPDK/Ustack
make
sudo ./build/ustack --iova-mode=pa
```

> `Mymulti_io`、`netio/io_uring` 和 `test/` 没有 `CMakeLists.txt`：`Mymulti_io` 是手写练习，
> `netio/io_uring` 使用 gcc + liburing 直接构建，`test/` 使用 gcc + pthread 直接构建。

`netio/io_uring` 使用 gcc，并先把仓库内的 liburing 源码构建成静态库：

```bash
cd netio/io_uring
(cd liburing-master && ./configure && make -C src -j"$(nproc)")
gcc -O2 -Wall -Wextra -D_GNU_SOURCE \
    -I liburing-master/src/include \
    echo_server.c liburing-master/src/liburing.a \
    -o echo_server
./echo_server        # 另一个终端：nc 127.0.0.1 9999
```

需要 Linux 内核 ≥ 5.6；Windows / MSVC 无法编译。

`test/` 的两个压测客户端是单文件程序，用 gcc + pthread 编译：

```bash
cd test

# v2：多线程 / 多连接 / pipeline 压测（推荐）
gcc -O2 -Wall -Wextra -D_GNU_SOURCE -pthread \
    test_qps_tcpclient_v2.c -o test_qps_tcpclient_v2

# v1：简单同步版
gcc -O2 -Wall -Wextra -pthread \
    test_qps_tcpclient.c -o test_qps_tcpclient

# 运行（服务端需要先启动且 stdout 重定向）
./test_qps_tcpclient_v2 -s 127.0.0.1 -p 9999 -t 4 -c 64 -n 200000 -b 1920 -P 4
```

参数、指标含义与拆包/粘包复现方法见 [`test/README.md`](test/README.md)。

### 跨平台说明

| 项目 | Linux | Windows |
|------|:-----:|:-------:|
| `multi_io` | ✅ | ✅（MSVC，`select` 后端 + HTTP 层全部可用） |
| `Mymulti_io` | ✅ | ❌ |
| `tcp_p2p` | ✅ | ❌（直接依赖 `<sys/epoll.h>`） |
| `udp_kcp` | ✅ | ❌（POSIX socket；CMake 里有 `if(NOT UNIX)` 硬拦截） |
| `netio/io_uring` | ✅ | ❌（io_uring 是 Linux 内核接口） |
| `test` | ✅ | ❌（POSIX socket + pthread + poll） |
| `coroutine` | ✅ | ❌（`ucontext` + `epoll`，需走 WSL / Linux 机器） |
| `coroutine/NtyCo-master` | ✅ | ❌（`ucontext` + `epoll` + `dlsym`，仅 Linux） |
| `DPDK/Ustack` | ✅（仅 VMware Ubuntu 虚拟机） | ❌（需要 vfio、大页和 Linux DPDK 环境） |

### 常见命令

```bash
# 带调试符号（配 gdb）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# 查内存错误（udp_kcp 已内置该开关）
cmake -S . -B build-asan -DENABLE_ASAN=ON

# 换了 CMakeLists.txt 之后：重跑一次配置即可，不必删 build
cmake -S . -B build && cmake --build build -j

# 彻底重来
rm -rf build
```

### DPDK 相关常用命令

```bash
# 大页
grep Huge /proc/meminfo

# 网卡绑定状态
cd ~/dpdk-stable-24.11.7/usertools
sudo ./dpdk-devbind.py --status

# testpmd 收包验证
sudo ~/dpdk-stable-24.11.7/build/app/dpdk-testpmd \
  -l 0-1 -a 0000:0b:00.0 --iova-mode=pa -- -i --rxq=1 --txq=1 --disable-rss

# 发送端路由/邻居检查
ip -br addr show ens37
ip route get 10.10.10.100
ip neigh show 10.10.10.100
```

---

## 环境要求

| 项 | 版本 |
|----|------|
| 系统 | Linux（`multi_io` 额外支持 Windows + MSVC；`coroutine` 依赖 glibc 的 `ucontext` 与 `epoll`；`netio/io_uring` 需要内核 ≥ 5.6） |
| 编译器 | GCC / Clang，C++17 与 C11 |
| 构建 | CMake ≥ 3.13（`tcp_p2p` 要求 ≥ 3.10）；`NtyCo-master` 与 `DPDK/Ustack` 使用 Make；`netio/io_uring` 使用 gcc + 仓库内 liburing；`test/` 使用 gcc + pthread |
| 依赖 | 主线项目无第三方库。`udp_kcp` 自带的 `ikcp.c/h` 是 vendored 源码，不通过包管理器获取；`netio/io_uring` 的 `liburing-master`（v2.15）是随仓库提供的源码；`NtyCo-master` 的 WebSocket / MySQL / Redis 示例额外依赖 OpenSSL、libmysqlclient、hiredis；`test/` 只依赖 glibc 的 pthread/poll |
| io_uring | Linux 内核 ≥ 5.6；构建前在 `netio/io_uring/liburing-master` 执行 `./configure && make -C src` |
| DPDK | DPDK 24.11.7（`pkg-config libdpdk`），VMware 虚拟机，`vmxnet3` 网卡，vfio-pci + no-IOMMU + 大页 |

> **测试环境说明**：当前开发机是 **Windows**，没有安装 WSL，也没有本地 Linux 工具链。
> 源码在本机编写；`coroutine`、`tcp_p2p`、`udp_kcp`、`netio/io_uring`、`test` 等依赖
> Linux 系统调用的项目，统一在 **虚拟机中的 Linux** 上编译、运行和测试。
> `netio/io_uring` 还要求虚拟机内核 ≥ 5.6 且未通过 seccomp 禁用 io_uring。
> `test/` 的压测客户端同样在虚拟机中运行，并建议把服务端 stdout 重定向到
> `/dev/null` 后再测 QPS。
> `DPDK/Ustack` 还需要在虚拟机中完成大页分配、vfio 绑定和 VMnet 配置。

---

## 仓库约定

- **注释语言**：中文。解释「为什么这么做」而不只是「做了什么」。
- **构建产物**：CMake 项目一律生成在各自的 `build/` 子目录；`NtyCo-master` 保留原 Makefile 的
  `objs/`、`bin/` 布局（已在其 `.gitignore` 中忽略）；`DPDK/Ustack` 使用 `build/`；
  `netio/io_uring` 直接在目录下生成 `echo_server`，liburing 产物在 `liburing-master/src/`；
  `test/` 的可执行文件直接生成在 `test/` 目录下。
- **平台相关代码**：用 `if(UNIX)` / `if(WIN32)` 在 CMake 里隔离，或在源码里用 `#ifdef` 包住。
- **不引入外部依赖**：学习项目优先自己实现，能用标准库就不用第三方库。
  例外是 `netio/io_uring/liburing-master`（随仓库提供的 liburing 源码）和
  `udp_kcp` 自带的 `ikcp.c/h`。
- **第三方源码**：`coroutine/NtyCo-master/` 是原样保留的参考实现，保留其 Makefile 和外部依赖，
  不纳入主线的 CMake 构建，也不按本仓库的代码风格改写。
- **DPDK 实验**：`DPDK/Ustack/Makefile` 复制自 DPDK 官方示例
  `examples/helloworld/Makefile`，仅修改 `APP`（helloworld → ustack）和
  `SRCS-y`（main.c → ustack.c），并把 `ln -sf` 改为 `cp`——VMware 共享目录
  hgfs 不支持软链接（`ln: Operation not supported`）；编译/链接参数通过
  `pkg-config libdpdk` 获取。
