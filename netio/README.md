# netio —— 网络 IO 与协议实现

Linux 下 C/C++ 网络编程的练习集合。五个子项目分别打五个方向，
互相独立、没有依赖。前四个以 CMake 为主（`Mymulti_io` 暂未提供 `CMakeLists.txt`）；
`io_uring/` 直接使用 gcc + 仓库内自带的 liburing 源码构建。

---

## 子项目导航

| 目录 | 语言 | 是什么 | 详细文档 |
|------|------|--------|---------|
| [`multi_io/`](multi_io/) | C++17 | 完整版 Reactor 网络库：`select`/`poll`/`epoll` 三种后端 + `TcpServer` + 自写 HTTP 服务器 | [README.md](multi_io/README.md)（**1543 行深度教学文档**） |
| [`Mymulti_io/`](Mymulti_io/) | C++17 | 对照 `multi_io` 手写的练习版，用来检验掌握程度 | [README.md](Mymulti_io/README.md) |
| [`tcp_p2p/`](tcp_p2p/) | C11 | 纯 C 的 epoll reactor + TCP **对等**通信程序 | [README.md](tcp_p2p/README.md) |
| [`udp_kcp/`](udp_kcp/) | C++17 | 基于 KCP 的 UDP 文字聊天室 | [README.md](udp_kcp/README.md) |
| [`io_uring/`](io_uring/) | C | 基于 liburing 的 TCP Echo Server：`accept`/`recv`/`send` 全部走 SQ/CQ，演示完成通知模型 | [README.md](io_uring/README.md) |

---

## 建议的阅读顺序

```
                    ① 先弄懂"一个线程怎么盯住很多连接"
                                 │
                    ┌────────────┴────────────┐
                    ▼                         │
            multi_io/ 的 poller 层             │
            SocketUtils → EventPoller         │
            → Select / Poll / Epoll           │
                    │                         │
                    ② 字节流怎么变成"消息"      │
                    ▼                         │
            multi_io/ 的 TcpServer + HTTP     │
                    │                         │
        ┌───────────┴───────────┐             │
        ③ 换一种语言/组织方式    ④ UDP 上怎么做可靠传输
        ▼                       ▼             │
    tcp_p2p/                udp_kcp/          │
    （C + 回调表）           （KCP 协议）       │
                                              │
                    ⑤ 合上代码，自己写一遍 ◄───┘
                    ▼
              Mymulti_io/
```

| 阶段 | 做什么 | 完成后应该能回答 |
|------|--------|-----------------|
| ① | 读 `multi_io` 的 `EventPoller` 与三个后端 | select / poll / epoll 的时间复杂度差在哪？ET 为什么必须非阻塞？ |
| ② | 读 `multi_io` 的 `TcpServer` 与 `net/http` | 为什么必须有应用层发送缓冲区？HTTP 报文边界怎么切？ |
| ③ | 读 `tcp_p2p` | 不用虚函数，用函数指针表怎么实现同样的分发？ |
| ④ | 读 `udp_kcp` | KCP 怎么在不可靠的 UDP 上做到不丢不乱？为什么必须定时驱动？ |
| ⑤ | 写 `Mymulti_io` | 前四步是否真的进了脑子 |

> 第 ① 步讲的是 **epoll 的就绪通知**：fd 可读了，应用再调用 `recv`。
> `io_uring/` 是同一问题的另一种答案——**完成通知**：应用把 `accept` /
> `recv` / `send` 直接提交给内核，内核做完后通过 CQE 通知。
> 建议在理解 `multi_io` 的 poller 层之后，把 `io_uring/README.md`
> 当作第 ① 步的延伸阅读。

---

## 各项目解决的问题各不相同

