# io_uring —— 基于 liburing 的 TCP Echo Server

用 **liburing** 实现的 TCP 回显服务器。和 `multi_io/`、`tcp_p2p/` 解决的是同一个问题
（一个线程如何同时处理很多连接），但换了一套完全不同的机制：

> epoll 的思路是「**问内核：哪些 fd 就绪了？**」
> io_uring 的思路是「**告诉内核：帮我做这些操作，做完通知我。**」

- **平台**：仅 Linux（内核建议 ≥ 5.6）
- **构建**：gcc + 本目录下的 `liburing-master`（v2.15，源码构建）
- **规模**：单文件 `echo_server.c`
- **模型**：单次 shot（one-shot）accept / recv / send，单线程事件循环

---

## 1. 它做了什么

- 监听 `0.0.0.0:9999`
- 用 `io_uring_prep_accept()` 异步接受连接
- 用 `io_uring_prep_recv()` 异步读取数据
- 用 `io_uring_prep_send()` 把收到的数据原样发回
- 每条连接一块固定缓冲区，同一时刻只有一个在途操作
- 处理 send 短写（short write）
- 处理对端关闭、连接错误、`SIGPIPE`

一次连接的生命周期：

```
客户端 connect
      │
      ▼
 [OP_ACCEPT CQE] ──► 初始化 conns[fd] ──► add_recv(fd) + add_accept()
                                                    │
     客户端发送数据                                  ▼
                                        [OP_READ CQE]，res = n > 0
                                                    │
                                                    ▼
                                             add_send(fd)
                                                    │
                    ┌───────────────────────────────┴────────────────────┐
                    ▼                                                    ▼
        [OP_WRITE CQE]，只发了一部分                            [OP_WRITE CQE]，全部发完
                    │                                                    │
                    ▼                                                    ▼
              继续 add_send(fd)                                     add_recv(fd)
```

连接结束时（对端发送 FIN，recv 返回 0）直接 `close(fd)`，槽位标记为 `-1`。

---

## 2. 快速开始

以下命令都在 **Linux / WSL2** 中执行。io_uring 是 Linux 内核接口，
Windows 原生 MSVC 无法编译。

### 2.1 构建 liburing

```bash
cd cpp_internet_study/netio/io_uring

# 在 liburing-master 根目录生成 config-host.h / config-host.mak /
# src/include/liburing/io_uring_version.h
(cd liburing-master && ./configure)

# 只构建库（不构建 test/ 和 examples/，快很多）
(cd liburing-master && make -C src -j"$(nproc)")
```

产物：

```
liburing-master/src/liburing.a         静态库
liburing-master/src/liburing.so.2.15   动态库
```

> `./configure` 不能省略。`src/Makefile` 依赖根目录生成的 `config-host.h`，
> 公开头文件 `liburing.h` 也会包含生成的 `io_uring_version.h`。

### 2.2 编译 echo_server

静态链接最省事，运行时不需要设置 `LD_LIBRARY_PATH`：

```bash
gcc -O2 -Wall -Wextra -D_GNU_SOURCE \
    -I liburing-master/src/include \
    echo_server.c \
    liburing-master/src/liburing.a \
    -o echo_server
```

三个关键参数：

| 参数 | 作用 |
|------|------|
| `-D_GNU_SOURCE` | liburing 头文件和示例都要求打开 GNU 扩展 |
| `-I liburing-master/src/include` | 找到 `liburing.h` 及其子头文件 |
| `liburing-master/src/liburing.a` | 显式静态链接，避免动态库路径问题 |

也可以使用动态库（先 `sudo make install`，或者手动指定路径）：

```bash
# 方式一：安装到系统
(cd liburing-master && sudo make install)
gcc -O2 -Wall -Wextra -D_GNU_SOURCE echo_server.c -luring -o echo_server

# 方式二：不安装，直接链接构建目录
gcc -O2 -Wall -Wextra -D_GNU_SOURCE \
    -I liburing-master/src/include \
    echo_server.c -L liburing-master/src -luring -o echo_server
LD_LIBRARY_PATH=liburing-master/src ./echo_server
```

> 构建目录里默认只有 `liburing.so.2.15`，没有 `liburing.so` 这个软链接
> （`make install` 才会创建）。所以 `-L src -luring` 通常会优先链接到
> 同目录下的 `liburing.a`，实际上仍是静态链接。

### 2.3 运行验证

终端 1：

```bash
./echo_server
# io_uring echo server listening on port 9999
```

终端 2：

```bash
nc 127.0.0.1 9999
hello
# 客户端看到 hello 被原样回显
# 服务器端打印：recv 5 bytes: hello
```

### 2.4 二进制 / 大数据量回显测试

`nc` 各版本行为不完全一致，用 Python 做一次 1 MiB 的严格校验更可靠：

```bash
head -c 1048576 /dev/urandom > /tmp/in.bin

python3 - <<'PY'
import socket

data = open('/tmp/in.bin', 'rb').read()
s = socket.create_connection(('127.0.0.1', 9999))
s.sendall(data)
s.shutdown(socket.SHUT_WR)

out = bytearray()
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    out += chunk

assert bytes(out) == data, (len(out), len(data))
print('1 MiB echo OK')
PY
```

服务器会把收到的数据以十六进制以外的方式打印到 stdout（`fwrite`），
测试时可以重定向到 `/dev/null`：

```bash
./echo_server > /dev/null
```

---

## 3. io_uring 基础

### 3.1 SQ / CQ / SQE / CQE

```
                用户态                                    内核态
┌──────────────────────────────────┐
│  io_uring_get_sqe()              │
│        │                         │
│        ▼                         │
│  填写 SQE（opcode、fd、buf...）   │
│        │                         │
│  io_uring_submit()               │
│        └────────── SQ ring ──────┼──────► 执行 accept / recv / send
│                                  │
│  io_uring_for_each_cqe()         │
│        ▲                         │
│        └────────── CQ ring ◄─────┼────── 完成事件（res + user_data）
│                                  │
│  io_uring_cq_advance()           │
└──────────────────────────────────┘
```

- **SQE**（Submission Queue Entry）：应用填写的「请求」，包含操作码、fd、缓冲区、长度等。
- **CQE**（Completion Queue Entry）：内核填写的「完成结果」，只有 `user_data`、`res`、`flags` 三个有效字段。
- **`user_data`**：提交时由应用写入的 64 位值，完成时原样返回，用来区分是哪一个请求。
- **`res`**：完成结果。io_uring **不使用 errno**，错误直接以负数返回，例如 `-ECONNRESET`。

### 3.2 系统调用与 liburing 的对应关系

| 原来的写法 | io_uring 写法 |
|-----------|---------------|
| `accept()` | `io_uring_prep_accept()` |
| `recv()` | `io_uring_prep_recv()` |
| `send()` | `io_uring_prep_send()` |
| `epoll_wait()` | `io_uring_submit_and_wait()` |
| 遍历就绪事件 | `io_uring_for_each_cqe()` |
| 处理完事件 | `io_uring_cq_advance()` |

`io_uring_prep_*()` 只是往 SQE 里填参数，不会发起系统调用；真正的提交发生在
`io_uring_submit()` / `io_uring_submit_and_wait()`。

### 3.3 `user_data` 的编码

本项目只提交三种操作，于是把类型放在高 32 位、fd 放在低 32 位：

```
   63            32 31             0
   +---------------+---------------+
   |    op_type    |      fd       |
   +---------------+---------------+
```

对应代码：

```c
static uint64_t encode_ud(enum op_type type, int fd) {
  return ((uint64_t)type << 32) | (uint64_t)(uint32_t)fd;
}
```

CQE 返回时：

```c
switch (decode_type(cqe->user_data)) {
  case OP_ACCEPT: handle_accept(cqe); break;
  case OP_READ:   handle_recv(cqe);   break;
  case OP_WRITE:  handle_send(cqe);   break;
}
```

> 官方 `liburing-master/examples/echo-server.c` 用了更紧凑的编码：
> 高 8 位类型、次 16 位 buffer id、低 16 位 fd。本项目没有 provided buffer，
> 所以直接 32 + 32 更直观。

---

## 4. 代码结构

全部逻辑都在 `echo_server.c` 中。