```
    应用层协议                 ┌──────────────┐
    （HTTP 报文怎么切）  ────► │ multi_io/    │  C++，类继承 + 虚函数
                              │   net/http   │
                              └──────────────┘
                                     ▲
    事件分发机制               ┌──────────────┐     ┌──────────────┐
    （谁就绪了就通知谁）  ────► │ multi_io/    │     │ tcp_p2p/     │
                              │ EventPoller  │     │  reactor.c   │
                              └──────────────┘     └──────────────┘
                                C++ 抽象接口           C 回调函数表
                                     ▲                      ▲
    可靠的字节流               ┌──────────────┐             │
    （TCP 帮我们做好了）  ────► │ multi_io/    │             │
                              │  TcpServer   │◄────────────┘
                              └──────────────┘
                                     ▲
    不可靠的数据报             ┌──────────────┐
    （要靠协议自己补救）  ────► │ udp_kcp/     │
                              │  KCP 层      │
                              └──────────────┘
```

`io_uring/` 位于同一层的「事件分发机制」，但接口方向相反：
epoll 告诉你「fd 就绪了」，io_uring 告诉你「你提交的操作完成了」。

---

## 各项目的编译与运行

### `multi_io`

```bash
cd multi_io
cmake -S . -B build && cmake --build build -j

./build/echo_server 9000 epoll et          # 端口 后端 触发模式
./build/echo_client 127.0.0.1 9000 2000 4096   # 主机 端口 消息数 每条字节数
./build/http_server 8080 auto lt www       # 浏览器打开 http://127.0.0.1:8080/
./build/http_client 127.0.0.1 8080 8388608 # 27 项断言的自动化测试
```

Windows 上用 `build_msvc.bat`（首次需改里面的 `VSROOT`）。

### `Mymulti_io`

暂无 `CMakeLists.txt`，详见该目录的 README。

### `tcp_p2p`

```bash
cd tcp_p2p
cmake -S . -B build && cmake --build build -j

./build/tcp_p2p -l 9001            # 监听
./build/tcp_p2p -c 127.0.0.1 9001  # 连接对端
```

### `udp_kcp`

```bash
cd udp_kcp
cmake -S . -B build && cmake --build build -j

./build/server 8888
./build/client 127.0.0.1 8888 alice
```

### `io_uring`

```bash
cd io_uring

# 构建仓库内自带的 liburing（只需一次）
(cd liburing-master && ./configure && make -C src -j"$(nproc)")

# 静态链接 liburing，编译 echo_server
gcc -O2 -Wall -Wextra -D_GNU_SOURCE \
    -I liburing-master/src/include \
    echo_server.c liburing-master/src/liburing.a \
    -o echo_server

./echo_server
# 另一个终端：nc 127.0.0.1 9999
```

需要 Linux 内核 ≥ 5.6（`accept` 5.5+、`recv`/`send` 5.6+）；
Windows / MSVC 不支持。

---

## 共同的技术底色

这些项目反复出现同一批概念，只是换语言、协议或 IO 模型各实现一遍：

| 概念 | 出现在 |
|------|--------|
| 非阻塞 fd + 循环读写到 `EAGAIN` | `multi_io/TcpServer`、`tcp_p2p/peer.c`、`udp_kcp/server.cpp` |
| 应用层发送缓冲区（处理部分写） | `multi_io/TcpServer`、`tcp_p2p/peer_t::send_buff`、`io_uring/conn.send_off` |
| 事件循环 + 定时器驱动 | `multi_io`（超时参数）、`udp_kcp`（`ikcp_check` 算下一次唤醒时刻）、`io_uring/event_loop` |
| 连接生命周期与回调中的对象失效 | `multi_io/conns_`、`udp_kcp/Session`、`tcp_p2p/peer_t`、`io_uring/conns[]` |
| `SIGPIPE` 处理 | 四者都做了（Linux 上往已关闭的 socket 写会直接杀进程） |

---

## 环境要求

- Linux（`multi_io` 额外支持 Windows + MSVC；`io_uring/` 需要内核 ≥ 5.6）
- C++17 / C11，CMake ≥ 3.13（`io_uring/` 用 gcc + 仓库内 liburing 源码，不用 CMake）
- 主线项目无第三方依赖；`io_uring/` 的 `liburing-master` 是随仓库提供的第三方源码
  （v2.15），构建前先执行 `./configure && make -C src`