### 4.1 连接表

```c
#define MAX_CONNS 1024
#define BUFF_SIZE 4096

struct conn {
  int fd;                 /* -1 表示槽位空闲 */
  char buff[BUFF_SIZE];   /* 收 / 发共用缓冲区 */
  size_t send_len;        /* 本次要回显的总字节数 */
  size_t send_off;        /* 已经发送的字节数 */
};

static struct conn conns[MAX_CONNS];
```

**fd 直接作为数组下标**，所以 `conns[fd]` 是 O(1) 查找。代价是 fd 必须小于
`MAX_CONNS`，连接数也受此限制。

### 4.2 函数一览

| 函数 | 职责 |
|------|------|
| `encode_ud()` / `decode_type()` / `decode_fd()` | 打包 / 解包 `user_data` |
| `get_sqe()` | 获取 SQE；SQ 满时先 `io_uring_submit()` 再重试 |
| `close_conn()` | `close(fd)` 并把槽位标记为 `-1` |
| `add_accept()` | 提交一次 accept（one-shot） |
| `add_recv()` | 为连接提交一次 recv |
| `add_send()` | 提交一次 send，从 `send_off` 开始 |
| `handle_accept()` | 新连接初始化 + 重新 arm accept |
| `handle_recv()` | 处理数据 / EOF / 错误，转入 send |
| `handle_send()` | 处理短写，发完后重新 recv |
| `event_loop()` | 提交、等待、批量收割 CQE |
| `main()` | socket / bind / listen、初始化 ring |

### 4.3 事件循环

```c
static void event_loop(void) {
  add_accept();

  for (;;) {
    int ret = io_uring_submit_and_wait(&ring, 1);
    if (ret < 0) {
      if (ret == -EINTR || ret == -EAGAIN)
        continue;
      fprintf(stderr, "io_uring_submit_and_wait: %s\n", strerror(-ret));
      return;
    }

    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned done = 0;

    io_uring_for_each_cqe(&ring, head, cqe) {
      switch (decode_type(cqe->user_data)) {
        case OP_ACCEPT: handle_accept(cqe); break;
        case OP_READ:   handle_recv(cqe);   break;
        case OP_WRITE:  handle_send(cqe);   break;
      }
      ++done;
    }

    io_uring_cq_advance(&ring, done);
  }
}
```

四个动作、一个都不能少：**提交 → 等待 → 遍历 → advance**。

---

## 5. 设计要点

### 5.1 accept 是 one-shot，必须重新 arm

`io_uring_prep_accept()` 每次只接受一个连接。`handle_accept()` 无论成功还是失败，
最后都会再调用一次 `add_accept()`。漏掉这一步的典型现象是：**服务器只能接入第一个客户端**。

> 5.19 之后有 `io_uring_prep_multishot_accept()`，一次提交可以持续产生 accept CQE，
> 见第 8 节。

### 5.2 每连接一个在途操作，buffer 不需要复杂管理

同一连接的操作是串行的：

```
recv 完成 → send → send 完成 → recv ...
```

在 send 完成之前不会为同一连接提交新的 recv，因此 `conns[fd].buff`
不会被下一批数据覆盖，也就**不需要** provided buffer ring 或多缓冲区轮转。

代价是：同一连接同一时刻只能有一个 recv 在途，吞吐不如 multishot 版本，
但逻辑清晰，适合理解 io_uring 的基本模型。

### 5.3 短写必须处理

`send` 返回的字节数可能小于请求长度（内核发送缓冲区满了）。代码用两个计数器：

```c
conns[fd].send_len = (size_t)n;  /* 本次要发 n 字节 */
conns[fd].send_off = 0;

/* 每次 send 完成后 */
conns[fd].send_off += (size_t)n;
if (conns[fd].send_off < conns[fd].send_len)
  add_send(fd);   /* 没发完，继续 */
else
  add_recv(fd);   /* 发完了，继续读 */
```

### 5.4 `cqe->res` 是负数 errno

同步系统调用失败返回 `-1` 并设置 `errno`；io_uring 直接在 `cqe->res` 中返回
`-errno`。因此：

```c
if (cqe->res < 0)
  fprintf(stderr, "recv: %s\n", strerror(-cqe->res));
```

不要判断 `cqe->res == -1`，也不要读 `errno`。

### 5.5 SQ ring 满时先提交

`io_uring_get_sqe()` 在 SQ ring 满时返回 `NULL`。`get_sqe()` 的处理是：
先 `io_uring_submit()` 把已有 SQE 交给内核，腾出槽位后再取一次。

### 5.6 `io_uring_submit_and_wait` 的 `wait_nr` 必须是 1

```c
io_uring_submit_and_wait(&ring, 1);   /* 正确：提交并等待至少 1 个 CQE */
io_uring_submit_and_wait(&ring, 0);   /* 错误：只提交，不等待 */
```

从 `liburing` 的 `src/queue.c` 可以看到，`wait_nr == 0` 时不会设置
`IORING_ENTER_GETEVENTS`，函数立刻返回。如果此时 CQ 里没有事件，
`io_uring_for_each_cqe` 遍历 0 个 CQE，循环会空转，**CPU 占用 100%**。

### 5.7 处理完必须 `io_uring_cq_advance`

`io_uring_for_each_cqe` 只是遍历，不会消费 CQE。遍历结束后必须：

```c
io_uring_cq_advance(&ring, done);
```

否则下一轮还会看到同一批 CQE，表现为重复回显、重复 accept 等。

### 5.8 fd 边界检查

`conns[fd]` 要求 `0 <= fd < MAX_CONNS`。`handle_accept()` 中如果
`fd >= MAX_CONNS`，无法放入连接表，只能 `close(fd)` 并继续 accept。
`close_conn()` 里同样做边界检查，防止数组越界。

> 注意合法下标的上界是 `MAX_CONNS - 1`，所以判断条件是 `fd >= MAX_CONNS`，
> 不是 `fd > MAX_CONNS`。

### 5.9 SIGPIPE

向已经关闭的 socket 写数据，Linux 默认会向进程发送 `SIGPIPE`，默认动作是终止进程。
本项目有两层防护：

- `io_uring_prep_send(..., MSG_NOSIGNAL)`：让 send 返回 `-EPIPE`
- `signal(SIGPIPE, SIG_IGN)`：兜底

### 5.10 不需要手动设置非阻塞

io_uring 对 socket 的 recv/send/accept 会自行处理「现在不能做」的情况：
先尝试非阻塞执行，返回 `-EAGAIN` 时自动挂到内部 poll 上，完成后产生 CQE。
因此**不需要** `fcntl(fd, F_SETFL, O_NONBLOCK)`。

---

## 6. 与 epoll 方案的对照

| 维度 | `multi_io/`（epoll） | `tcp_p2p/`（epoll） | 本项目（io_uring） |
|------|---------------------|---------------------|-------------------|
| 事件来源 | `epoll_wait()` 返回就绪 fd | 同左 | CQ ring 中的 CQE |
| 谁发起 IO | 应用在就绪后调用 `recv/send` | 同左 | 应用提交 SQE，内核执行 |
| 每次事件系统调用 | `epoll_wait` + `recv/send` | 同左 | `io_uring_enter`（批量提交/收割） |
| 缓冲区 | 应用层发送队列 | 定长 recv + 堆 send | 每连接定长 buffer + 短写计数 |
| 连接表 | `unordered_map` | `slots[fd]` 数组 | `conns[fd]` 数组 |
| 内核要求 | epoll（2.6+） | 同左 | io_uring（5.6+） |
| 语言 | C++17 | C11 | C |

概念上的关键变化：

- epoll 是**就绪通知**：fd 可读/可写时通知你，真正的 `read/write` 仍由应用发起。
- io_uring 是**完成通知**：应用直接提交「读 4096 字节」这个操作，内核做完后通知你结果。
- 因此 io_uring 天然支持批量提交和批量收割，减少系统调用次数，也为真正的异步 IO
  （文件 IO、`O_DIRECT` 等）提供了统一接口。

---

## 7. 常见问题排查

| 现象 | 可能原因 | 处理 |
|------|---------|------|
| `io_uring_queue_init: Function not implemented` | 内核 < 5.1 或未开启 io_uring | `uname -r`，换新内核 |
| `io_uring_queue_init: Operation not permitted` | 容器 seccomp / 沙箱禁用 `io_uring_setup` | 放宽 seccomp，或在宿主机运行 |
| 编译报 `liburing.h: No such file` | 没加 `-I liburing-master/src/include` | 补 include 路径 |
| 链接报 `undefined reference to io_uring_*` | 没链接 `liburing.a` 或 `-luring` | 补库路径和库名 |
| 运行时 `error while loading shared libraries: liburing.so.2` | 动态链接但没有库路径 | 静态链接，或设置 `LD_LIBRARY_PATH` |
| 服务器 CPU 100%，但没有连接 | `io_uring_submit_and_wait(&ring, 0)` | 改成 `wait_nr = 1` |
| 只能接入第一个客户端 | 忘记在 `handle_accept` 末尾重新 `add_accept()` | 每次 accept CQE 后重新 arm |
| 回显重复、CQE 被处理多次 | 忘记 `io_uring_cq_advance()` | 遍历后统一 advance |
| 收到数据后打印乱码 / 越界 | 用 `%s` 打印非 NUL 结尾的 TCP 数据 | 用 `fwrite(buf, 1, n, stdout)` |
| fd 很大时崩溃 | 未检查 `fd >= MAX_CONNS` | 超界连接直接 `close` |

> 老内核（≤ 5.11）上，io_uring 的内存计入 `RLIMIT_MEMLOCK`。如果
> `io_uring_queue_init` 返回 `-ENOMEM`，可以 `ulimit -l unlimited` 或减小 ring 深度。

---

## 8. 下一步：multishot + provided buffer ring

当前版本是单次 shot，适合入门。生产级 io_uring 网络服务器通常会用：

| 特性 | 内核版本 | liburing API |
|------|---------|--------------|
| multishot accept | 5.19 | `io_uring_prep_multishot_accept()` |
| multishot recv | 6.0 | `io_uring_prep_recv_multishot()` |
| provided buffer ring | 5.19 | `io_uring_setup_buf_ring()` / `io_uring_buf_ring_add()` |
| `DEFER_TASKRUN` | 6.1 | `io_uring_queue_init_params()` + `IORING_SETUP_DEFER_TASKRUN` |
| `SINGLE_ISSUER` | 6.0 | 同上 |

官方已经提供了一个完整示例：

- [`liburing-master/examples/echo-server.c`](liburing-master/examples/echo-server.c)

它演示了：

- `io_uring_prep_multishot_accept()`：一次提交，持续产生 accept CQE
- `io_uring_prep_recv_multishot()` + `IOSQE_BUFFER_SELECT`：内核自动从
  provided buffer ring 取 buffer
- send 完成后调用 `io_uring_buf_ring_add()` 把 buffer 还给内核
- `user_data` 编码 `type(8) + buffer id(16) + fd(16)`
- `IORING_SETUP_SUBMIT_ALL | CQSIZE | SINGLE_ISSUER | DEFER_TASKRUN`，
  旧内核自动回退到 `COOP_TASKRUN`

可以先把官方示例编译出来验证环境：

```bash
(cd liburing-master && ./configure)
(cd liburing-master && make -C src -j"$(nproc)")
(cd liburing-master && make -C examples echo-server)
./liburing-master/examples/echo-server 8000
# 另一个终端：nc 127.0.0.1 8000
```

官方示例要求内核 ≥ 6.0（multishot recv + buffer ring）。

---

## 9. 参考

- liburing 仓库说明：[`liburing-master/README`](liburing-master/README)
- 官方 echo server 示例：[`liburing-master/examples/echo-server.c`](liburing-master/examples/echo-server.c)
- API 手册：`liburing-master/man/io_uring_prep_accept.3`、
  `io_uring_prep_recv.3`、`io_uring_prep_send.3`、`io_uring_setup_buf_ring.3`
- 同一问题的 epoll 实现：[`../multi_io/`](../multi_io/)、[`../tcp_p2p/`](../tcp_p2p/)
- io_uring 设计文档：<https://kernel.dk/io_uring.pdf>
