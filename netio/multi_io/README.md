# 网络 IO 与 IO 多路复用：select / poll / epoll

这份文档配套同目录下的 `netio` 代码，讲清楚三件事：

1. 网络 IO 在操作系统层面到底发生了什么
2. select / poll / epoll 各自的运作方式、优缺点、适用场景
3. 代码是怎么把这些机制封装成一个可用的类的

---

## 目录

- [一、先看清楚：一次网络读取到底发生了什么](#一先看清楚一次网络读取到底发生了什么)
- [二、为什么要有多路复用](#二为什么要有多路复用)
- [三、select](#三select)
- [四、poll](#四poll)
- [五、epoll](#五epoll)
- [六、三者横向对比](#六三者横向对比)
- [七、代码结构导读](#七代码结构导读)
- [八、编译与运行](#八编译与运行)
- [九、实测数据](#九实测数据)
- [十、真实踩到的坑](#十真实踩到的坑)
- [十一、工程实践清单](#十一工程实践清单)
- [十二、进阶：从 TCP 字节流到 HTTP 服务器](#十二进阶从-tcp-字节流到-http-服务器)

---

## 一、先看清楚：一次网络读取到底发生了什么

### 1.1 数据的完整路径

假设客户端发来一段数据，服务端调用 `recv()` 取走它，中间经历了这些步骤：

```
   客户端
     │  ① 用户态 write() → 数据进入本机内核发送缓冲区
     ▼
 ┌─────────────────────────────────────────────────────────────┐
 │  客户端内核                                                   │
 │   ② TCP 分段 → IP 封装 → 网卡驱动 → 网卡硬件发出              │
 └─────────────────────────────────────────────────────────────┘
     │  ③ 经过物理链路 / 中间路由器
     ▼
 ┌─────────────────────────────────────────────────────────────┐
 │  服务端内核                                                   │
 │   ④ 网卡收到数据 → DMA 写入内核内存 → 硬件中断                │
 │   ⑤ 中断处理程序 → 内核协议栈 tcp_v4_rcv()                    │
 │   ⑥ 校验、排序、重组 → 数据挂到该 socket 的接收队列           │
 │   ⑦ 唤醒等待在这个 socket 上的进程/线程（关键！）             │
 │   ⑧ 用户态 recv() → 把数据从内核缓冲区【拷贝】到用户缓冲区    │
 └─────────────────────────────────────────────────────────────┘
     │
     ▼
   服务端用户态代码拿到数据
```

这里有两次数据拷贝：④ 网卡 → 内核内存（DMA，不需要 CPU 参与），⑧ 内核缓冲区 → 用户缓冲区（**这一步要 CPU 参与，是真正昂贵的**）。

### 1.2 两个阶段

对一个 socket 的读操作，可以拆成两个阶段：

| 阶段 | 做什么 | 谁在等 |
|------|--------|--------|
| **第一阶段：等待数据就绪** | 数据从网络到达，进入内核接收缓冲区 | 通常要等很久（网络延迟） |
| **第二阶段：数据拷贝** | 从内核缓冲区拷贝到用户缓冲区 | 很快，但必然发生 |

**所有 IO 模型的区别，本质上只是这两个阶段怎么处理。** 记住这句话，后面所有内容都只是它的展开。

### 1.3 五种 IO 模型

这是《UNIX 网络编程》第 6 章的分类，至今仍是标准答案。

```
【阻塞 IO】
  用户线程 ──recv()──► 等待数据 ═══════════► 数据到达 ──拷贝──► 返回
                       (阻塞)                  (阻塞)

【非阻塞 IO】
  用户线程 ──recv()──► 没有?立刻返回 EWOULDBLOCK
           ──recv()──► 没有?立刻返回
           ──recv()──► 没有?立刻返回      ← 轮询，疯狂浪费 CPU
           ──recv()──► 有数据 ──拷贝──► 返回

【IO 多路复用】
  用户线程 ──select/poll/epoll──► 等待【多个】fd 中的任意一个就绪
                                   ══════════► 有 fd 就绪，返回
           ──recv()──► 数据已就绪 ──拷贝──► 返回

【信号驱动 IO】
  用户线程 ──注册 SIGIO 处理函数──► 立刻返回，继续干别的
                                   ↓ 数据到达时内核发信号
           ◄──────────────────────┘
           ──recv()──► 数据已就绪 ──拷贝──► 返回

【异步 IO (AIO / IOCP / io_uring)】
  用户线程 ──aio_read()──► 立刻返回，继续干别的
                           ↓ 内核【完全自己完成】等待 + 拷贝
           ◄──────────────┘ 内核通知：数据已经躺在你的缓冲区里了
```

### 1.4 同步 vs 异步的分界线

一个常见的误解是"非阻塞 IO 就是异步 IO"。**不是的。**

> **POSIX 的定义**：如果一个 IO 操作导致调用线程被阻塞，直到 IO 完成，那它就是**同步**的。

按这个定义：

| 模型 | 第一阶段（等待） | 第二阶段（拷贝） | 分类 |
|------|-----------------|-----------------|------|
| 阻塞 IO | 阻塞 | 阻塞 | 同步 |
| 非阻塞 IO | 不阻塞（轮询） | 阻塞 | 同步 |
| IO 多路复用 | 阻塞在 select 上 | 阻塞 | 同步 |
| 信号驱动 IO | 不阻塞 | 阻塞 | 同步 |
| 异步 IO | 不阻塞 | **不阻塞** | **异步** |

**前四种在第二阶段都要自己调用 recv()，都会阻塞在拷贝上，所以全都是同步 IO。**

只有异步 IO 把"等待"和"拷贝"两件事都交给了内核，用户代码只在最后收到"已经好了"的通知。

> 所以在 Linux 上，`epoll` 再快也是同步 IO。真正的异步 IO 是：
> - Windows 的 **IOCP**（成熟，工业级）
> - Linux 的 **AIO**（`io_submit`，只对直接 IO 有效，很局限）
> - Linux 的 **io_uring**（5.1+，共享内存环形队列，才是 Linux 上真正的异步 IO 未来）
>
> 但注意：**io_uring 需要的编程模型和 epoll 完全不同**，而且 epoll + 非阻塞 IO 在绝大多数场景下已经足够快，所以 epoll 仍然是当今 Linux 服务端的主流。

---

## 二、为什么要有多路复用

### 2.1 一连接一线程的代价

最直觉的服务器写法：accept 到一个连接就开一个线程。

```c
while (1) {
    int fd = accept(listen_fd, ...);
    std::thread(handle_client, fd).detach();   // 每连接一个线程
}
```

连接数一上来就崩，原因不在 CPU，而在**资源与调度**：

- **内存**：每个线程默认栈 8 MB（Linux）/ 1 MB（Windows）。10000 连接 × 8 MB = **80 GB 虚拟内存**（虽然可以调小栈，但仍有内核数据结构开销）
- **上下文切换**：1 万个线程轮流上 CPU，每次切换都要保存/恢复寄存器、刷 TLB、污染 CPU 缓存。大量时间花在切换本身而不是干活
- **绝大多数线程都在睡觉**：一个连接大部分时间是空闲的，线程却占着栈和调度器槽位

### 2.2 C10K 问题

1999 年 Dan Kegel 提出的问题：**如何让一台服务器同时处理 10000 个连接？**

答案的核心就是 IO 多路复用：**不要为每个连接开一个执行单元，而是用一个执行单元管理所有连接。**

```
【一连接一线程】                【多路复用 / Reactor】
 线程1 ─► 连接1 (大部分时间在睡)     ┌──────────────────┐
 线程2 ─► 连接2 (大部分时间在睡)     │   一个事件循环    │
 线程3 ─► 连接3 (大部分时间在睡)     │  epoll_wait()    │
 ...                                │       ↓          │
 线程N ─► 连接N (大部分时间在睡)     │  有事件才处理    │
                                    └──────────────────┘
 N 个线程 × 8MB 栈                       1 个线程 × 8MB 栈
 N 份调度开销                            1 份调度开销
```

关键洞察：**连接空闲时不该占用任何执行单元**。只有当它真的有数据要处理时，才临时借用一下事件循环。

### 2.3 Reactor 模式

```
                    ┌───────────────────────────────────────┐
                    │            Reactor 事件循环            │
                    │                                       │
   注册 fd ─────────►│  epoll_wait()  ◄── 阻塞在这里等待      │
   关注事件          │       │                               │
                    │       │ 返回就绪事件列表               │
                    │       ▼                               │
                    │   ┌─────────┐  ┌─────────┐            │
                    │   │ 可读    │  │ 可写    │  ...       │
                    │   └────┬────┘  └────┬────┘            │
                    │        ▼            ▼                 │
                    │   recv + 回调   send + 回调            │
                    └───────────────────────────────────────┘
                               │
                               ▼
                     业务回调：onMessage(fd, data, len)
```

这就是 **Reactor** 模式：**事件循环负责"等"，回调函数负责"干"**。

常见的几种变体：

| 变体 | 结构 | 典型代表 |
|------|------|---------|
| 单 Reactor 单线程 | 一个线程 accept + 读写 + 业务 | Redis（业务本身极快） |
| 单 Reactor 多线程 | 一个线程做 IO，业务丢线程池 | 早期 Netty、部分 Java 框架 |
| 主从 Reactor 多线程 | mainReactor 只 accept，subReactor 各自管理一批连接 | **muduo**、Netty 默认 |
| Proactor | 内核完成 IO 后通知 | Windows IOCP、`io_uring` |

本项目实现的是最简单的**单 Reactor 单线程**版本，这是理解其余变体的基础。

---

## 三、select

### 3.1 接口

```c
#include <sys/select.h>

int select(int nfds,
           fd_set *readfds,      // 关注「可读」的 fd 集合
           fd_set *writefds,     // 关注「可写」的 fd 集合
           fd_set *exceptfds,    // 关注「异常」的 fd 集合
           struct timeval *timeout);   // 超时，NULL 表示永久阻塞
```

返回值：
- `> 0`：就绪的 fd 总数（**注意：是几个集合加起来的总数**）
- `= 0`：超时
- `< 0`：出错（`EINTR` 表示被信号打断）

### 3.2 fd_set 是什么

`fd_set` 本质是一个**位图**：第 N 位为 1，表示"我关注 fd N"。

```c
typedef struct {
    long fds_bits[FD_SETSIZE / (8 * sizeof(long))];
} fd_set;
```

操作宏：

```c
FD_ZERO(&set);      // 全部清零
FD_SET(fd, &set);   // 把第 fd 位置 1
FD_CLR(fd, &set);   // 清零
FD_ISSET(fd, &set); // 测试第 fd 位
```

三个致命特性：

1. **容量固定**：`FD_SETSIZE` 通常是 **1024**，是编译期常量。fd 一旦 `>= 1024`，`FD_SET` 会**越界写内存**——不报错，只会在别处崩溃，极难排查
2. **值传递**：每次调用都要把整个位图拷进内核
3. **破坏性修改**：返回后位图被改成"只有就绪的 fd 置位"，原来的关注信息**没了**

### 3.3 一次 select 调用完整流程

```
用户态                                    内核态
────────                                  ────────
① 清空三个 fd_set
② 遍历所有 fd，FD_SET 进去
   （O(n) 装配）
                                          ③ 把三个位图 copy_from_user 进来
                                             【O(n) 拷贝】
                                          ④ 遍历位图里的每个 fd：
                                             调 vfs_poll(fd) 问"就绪了吗"
                                             【O(n) 轮询】
                                          ⑤ 没就绪 → 挂到各 fd 的等待队列
                                             → 睡眠
                                             ...
                                             某个 socket 有数据了，
                                             内核唤醒 select
                                             → 重新遍历所有 fd（可能被误唤醒）
                                          ⑥ 把就绪的 fd 写回位图
                                             copy_to_user 到用户态
                                          【O(n) 拷贝】
⑦ 遍历所有 fd，FD_ISSET 检查
   （O(n) 扫描）
⑧ 对就绪的 fd 调 recv/send
```

**一次调用 = 三次 O(n) 遍历 + 两次 O(n) 拷贝。**

### 3.4 五个固有问题

| # | 问题 | 后果 |
|---|------|------|
| 1 | `FD_SETSIZE` 硬上限 1024 | 无法支撑高并发，超了会越界写内存 |
| 2 | 每次调用都要重新装配 fd_set | 代码啰嗦，且是 O(n) 操作 |
| 3 | fd_set 每次全量拷贝进内核 | O(n) 内存拷贝，fd 越多越慢 |
| 4 | 内核要遍历所有 fd 询问就绪状态 | O(n)，且与实际活跃数无关 |
| 5 | 返回后还要遍历所有 fd 找就绪的 | O(n) 再次扫描 |

**核心问题在于：这四个 O(n) 的成本，只跟"注册了多少 fd"有关，跟"有多少 fd 真的活跃"无关。**

1 万个连接里只有 3 个活跃，select 依然要白白折腾 1 万次。

还有一个常被忽略的坑：

```c
struct timeval tv = {5, 0};
select(nfds, &r, &w, &e, &tv);   // 第一次：等 5 秒
select(nfds, &r, &w, &e, &tv);   // 第二次：不是 5 秒了！
```

**Linux 上 select 会把 `timeout` 改写成"剩余时间"**，所以每次调用前必须重新赋值 `tv`。复用同一个结构体是经典 bug。

### 3.5 优点

说了这么多缺点，select 仍然有两个不可替代的优势：

- **跨平台**：Windows / Linux / macOS / 各种 Unix 全都有。**Windows 上 `select` 是唯一可用且没有严重缺陷的多路复用方案**
- **超时精度是微秒**（`timeval`），poll / epoll 都只有毫秒

所以：**连接数少、又要求跨平台时，select 是合理选择**，并不是"过时的垃圾"。

---

## 四、poll

### 4.1 接口

```c
#include <poll.h>

struct pollfd {
    int   fd;       // 要关注的 fd
    short events;   // 【输入】我关注什么事件
    short revents;  // 【输出】实际发生了什么事件
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout);   // timeout 单位：毫秒
```

可用事件（部分）：

| 事件 | 含义 |
|------|------|
| `POLLIN` | 有数据可读 |
| `POLLOUT` | 可以写 |
| `POLLPRI` | 有紧急数据（TCP 带外数据） |
| `POLLERR` | 出错（**只能输出，注册时无效**） |
| `POLLHUP` | 挂断（**只能输出**） |
| `POLLNVAL` | fd 非法，比如已经 close 过（**只能输出**） |

### 4.2 相对 select 的改进

| 改进点 | 说明 |
|--------|------|
| **没有 1024 限制** | 用数组代替位图，上限变成 `RLIMIT_NOFILE`（可调）和内存 |
| **关注集合可复用** | `events` 是输入、`revents` 是输出，内核只改 `revents`，所以**不必每次重建** |
| **不需要 nfds 参数** | 不用维护"最大 fd + 1" |
| **超时是毫秒且不会被改写** | 不像 select 那样有 `timeval` 被修改的陷阱 |
| **事件更精细** | 能区分 `POLLPRI` / `POLLNVAL` 等具体情况 |

最关键的是第 2 条。看代码对比：

```c
// select：每次都要重建
FD_ZERO(&rset);
for (i = 0; i < n; i++) FD_SET(fds[i], &rset);
select(maxfd+1, &rset, NULL, NULL, &tv);

// poll：fds 数组填一次就行，之后反复用
poll(fds, n, timeout);
```

### 4.3 仍然没解决的问题

poll 只解决了"接口好用"的问题，**没有解决性能问题**：

- ❌ 数组依然要整体拷进内核 —— **O(n) 拷贝**
- ❌ 内核依然要遍历所有 fd —— **O(n) 轮询**
- ❌ 返回后依然要遍历数组找就绪项 —— **O(n) 扫描**
- ❌ Windows 上**没有 poll**（有个 `WSAPoll`，但存在"连接失败不上报"的缺陷，微软自己都不推荐）

根本原因没变：**每次调用都要把完整的 fd 清单交给内核重新检查一遍**。

---

## 五、epoll

### 5.1 三个接口

```c
#include <sys/epoll.h>

// 1. 创建一个 epoll 实例（返回一个 fd）
int epfd = epoll_create1(EPOLL_CLOEXEC);

// 2. 增删改要关注的 fd
struct epoll_event ev;
ev.events   = EPOLLIN | EPOLLET;
ev.data.fd  = fd;              // 用户数据，内核原样保存、原样返回
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);   // 添加
epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);   // 修改关注的事件
epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);  // 删除

// 3. 等待事件
struct epoll_event events[MAX];
int n = epoll_wait(epfd, events, MAX, timeout_ms);
```

**注意这三步的分工**：`epoll_ctl` 是"注册"，只在 fd 的生命周期里调用一两次；`epoll_wait` 是"等待"，在事件循环里反复调用。

**这个"注册与等待分离"就是 epoll 一切优势的来源。**

`data` 字段是 epoll 最巧妙的设计之一：它是一个 64 位联合体，可以存 fd，也可以存指针。存指针时，事件返回的瞬间你就直接拿到了连接对象，省掉一次哈希查表。

### 5.2 内核数据结构

`epoll_create1()` 创建了一个 `struct eventpoll`：

```c
struct eventpoll {
    struct rb_root_cached rbr;      // 红黑树：所有【已注册】的 fd
    struct list_head      rdllist;  // 链表：所有【已就绪】的 fd
    wait_queue_head_t     wq;       // 等待队列：阻塞在 epoll_wait 上的线程
    wait_queue_head_t     poll_wait;
    ...
};
```

每个注册进来的 fd 对应一个 `struct epitem`：

```c
struct epitem {
    struct rb_node       rbn;        // 红黑树节点
    struct list_head     rdllink;    // 就绪链表节点
    struct epoll_filefd  ffd;        // 关联的 fd + file
    struct epoll_event   event;      // 用户注册的事件和 data
    struct list_head     pwqlist;    // 挂到目标 socket 等待队列上的"挂钩"列表
    ...
};
```

```
                  epoll 实例 (eventpoll)
        ┌──────────────────────────────────────┐
        │                                      │
        │   红黑树 rbr              就绪链表     │
        │   （注册的 fd）           rdllist      │
        │                                      │
        │      ●                    ┌───┐       │
        │     / \                   │ ● │──┐    │
        │    ●   ●                  └───┘  │    │
        │   / \   \                 ┌───┐  │    │
        │  ●   ●   ●                │ ● │◄─┘    │
        │                           └───┘       │
        │                                      │
        │   等待队列 wq                         │
        │   （阻塞在 epoll_wait 的线程）         │
        └──────────────────────────────────────┘
                     ▲
                     │ eppoll_entry 挂钩
                     │
        ┌────────────┴─────────────────────────┐
        │   socket 的等待队列 sk_wq             │
        │   [ tcp_sock 结构 ]                   │
        └──────────────────────────────────────┘
```

### 5.3 事件是怎么被"推"上来的（关键）

**这是理解 epoll 的核心。** 注意：整个过程里，**epoll 从来没有主动去问过 socket "你有数据了吗"**。

```
① 网卡收到数据
        ↓
② 内核协议栈 tcp_v4_rcv() → tcp_data_queue()
   把数据放进 socket 的接收队列
        ↓
③ socket 调用 sk_data_ready()，即 sock_def_readable()
        ↓
④ wake_up_interruptible_sync_poll(&sk->sk_wq, EPOLLIN | ...)
   遍历这个 socket 等待队列上的【所有】等待项
        ↓
⑤ 其中一项就是 epoll 注册时挂上来的 eppoll_entry
   → 调用它的回调 ep_poll_callback()
        ↓
⑥ ep_poll_callback():
     - 通过 epitem 找到所属的 eventpoll
     - 如果这个 epitem 还不在 rdllist 上，就 list_add_tail 挂进去
     - wake_up(&ep->wq)  唤醒阻塞在 epoll_wait 的线程
        ↓
⑦ epoll_wait() 返回
```

**所以是数据到达时内核"主动"把 fd 挂到就绪链表上，而不是 epoll_wait 去轮询发现。** 这就是"事件驱动"和"轮询"的本质区别。

### 5.4 为什么高效：三个问题的答案

对照 select/poll 的三大问题，看 epoll 是怎么解决的：

| select / poll 的问题 | epoll 的解法 |
|---------------------|-------------|
| 每次调用要传完整 fd 清单 → **O(n) 拷贝** | fd 用 `epoll_ctl` 注册一次就**常驻内核红黑树**，`epoll_wait` 不传任何清单 → **拷贝消失** |
| 内核要遍历所有 fd → **O(n) 轮询** | 内核通过回调**主动**把就绪 fd 挂到 `rdllist`，`epoll_wait` 只需检查链表是否为空 → **O(1) 判断** |
| 返回后要遍历所有 fd 找就绪的 → **O(n) 扫描** | `epoll_wait` 直接把 `rdllist` 里的就绪项拷回用户态 → **O(k)，k = 就绪数** |

**复杂度从 O(n) 降到了 O(k)**，n 是注册的 fd 总数，k 是本次就绪的 fd 数。在"1 万个连接只有 3 个活跃"的场景下，这是数量级的差距。

> **两个必须澄清的常见误解：**
>
> **误解一："epoll 是 O(1)"**
> 不准确。准确说是 **O(就绪数)**。如果 1 万个连接全部活跃，epoll_wait 每次都要拷 1 万个事件回用户态，此时它和 poll 的差距会大幅缩小，甚至可能更慢（多了一层链表操作）。**epoll 的优势建立在"活跃连接只占少数"这个前提上。**
>
> **误解二："epoll 用 mmap 共享内存，所以零拷贝"**
> 这是 2.4 时代旧设计文档的说法，被中文博客大量抄袭。**现代 Linux 的 epoll 并没有 mmap**，`epoll_wait` 是通过 `__put_user` 把就绪事件拷回用户态的。
> 所谓"零拷贝"是相对于"每次把所有 fd 都拷进内核"而言的**注册信息不再重复拷贝**，而不是真的没有数据拷贝。

### 5.5 LT 与 ET

这是 epoll 最容易用错的地方。

#### 水平触发（LT，Level Triggered）—— 默认模式

> **只要你关心的条件还成立，就反复通知你。**

```
内核接收缓冲区: [有数据][有数据][有数据][空]
                   │       │       │
epoll_wait 返回:   ✓       ✓       ✓      ✗
                （每次调用都返回，直到数据被读完）
```

- 行为上**和 poll 一样**
- 可以只读一部分，剩下的下次再读
- 编程简单，不容易出错
- **默认模式**，不加 `EPOLLET` 就是 LT

#### 边缘触发（ET，Edge Triggered）—— `EPOLLET`

> **只在状态发生变化的那一刻通知一次。**

```
内核接收缓冲区: [空]→[有数据][有数据][有数据][空]→[有数据]
                    │                       │
epoll_wait 返回:    ✓(边沿)                 ✓(边沿)
                    ↑ 只通知这一次            ↑ 空了之后再来才会再通知
```

- 只有"从无到有"的那一瞬间通知
- **必须一次把数据读干净**（循环 read 到 `EAGAIN`），否则剩下的数据**永远不会再收到通知**，连接就"卡死"了
- **必须配合非阻塞 fd**，理由见下
- 系统调用次数更少，高并发下性能更好

#### ET 模式的三条铁律

```c
// 1. 必须设置非阻塞
setNonblocking(fd);

// 2. 必须循环读，直到 EAGAIN
while (1) {
    n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0)  { /* 处理数据 */ continue; }
    if (n == 0) { /* 对端关闭 */ break; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // ← 必须走到这里才算读完
    if (errno == EINTR) continue;
    /* 真错误 */
    break;
}

// 3. accept 也必须循环到 EAGAIN
while (1) {
    int c = accept(listen_fd, ...);
    if (c < 0) {
        if (errno == EAGAIN) break;   // ← 同样必须走完
        ...
        continue;
    }
    /* 处理新连接 */
}
```

**为什么 ET 必须非阻塞？**

因为 ET 只告诉你"有数据了"，**不告诉你"还有多少"**。你的循环必须靠"读不到为止"来退出。但如果 fd 是**阻塞**的，最后一次 `recv` 在数据读完后**不会返回 EAGAIN，而是永久阻塞住**——整个事件循环（在这个模型里就是整个服务器）就这么卡死了。

**为什么 ET 的 accept 也要循环？**

因为一次 ET 通知可能对应**多个**积压的连接（内核的 accept 队列里有 5 个连接，但只触发一次边沿）。你只 accept 一个就走，剩下 4 个在新连接到来之前**再也不会被通知**。

#### 怎么选？

| 场景 | 建议 |
|------|------|
| 初次使用 / 追求正确性 | **LT**。默认、简单、不会因漏读而卡死 |
| 追求极致性能、团队有经验 | ET |
| 连接数不多、数据量小 | LT（差距很小） |
| 长连接海量并发、短报文为主 | ET（减少 epoll_wait 返回次数和系统调用） |

> 本项目的 `TcpServer` **在两种模式下都按 ET 语义实现**（统一循环读到 `EAGAIN`）。这样同一份代码在 LT 和 ET 下都正确，也是工程中的通用做法。

### 5.6 EPOLLONESHOT 与 EPOLLEXCLUSIVE

#### EPOLLONESHOT

注册后，事件**只通知一次**，之后这个 fd 就"哑"了，必须用 `EPOLL_CTL_MOD` 重新激活。

**为什么需要它？** 在多线程 Reactor 里：

```
线程A: epoll_wait 返回 fd=5 可读 ──► 开始处理（读数据、解析协议）
线程B: epoll_wait 也返回 fd=5 可读 ──► 也开始处理    ← 灾难！
```

同一个 fd 被两个线程同时读写，数据错乱、状态机崩溃。`EPOLLONESHOT` 保证一个 fd 的事件在任意时刻**只会被一个线程拿到**。处理完后用 `EPOLL_CTL_MOD` 重新注册，交还给 epoll。

本项目的 `EpollPoller::rearm()` 就是为此准备的。

#### EPOLLEXCLUSIVE（Linux 4.5+）

解决**惊群（thundering herd）**问题。

多个进程/线程各自有独立的 epoll 实例，都监听着**同一个** listen fd。新连接到来时，内核会把**所有** epoll 实例都唤醒，但最终只有一个能 accept 成功，其他的白跑一趟——连接越密集，浪费越严重。

`EPOLLEXCLUSIVE` 让内核**只唤醒其中一个**。

> 补充：早期 Linux 的 `accept` 惊群在内核 2.6 之后已经通过 `WQ_FLAG_EXCLUSIVE` 基本解决了（只唤醒一个等待者）。`EPOLLEXCLUSIVE` 针对的是"多个 epoll 实例注册同一 fd"这个更具体的场景。
> Nginx 用的 `accept_mutex` 是用户态的另一种解法，比内核方案更早。

### 5.7 代价与限制

epoll 不是免费的午餐：

- **仅 Linux**。Windows 用 IOCP，macOS/BSD 用 kqueue，各写各的
- **编程复杂度高**。ET 模式的三个铁律、`EPOLLONESHOT` 的重注册、回调中的对象生命周期，都是容易出错的地方
- **优势有前提**。只有"活跃连接远少于总连接"时才有压倒性优势
- **`epoll_ctl` 是有开销的**。频繁增删 fd（比如短连接服务器）会有锁竞争，此时未必比 poll 快

---

## 六、三者横向对比

| 维度 | select | poll | epoll |
|------|--------|------|-------|
| **fd 上限** | `FD_SETSIZE`（通常 **1024**），超了越界写内存 | `RLIMIT_NOFILE`，可调 | `RLIMIT_NOFILE`，可调 |
| **数据结构** | 位图 `fd_set` | 数组 `pollfd[]` | 内核红黑树 + 就绪链表 |
| **关注集合** | 每次调用都要重建 | 填一次可复用 | `epoll_ctl` 注册一次，常驻内核 |
| **内核拷贝** | 每次全量 O(n) | 每次全量 O(n) | 注册时一次；wait 只拷就绪的 O(k) |
| **内核发现就绪** | 遍历全部 fd，O(n) | 遍历全部 fd，O(n) | 回调主动上报，O(1) 判断 |
| **用户态找就绪** | 遍历全部 fd，O(n) | 遍历全部 fd，O(n) | 只遍历返回的 k 个，O(k) |
| **时间复杂度** | **O(n)** | **O(n)** | **O(k)**，k=就绪数 |
| **触发模式** | 只有 LT | 只有 LT | **LT + ET** |
| **超时精度** | 微秒 | 毫秒 | 毫秒 |
| **timeout 是否被改写** | **会**（必须每次重设） | 不会 | 不会 |
| **返回的事件** | 只有总数，要自己找 | 要自己遍历找 | 直接给出就绪列表 + 用户数据 |
| **跨平台** | **Windows/Linux/macOS 都有** | 仅 Unix（**Windows 无**） | **仅 Linux** |
| **编程复杂度** | 低 | 低 | 中～高（ET 尤其） |
| **适合的场景** | 连接少、要跨平台 | 连接中等、Unix | **海量连接、活跃比例低** |

**一句话总结选型：**

```
连接数 < 1000 且要跨平台   →  select（或随便，差别不大）
Unix 环境、连接数千         →  poll 够用
Linux、海量连接、追求性能   →  epoll（ET 更好，但要小心）
Windows 高并发             →  别用 select，上 IOCP
```

---

## 七、代码结构导读

### 7.1 分层

```
┌───────────────────────────────────────────────────────────┐
│  examples/echo_server.cpp      业务代码（只有几个回调）      │
├───────────────────────────────────────────────────────────┤
│  net/http/HttpServer           HTTP 层：字节流 → 请求/响应   │
│    · 解析状态机（HttpContext）· 路由 · 静态文件 · keep-alive │
│    → 详见第十二章                                          │
├───────────────────────────────────────────────────────────┤
│  net/TcpServer.h               Reactor：事件 → 业务语义     │
│    · accept / recv / send 的循环处理                       │
│    · 应用层发送缓冲区                                      │
│    · 连接生命周期管理                                      │
├───────────────────────────────────────────────────────────┤
│  net/EventPoller.h             多路复用抽象接口（+ 工厂）    │
│    ┌────────────┬────────────┬────────────┐               │
│    │SelectPoller│ PollPoller │EpollPoller │               │
│    └────────────┴────────────┴────────────┘               │
├───────────────────────────────────────────────────────────┤
│  net/SocketUtils.h             跨平台 socket 基础           │
│    · socket_t 类型统一 · 错误码统一 · 非阻塞设置            │
└───────────────────────────────────────────────────────────┘
```

> `net/http` 那一层是可选的：它只依赖 `TcpServer`，不依赖任何多路复用细节。
> 它要解决的是「字节流没有消息边界」这个问题，详见第十二章。

分层的意义：`EventPoller` 之上的代码**完全不知道**底层用的是哪种机制。换后端只需要改一个枚举值：

```cpp
net::TcpServer::Options opts;
opts.backend = net::EventPoller::Backend::Epoll;   // 换成 Select / Poll 只改这里
opts.edgeTriggered = true;                          // 仅 epoll 生效
net::TcpServer server(opts);
```

### 7.2 EventPoller 接口设计

```cpp
class EventPoller {
public:
    enum class Backend { Auto, Select, Poll, Epoll };

    static std::unique_ptr<EventPoller> create(Backend b = Backend::Auto,
                                               int maxEvents = 1024,
                                               bool edgeTriggered = false);

    virtual bool add(socket_t fd, uint32_t events) = 0;   // 注册
    virtual bool mod(socket_t fd, uint32_t events) = 0;   // 改关注
    virtual bool del(socket_t fd) = 0;                    // 注销
    virtual int  wait(std::vector<Event>& ready, int timeoutMs) = 0;  // 等待

    virtual const char* name() const = 0;
    virtual size_t size() const = 0;
    const std::string& lastError() const;
};
```

事件用**位掩码**表示，一个 fd 可以同时关注多种：

```cpp
namespace ev {
constexpr uint32_t Read  = 0x001;   // 可读
constexpr uint32_t Write = 0x002;   // 可写
constexpr uint32_t Error = 0x004;   // 错误
constexpr uint32_t Hup   = 0x008;   // 挂断
constexpr uint32_t Rdhup = 0x010;   // 对端半关闭（仅 epoll 上报）
}
```

这样 `add(fd, ev::Read | ev::Write)` 就能一次关注两种事件。

### 7.3 各后端的实现要点

**SelectPoller** —— 三个集合作为成员复用，每次 `wait` 重建：

```cpp
int SelectPoller::wait(std::vector<Event>& ready, int timeoutMs) {
    FD_ZERO(&rset_); FD_ZERO(&wset_); FD_ZERO(&eset_);
    for (const auto& kv : interest_) {          // ① O(n) 装配
        if (kv.second & ev::Read)  FD_SET(kv.first, &rset_);
        if (kv.second & ev::Write) FD_SET(kv.first, &wset_);
        FD_SET(kv.first, &eset_);
    }
    timeval tv{};                                // ② 每次重建，因为它会被改写
    ...
    int n = ::select((int)maxFd_ + 1, &rset_, &wset_, &eset_, ptv);   // ③ O(n) 内核轮询
    ...
    for (const auto& kv : interest_) {           // ④ O(n) 扫描找就绪的
        if (FD_ISSET(kv.first, &rset_)) ready.push_back({kv.first, ev::Read});
        ...
    }
}
```

`add()` 里有一个**必须在 Linux 上做的检查**：

```cpp
if (fd >= FD_SETSIZE) {
    lastError_ = "fd 超出 FD_SETSIZE，select 无法管理";
    return false;      // 不检查的话，FD_SET 会越界写内存！
}
```

**PollPoller** —— 关注集合可复用，但数组仍要重建：

```cpp
int PollPoller::wait(std::vector<Event>& ready, int timeoutMs) {
    pfds_.clear();
    for (const auto& kv : interest_) {
        pollfd p{};
        p.fd = kv.first;
        p.events = toPollEvents(kv.second);
        p.revents = 0;                     // 必须清零，否则残留会被误判
        pfds_.push_back(p);
    }
    int n = ::poll(pfds_.data(), (nfds_t)pfds_.size(), timeoutMs);
    ...
    for (const pollfd& p : pfds_) {        // 仍然要整个扫一遍
        if (p.revents == 0) continue;
        ready.push_back(Event{p.fd, fromPollEvents(p.revents)});
    }
}
```

**EpollPoller** —— 注册一次，之后只收就绪的：

```cpp
bool EpollPoller::add(socket_t fd, uint32_t events) {
    epoll_event e{};
    e.events  = toEpoll(events);
    e.data.fd = fd;                                    // 用户数据随事件带回
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &e) < 0) return false;
    interest_[fd] = events;
    return true;
}

int EpollPoller::wait(std::vector<Event>& ready, int timeoutMs) {
    // 没有传任何 fd 清单！内核只返回就绪的那几个
    const int n = ::epoll_wait(epfd_, events_.data(), maxEvents_, timeoutMs);
    ...
    for (int i = 0; i < n; ++i) {                       // 只遍历 n 个
        ready.push_back(Event{events_[i].data.fd, fromEpoll(events_[i].events)});
    }
    return n;
}
```

### 7.4 TcpServer：Reactor 的落地

```
loopOnce()
   │
   ├─ poller_->wait(ready, timeout)      ← 阻塞在这里等事件
   │
   └─ for (每个就绪事件)
        ├─ e.fd == listenFd_  → handleAccept()   循环 accept 到 EAGAIN
        │                       └─ onConnect_(fd)
        │
        ├─ e.events & Error   → closeConnection()
        │
        ├─ e.events & Write   → handleWrite()    续发缓冲区里的数据
        │
        └─ e.events & Read    → handleRead()     循环 recv 到 EAGAIN
                                └─ onMessage_(fd, data, len)
```

`TcpServer` 里还有几个容易忽略但**必不可少**的细节：

**① `accept` 之后必须把新 fd 设为非阻塞**

```cpp
setNonBlocking(fd, true);   // ET 模式的硬性前提
```

**② 读取统一按 ET 语义写**

```cpp
for (;;) {
    const int n = ::recv(fd, buf.data(), (int)buf.size(), 0);
    if (n > 0) { onMessage_(fd, buf.data(), n); continue; }
    if (n == 0) { closeConnection(fd); return false; }     // 对端 FIN
    const int err = socketError();
    if (isInterrupted(err)) continue;
    if (isWouldBlock(err)) return true;                    // 读干净了
    closeConnection(fd); return false;
}
```

这样 LT / ET 下都正确。

**③ 回调里可能把连接关掉**

```cpp
if (onMessage_) onMessage_(fd, buf.data(), n);
// 回调里可能调用了 closeConnection，此处的 fd 已经失效，必须重新确认
if (conns_.find(fd) == conns_.end()) return false;
```

**④ `close` 和 `epoll_ctl(DEL)` 的顺序不能反**

```cpp
poller_->del(fd);      // 必须在前
closeSocket(fd);       // 必须在后
```

`close()` 之后 fd 号会被内核回收，可能**立刻**被新连接复用。如果复用器里还残留着旧的注册项，它就会指向一个完全不相干的连接——这种 bug 极其难查。

### 7.5 应用层发送缓冲区（为什么必须有）

这是**非阻塞网络编程的必修课**，也是新手最容易漏掉的地方。

```cpp
// 天真写法：以为 send 一定能全部发出去
send(fd, data, len);      // ❌ 可能只发出去了 100 字节中的 30 字节！
```

非阻塞 socket 上，`send` 的语义是：**能发多少发多少，发不完就返回已发送的字节数**。因为内核的发送缓冲区是有限的。

剩下的数据必须自己缓存，等可写事件再发：

```cpp
bool TcpServer::send(socket_t fd, const void* data, size_t len) {
    Connection& c = ...;

    // 【关键】如果之前还有没发完的数据，新数据必须排到它们后面，
    // 否则 TCP 字节流的顺序就乱了
    if (c.outSent < c.outBuf.size()) {
        appendToBuffer(fd, data, len);
        updateInterest(fd);
        return true;
    }

    // 缓冲区是空的，可以先尝试直接写出去
    size_t written = 0;
    while (written < len) { /* ::send ... */ }

    if (written < len) {                       // 没发完
        appendToBuffer(fd, (const char*)data + written, len - written);
        updateInterest(fd);                    // 开始关注可写事件
    }
    return true;
}
```

**还有一个必须做的开关操作：**

```cpp
void TcpServer::updateInterest(socket_t fd) {
    uint32_t events = ev::Read;
    if (c.outSent < c.outBuf.size()) {
        events |= ev::Write;      // 还有数据没发完 → 关注可写
    }
    poller_->mod(fd, events);     // 发完了 → 取消对可写的关注
}
```

**为什么必须取消？** 因为**可写事件几乎永远是就绪的**（内核发送缓冲区通常是空的）。如果你一直关注着可写事件，事件循环就会每轮都返回"可写"，然后你无事可做，接着又立刻返回……**CPU 直接 100% 空转**。

这是 epoll 编程中最常见的性能事故之一。

> 顺带一提：`outBuf` 会无限增长——如果对端一直不发数据、应用层又一直 `send`，迟早 OOM。真实项目必须做**高水位检查**：超过阈值就通知上层，由上层决定是丢弃还是断开连接。muduo 就是通过 `HighWaterMarkCallback` 做这件事的。

---

## 八、编译与运行

### Windows（MSVC）

需要 CMake + Visual Studio。首次使用先改 `build_msvc.bat` 里的 `VSROOT`：

```bat
set "VSROOT=D:\C++"
```

然后：

```bat
build_msvc.bat
```

产物在 `build\Release\`。

也可以手动来：

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Visual Studio 直接「打开文件夹」也支持 CMake 项目。

### Linux

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行

```bash
# 服务端（端口 后端 触发模式）
./echo_server 9000 epoll et
./echo_server 9000 poll  lt
./echo_server 9000 select lt     # Windows 上只能用这个

# 客户端（主机 端口 消息数 每条字节数）
./echo_client 127.0.0.1 9000 2000 4096

# HTTP 服务器（端口 后端 触发模式 静态目录）—— 详见第十二章
./http_server 8080 auto lt www
# 浏览器打开 http://127.0.0.1:8080/

# HTTP 测试客户端（主机 端口 大响应字节数）
./http_client 127.0.0.1 8080 8388608
```

`echo_server` 的输出会明确告诉你**实际生效的是哪个后端**，可以随时验证：

```
回显服务器已启动
  地址     : 0.0.0.0:9000
  复用后端 : epoll(ET)
  触发模式 : 边缘触发 ET
```

也可以直接用 `telnet 127.0.0.1 9000` 手打几行字看回显。

---

## 九、实测数据

在 Windows 11 + MSVC 19.44（`select` 后端，LT 模式，本机回环）上的实测：

| 场景 | 结果 |
|------|------|
| 2000 条 × 4 KiB | 全部正确，**360 MiB/s**，46074 次往返/秒，平均 RTT **21.7 µs** |
| 200 条 × 1 MiB | 全部正确，**851 MiB/s**（触发部分写，验证了发送缓冲区） |
| 5 个客户端并发 × 500 条 × 8 KiB | 全部正确 |

1 MiB 的测试很有意义：它必然触发 `send` 的部分写，能验证应用层发送缓冲区的正确性。如果缓冲区实现有 bug，客户端会因为字节流错乱而校验失败。

> **关于 poll / epoll 的实测**：这两个后端是 Linux 专有的，本次验证在 Windows 上进行，**未能实际运行验证**。代码逻辑是照着 Linux 的语义写的，但你如果在 Linux 上跑，建议先用 `./echo_server 9000 poll lt` 和 `./echo_server 9000 epoll et` 各压一轮确认。
>
> 尤其是 **ET 模式**——它在 Windows 上根本无法触发，是最需要单独验证的路径。

---

## 十、真实踩到的坑

这部分是写这份代码时**实际撞上**的问题，比教科书上的例子更有参考价值。

### 10.1 `accept` 的 `addr` 和 `addrlen` 必须成对

```cpp
// ❌ 错误写法
int addrLen = 0;
socket_t fd = ::accept(listenFd_, nullptr, &addrLen);
```

这行在 Linux 上可能侥幸能跑，在 **Windows 上直接返回 `WSAEFAULT (10014)`**——"尝试使用指针参数时的无效指针地址"。

规则是：**`addr` 和 `addrlen` 要么同时为 `NULL`，要么同时指向有效缓冲区**。传 `addr = NULL` 却给一个非空的 `addrlen` 是非法的。

```cpp
// ✅ 正确
socket_t fd = ::accept(listenFd_, nullptr, nullptr);
```

**更值得记的是它引发的连锁反应**：由于 accept 一直失败，而监听 fd 在 **LT 模式下持续可读**，事件循环陷入了疯狂空转——测试时 3 秒钟刷出了 **340 万行**错误日志，CPU 打满。

> 教训：**当事件源持续处于就绪状态、而处理逻辑又立刻失败返回时，LT 模式会形成 busy loop。** 类似的场景还有 `EMFILE`（fd 耗尽）。真实项目需要对这类"持续失败"做特殊处理（比如短暂停止监听），至少要限制日志频率。

### 10.2 `lastError` 的名字隐藏陷阱

`EventPoller` 有一个成员函数 `lastError()`。同时 `SocketUtils.h` 里有个全局函数 `net::lastError()` 返回错误码。

在派生类（如 `SelectPoller`）的成员函数里写：

```cpp
const int err = lastError();     // ❌ 编译错误
```

编译报的是让人摸不着头脑的：

```
error C2440: “初始化”: 无法从“const std::string”转换为“int”
```

原因：**C++ 的成员名会隐藏外层作用域的同名符号**（name hiding）。在成员函数体内，`lastError` 优先解析为 `EventPoller::lastError`（返回 `std::string`），而不是命名空间里的 `net::lastError`（返回 `int`）。

**解决**：把全局函数改名为 `socketError()`，从根上避免重名。

> 教训：给类成员函数起名时，想一想这个名字会不会和命名空间里的自由函数冲突。`lastError` / `error` / `size` / `data` 都是重灾区。

### 10.3 ET 模式必须配非阻塞

前面 [5.5 节](#55-lt-与-et) 详细讲过，这里再强调一次结论：

```
ET + 阻塞 fd = 最后一次 recv 永久阻塞 = 整个事件循环卡死
```

而且这个 bug 的表现是"服务器跑着跑着就不响应了"，不是崩溃，**非常难定位**。

### 10.4 可写事件的 busy loop

前面 [7.5 节](#75-应用层发送缓冲区为什么必须有) 讲过：**数据发完后必须立刻取消对可写事件的关注**，否则事件循环 100% 空转。

### 10.5 忘记处理 SIGPIPE

```cpp
::send(fd, data, len);   // 对端已经关闭 → Linux 默认发送 SIGPIPE → 进程直接被杀死
```

网络服务器"莫名退出"的头号原因。两种解法：

```cpp
signal(SIGPIPE, SIG_IGN);                              // 全局忽略
::send(fd, data, len, MSG_NOSIGNAL);                   // 只对这个调用生效（更精细）
```

本项目两个都用了（`ignoreSigPipe()` + `MSG_NOSIGNAL`）。

Windows **没有 SIGPIPE**，所以这个问题只在 Linux 上出现。

### 10.6 批处理文件里的中文

`.bat` 文件被 `cmd.exe` 按**本地代码页**（中文 Windows 上是 GBK）解析。如果你的 `.bat` 是 UTF-8 编码且含中文注释，会被解析成乱码并**破坏命令语法**，报出"''xxx' 不是内部或外部命令"这种莫名其妙的错误。

**结论：`.bat` / `.cmd` 文件只用 ASCII 字符。** 需要中文提示就加 `chcp 65001` 并确保文件本身编码一致，或者干脆不用中文。

---

## 十一、工程实践清单

如果你要在真实项目里用这套东西，下面这些是必须补上的：

### 必须做

- [ ] **非阻塞 + 循环读写到 EAGAIN**（无论 LT 还是 ET）
- [ ] **应用层发送缓冲区**，并做高水位检查
- [ ] **数据发完立刻取消可写关注**，避免 busy loop
- [ ] **`epoll_ctl(DEL)` 在 `close()` 之前**
- [ ] **忽略 SIGPIPE** 或使用 `MSG_NOSIGNAL`
- [ ] **`accept` 循环到 EAGAIN**
- [ ] **回调中关闭连接**时的对象生命周期管理（本项目用 `conns_.find(fd)` 重新确认；muduo 用 `shared_ptr` + 延迟销毁）

### 应该做

- [ ] **空闲连接超时**：维护 `lastActiveTime`，在事件循环的 timeout 分支里扫描踢掉死连接
- [ ] **心跳机制**：TCP keepalive 默认 2 小时才探测，应用层自己发心跳更快
- [ ] **`EMFILE` 处理**：预留一个空闲 fd 的技巧——先 `close` 它腾位置 → `accept` → 立刻 `close` 新连接 → 重新打开预留 fd。目的是让客户端立刻收到 RST 而不是干等超时
- [ ] **定时器**：用 `timerfd`（Linux）或 `timeout` 参数 + 时间轮/最小堆
- [ ] **`EPOLLONESHOT`**：多线程 Reactor 下避免同一 fd 被多线程并发处理
- [ ] **`EPOLLEXCLUSIVE`**：多进程/多 epoll 实例监听同一端口时避免惊群
- [ ] **日志限流**：持续错误时不要无限刷日志

### 需要理解

- [ ] **TCP 是字节流，没有消息边界**。`recv` 一次拿到的可能是半条消息，也可能是三条半。"粘包/拆包"这个说法其实不准确——TCP 本来就没有"包"的概念，是应用层需要自己定义消息边界（长度字段 / 分隔符）
- [ ] **`EPOLLHUP` / `EPOLLRDHUP` 不等于立即关闭**。对端可能只是关闭了写方向，还有最后一批数据没读完。应该先读完（`recv` 返回 0）再关
- [ ] **`EPOLLERR` / `EPOLLHUP` 无法通过 `epoll_ctl` 屏蔽**，内核总会上报

### 参考实现

想深入的话，读这些项目的源码：

| 项目 | 语言 | 说明 |
|------|------|------|
| **muduo** | C++ | 陈硕的 Reactor 网络库，代码清晰，配套《Linux 多线程服务端编程》 |
| **libevent** | C | 跨平台事件库，封装了 select/poll/epoll/kqueue/IOCP |
| **libuv** | C | Node.js 的底层，跨平台异步 IO |
| **Redis** | C | 单 Reactor 单线程模型的最佳范例，`ae.c` 只有几百行 |
| **Nginx** | C | 多进程 + epoll + accept_mutex |

### 延伸方向

- **io_uring**（Linux 5.1+）：真正的异步 IO，共享内存环形队列，批量提交，是 Linux IO 的未来
- **IOCP**（Windows）：完成端口，Proactor 模型，Windows 上高并发的唯一正解
- **多线程 Reactor**：主从 Reactor 模式，mainReactor 只 accept，subReactor 各自管理连接

---

## 十二、进阶：从 TCP 字节流到 HTTP 服务器

前十章讲的都是「怎么同时盯住很多条连接」。但盯住之后呢？`TcpServer` 给业务代码的只有一句话：

> "fd 7 上来了 573 个字节。"

这就是**字节流**层面的全部信息。而 HTTP 是**应用层协议**，它要求服务器自己找出报文的边界。这一章讲的就是中间补上的那层 `net::http`。

### 12.1 字节流没有"消息"这个概念

`TcpServer` 的 `onMessage(fd, data, len)` 里，`len` 是这一次 `recv` 恰好取回来的字节数 —— 它和「一条 HTTP 请求」**没有任何关系**：

```
客户端发来一个 GET 请求，服务器这边可能看到：

  情况 A（一次收全）      recv → "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"     一个请求
  情况 B（拆包）          recv → "GET /a HTT"     ← 只来了半截
                         recv → "P/1.1\r\nHost:"  ← 又来了半截
                         recv → " x\r\n\r\n"      ← 最后半截，现在才凑齐
  情况 C（粘包/管线化）   recv → "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n"
                                                ↑ 一次来了**两个**完整请求
```

这三种情况在真实网络里都会出现，取决于内核缓冲区、网络拥塞、客户端怎么写。**服务器不能假设任何一种**，只能准备一个增量解析器：来多少吃多少，吃不下就等下次。

这就是 `net::http::HttpContext` 存在的全部理由。

### 12.2 请求的边界是怎么被切出来的

HTTP/1.x 报文的骨架非常简单，边界全靠两个东西：

```
GET /index.html?q=1 HTTP/1.1␍␊        ← 请求行，以 CRLF 结束
Host: example.com␍␊                    ← 头部，每行一个 CRLF
Content-Length: 5␍␊
␍␊                                     ← 空行：头部到此结束
hello                                  ← 正文：由 Content-Length 定长（5 字节）
```

- **请求行和头部**：以 `\r\n` 为分隔符，一个**空行**（连续两个 CRLF）表示头部结束；
- **正文**：由 `Content-Length` 给出精确字节数（或者用 `Transfer-Encoding: chunked`，本项目不支持）。

所以解析器就是一个状态机：

```
        append() 喂字节
              │
              ▼
     ┌──────────────────┐  找到 CRLF   ┌──────────────┐  读到空行  ┌──────────┐
     │  RequestLine     │ ───────────▶ │   Headers    │ ─────────▶ │   Body   │
     │  等 "METHOD SP    │              │  逐行解析     │            │ 读数满 N │
     │   TARGET SP VER" │              │  Key: Value  │            │  个字节  │
     └──────────────────┘              └──────────────┘            └──────────┘
              │                               │                         │
              └───────────────┬───────────────┴─────────────────────────┘
                              ▼ 数据不够
                        Incomplete（等下一次 recv）
                              │ 全部到齐
                              ▼
                          Complete（交给业务）
```

`parse()` 的返回值里还有一个不常见的 `ExpectContinue`，见 12.9。

### 12.3 缓冲区：为什么用"读下标"而不是 `erase`

最直觉的写法是维护一个字符串，每消费一行就把它从头部删掉：

```cpp
inBuf_.erase(0, lineLen);      // ❌ 每次都是 O(剩余长度) 的内存搬移
```

问题是 `erase(0, n)` 要把后面剩下的所有字节**整体往前挪**。一个 100 KB 的请求分成 2000 行头部，每删一行就搬一次，累计就是 100 KB × 2000 = **200 MB 的内存拷贝** —— 全都浪费在把同一批字节挪来挪去上。

改用一个读下标，消费就变成了 O(1)：

```cpp
size_t readPos_ = 0;           // 已经消费到哪儿了
// 消费一行：只推进下标，一个字节都不搬
readPos_ = lineEnd + 2;
```

缓冲区只在攒够一定量之后才真正压缩一次（`compact()`），摊还下来每次消费仍是 O(1)。muduo 的 `Buffer` 用的是同一套思路，只不过它是 `readerIndex_` / `writerIndex_` 两个下标。

还有一个细节：**找 CRLF 要从上次扫描失败的地方继续**（`scanPos_`）。否则一个 8 KiB 的请求行分成 8000 次到达时，每次都要从行首重扫一遍，总量是 1+2+…+8000 ≈ **3200 万次**字节比较 —— 又是一个 O(n²)。

> 找不到 CRLF 时，`scanPos_` 要回退到"缓冲区的最后一个字节"而不是停在末尾。因为最后那个字节很可能正好是个孤零零的 `\r`，它的 `\n` 还在下一个 TCP 段里。停在末尾就会把这个 CRLF 永久跳过，后面的解析全部错位。

### 12.4 keep-alive 与管线化

`parse()` 的设计目标很明确：**一次调用最多产出一个完整请求，缓冲区里剩下的字节原样留着**。

正因为"剩下的字节原样留着"，同一个连接上只要外层反复调用 `parse()`，就能一个接一个地吐出请求。`HttpServer::onMessage` 里的这个循环就是全部秘密：

```cpp
ctx.append(data, len);
for (;;) {
    const auto r = ctx.parse();
    if (r == Incomplete) return;          // 数据不够，等下一次 recv
    if (r == ExpectContinue) { /* 回 100 Continue */ continue; }
    if (r == Error)        { /* 回 4xx 并关闭 */ return; }

    handleRequest(ctx.request(), resp);   // 业务处理
    sendResponse(fd, ctx.request(), resp);

    ctx.reset();                          // 【关键】只清解析状态，不动缓冲区
}
```

`reset()` 里**绝对不能**清空 `inBuf_`。管线化时客户端早就把第二、第三个请求一起发过来了，它们正躺在缓冲区里；清掉就是把它们无声无息地丢了。

连接复用的语义由「版本 + `Connection` 头」共同决定，这是 HTTP 里最容易记反的一处：

| 版本 | 默认行为 | 显式 `Connection: keep-alive` | 显式 `Connection: close` |
|------|----------|------------------------------|--------------------------|
| HTTP/1.0 | **关闭** | 复用 | 关闭 |
| HTTP/1.1 | **复用** | 复用 | 关闭 |

> `Connection` 的值是**逗号分隔的 token 列表**，真实客户端可能发 `keep-alive, Upgrade` 这种。拿整串去 `==` 比较是查不出来的，必须按 token 查（`hasToken()`）。

### 12.5 请求走私：几种必须直接拒绝的报文

HTTP 报文一旦出现"同一段字节，不同实现解读出不同边界"的情况，就可能被用来把一段请求**藏**进另一段里绕过前端的检查 —— 这叫**请求走私**（Request Smuggling）。RFC 7230 §3.3.3 对此有明确规定，实现上就是几条"宁可直接拒绝，也不要猜"的规则：

| 报文特征 | 处理 | 为什么 |
|----------|------|--------|
| `Transfer-Encoding` 与 `Content-Length` **同时出现** | 400 + 关连接 | 前端认 TE、后端认 CL（或反过来），两边算出的长度不一样，多出来的就是夹带的请求 |
| `Content-Length` 出现多次且**值不相同** | 400 | 同上，是最经典的载体 |
| 头部名和冒号之间有空格（`Content-Length : 5`） | 400 | 有的实现当成 `Content-Length`，有的当成叫 `Content-Length ` 的未知头。RFC 7230 §3.2.4 **要求**回 400，不能"顺手 trim 一下就放过" |
| 行首是空格/制表符（折行，obs-fold） | 400 | 老规范允许的续行写法，已废弃；各实现对它的处理不一致，同样是走私温床 |
| `Transfer-Encoding: chunked` | 501 | 本项目不实现分块解码。**绝不能**当成"长度 0"悄悄放过 —— 那样正文会留在缓冲区里被当成下一个请求解析，正合攻击者的意 |
| `Content-Length: 12abc` / `-1` / `0x10` | 400 | 所以这里**不能用 `atoi`/`strtol`**：它们会接受这些写法。必须一个字符一个字符地只认十进制数字 |
| HTTP/1.1 缺 `Host` | 400 | RFC 7230 §5.4 的强制要求；`Host` 决定请求交给哪个虚拟主机 |
| 重复的 `Host` 头 | 400 | 同上，"前后端可能选了不同的主机" |

### 12.6 最凶险的一处时序：响应发完之后才能关连接

这是整个 HTTP 层最容易写错、而且**错了不报任何错**的地方。

先看非阻塞 `send` 的语义：它能发多少发多少，发不完返回已发送的字节数。`TcpServer` 因此有一个应用层发送缓冲区，没发完的会缓存起来，等可写事件续发。

那么问题来了 —— 要回一个 `Connection: close` 的响应，代码该这么写吗？

```cpp
tcp_.send(fd, response);        // ❌
tcp_.closeConnection(fd);       // 立刻关
```

**不行。** 大响应（比如一整个 HTML 文件）在 `send` 返回时必然只发出去了一部分，剩下的正躺在 `outBuf` 里。这一 `close` 会把它们连同 fd 一起丢掉，客户端收到一个**被拦腰截断**的响应。

正确的做法是两条路配合：

```cpp
if (!keepAlive) {
    closing_.insert(fd);                      // ① 先标记"发完就关"

    if (tcp_.sendBufferEmpty(fd)) {
        tcp_.closeConnection(fd);             // ② 小响应一次写完 → 现在就关
    }
    // 否则什么都不做，等 onWriteComplete 回调来关
}
```

为此 `TcpServer` 新增了一个回调（muduo 里叫 `WriteCompleteCallback`）：

```cpp
if (c.outSent >= c.outBuf.size()) {
    const bool hadPending = !c.outBuf.empty();   // 必须是"从有到无"才算写完
    c.outBuf.clear();
    c.outSent = 0;
    updateInterest(fd);                          // 取消可写关注，否则 LT 下 100% 空转
    if (hadPending && onWriteComplete_) {
        onWriteComplete_(fd);                    // 放在最后：回调里可能把 c 所在的
    }                                            // map 项删掉，之后绝不能再碰 c
}
```

两条路合起来覆盖全部情况，一条都不漏：

```
   小响应（一次写完）     sendBufferEmpty() == true   → 立即关
   大响应（分多次写完）   sendBufferEmpty() == false  → 等 writeComplete 回调关
```

> 还有个配套的坑：`onWriteComplete` 回调可能在这时候把连接关掉，而 `handleWrite()` 返回的是 `true`。所以事件循环里紧接着的 `handleRead()` 必须**重新确认连接还在**，否则会去 `recv` 一个已经关闭、甚至已经被内核回收给新连接复用的 fd —— 读到的就是别人的数据了。

### 12.7 真实踩到的坑：RST 把刚发出去的响应吞掉了

这个 bug 是本项目写完之后跑测试才撞出来的，非常值得单独讲。

现象：**客户端发一个超长请求（触发 414 / 431），服务器明明回了错误响应，客户端却一个字节都收不到**，只拿到一个 `WSAECONNRESET (10054)`。

原因在 TCP 的一条硬规则上：

> 如果一个 socket 被 `close()` 时**接收队列里还有没读完的数据**，内核认为这条连接出了异常，会发送 **RST** 而不是正常的 FIN。而对端收到 RST 之后，**内核会把已经收下、但应用还没来得及读走的数据全部丢弃**。

而"大请求被拒绝"恰好完美踩中这条：

```
客户端 ──── 20 KB 的超长请求 ────▶ 服务器
                                    │  读到第 12 KB 时判定超限
                                    │  回一个 414，然后 close
                                    ▼
                              收到 RST，刚写出去的 414 响应
                              在客户端内核里被一并丢掉
```

修复办法很简单：**关之前先把接收队列读干净**。

```cpp
// closeConnection() 里，closeSocket 之前：
char drain[4096];
size_t drained = 0;
while (drained < 256 * 1024) {              // 加个上限，别被持续发送的对端拖住
    const int n = ::recv(fd, drain, sizeof(drain), 0);
    if (n > 0) { drained += n; continue; }
    break;                                   // n==0 对端已关；n<0 要么 EAGAIN 要么真错误
}
closeSocket(fd);
```

正常关闭（对端先发 FIN）时队列本来就是空的，这个循环立刻退出，没有额外开销。真实服务器（比如 nginx）用的是"lingering close"：`shutdown(SHUT_WR)` 之后继续保持读一小段时间，道理是一样的。

### 12.8 静态文件服务的安全链

"把文件发给浏览器"听着简单，但它是 Web 服务器**最经典的安全事故来源**。本项目在 `serveStaticFile()` 里串了一道道检查，顺序不能乱：

```
  req.path（已百分号解码）
        │
        ├─ ① 含 NUL 字节？               → 403   检查的字符串 ≠ 打开的路径
        │                                         （C 字符串会在 NUL 处截断）
        ├─ ② 逐段审查路径分量
        │      · 出现 ".."               → 403   目录穿越
        │      · 出现 "\\"               → 403   Windows 上它也是分隔符
        │      · 出现 ":"                → 403   盘符 / NTFS 数据流
        ├─ ③ 是目录？                    → 追加 index.html
        ├─ ④ 不是普通文件？              → 404   见下面的说明
        ├─ ⑤ 规范化后逃出根目录？        → 403   符号链接逃逸
        └─ ⑥ 读取（std::ios::binary）   → 200
```

几个不那么显然的点：

**① 校验必须在百分号解码之后。** `%2e%2e%2f` 解码出来就是 `../`。如果先校验再解码，检查时看到的是无害的 `%2e%2e%2f`，等真正打开文件时它才现出原形 —— 等于没检查。这是 Web 安全里最经典的绕过手法之一。

**④ 为什么非要确认"是普通文件"。** 如果路径指向一个 FIFO（命名管道）或字符设备，**读取它会永久阻塞**。而我们是单线程事件循环 —— 一条连接卡在 `read` 上，整个服务器的所有连接就一起卡死了。这是"单线程 Reactor 做文件 IO"最凶险的坑，没有之一。

**⑤ 逐段查 `..` 拦不住符号链接。** 静态目录里要是有人放了一个指向 `C:\Windows` 的链接，路径里一个 `..` 都没有。所以还要把最终路径规范化（`weakly_canonical`，会解析掉链接），再确认它确实位于根目录之内。

**⑥ 必须带 `std::ios::binary`。** Windows 上文本模式会把 `\n` 悄悄换成 `\r\n`，读出来的字节数和文件真实大小**对不上** —— 于是我们报出去的 `Content-Length` 是在撒谎，客户端要么提前截断要么一直等。图片、字体、压缩包会直接损坏。

> 顺带一个相关的坑：错误页面会把用户给的路径回显出来。这类回显**必须**配 `text/plain`，绝不能是 `text/html` —— 请求 `/<script>alert(1)</script>` 就变成了一个反射型 XSS。回显用户输入要么用纯文本，要么老老实实做 HTML 转义。

### 12.9 一个容易被忽略的协议往返：`Expect: 100-continue`

`curl` 对**超过 1 KiB** 的 POST 会自动加上 `Expect: 100-continue`，然后**停下来等服务器回话**才肯发正文。服务器要是装看不见，客户端就会一直等到自己的超时（curl 默认 1 秒）才补发 —— 表现为「POST 莫名其妙慢一秒」。

所以解析器在头部读完、正文还没到齐时，会多返回一个 `Result::ExpectContinue`：

```cpp
if (r == HttpContext::Result::ExpectContinue) {
    tcp_.send(fd, "HTTP/1.1 100 Continue\r\n\r\n");   // 注意：1xx 没有 Content-Length
    continue;                                          // 然后继续 parse，正文马上就到
}
```

> 100 是**中间响应**，它靠"空行"就结束了，是 HTTP 里唯一一类不需要正文长度也能定界的响应（RFC 7230 §3.3.3：1xx / 204 / 304 都不带正文）。客户端的解析器必须能识别并把它们跳过，否则会把一个没有正文的响应当成最终结果。

还有一处讲究：**只有前面所有校验都通过了才发这个 100**。如果判定要拒绝（413/400），就一声不吭直接给最终响应 —— 让客户端白白传上来几百 MB 再告诉它"太大了"，是很不礼貌也很浪费的做法。

### 12.10 分层：HTTP 层完全不知道底下是 select 还是 epoll

加上这一层之后，项目的分层变成：

```
┌───────────────────────────────────────────────────────────┐
│  examples/http_server.cpp    业务代码（注册路由 + 静态目录） │
├───────────────────────────────────────────────────────────┤
│  net/http/HttpServer         路由分发 · 静态文件 · 连接上下文│
│  net/http/HttpContext        解析状态机（字节流 → 请求）     │
│  net/http/HttpRequest/Response  纯数据 + 序列化             │
│  net/http/HttpUtil           状态码 · MIME · HTTP-date      │
├───────────────────────────────────────────────────────────┤
│  net/TcpServer               Reactor + 应用层发送缓冲区      │
├───────────────────────────────────────────────────────────┤
│  net/EventPoller             select / poll / epoll 抽象      │
├───────────────────────────────────────────────────────────┤
│  net/SocketUtils             跨平台 socket 基础              │
└───────────────────────────────────────────────────────────┘
```

HTTP 层对下层的依赖只有 **4 个回调 + `send()` + `closeConnection()` + `sendBufferEmpty()`**，一个 `epoll` 字样都找不到。想换后端，改的还是那个枚举值：

```cpp
net::http::HttpServer::Options opts;
opts.tcp.backend = net::EventPoller::Backend::Epoll;   // ← 只改这里
net::http::HttpServer server(opts);
```

### 12.11 编译、运行与验证

```bat
build_msvc.bat
```

```bash
# 服务端（端口 后端 触发模式 静态目录）
./http_server 8080 auto lt www
```

浏览器打开 `http://127.0.0.1:8080/` 就能看到 `www/index.html`，页面上那几个按钮会去调 `/api/time`、`/api/info`、`/api/echo`。

**自动化测试用自带的客户端**（`curl` 演示不了前四条）：

```bash
./http_client 127.0.0.1 8080 8388608
```

它会逐项验证 27 个断言，覆盖这些只有自己拼报文才做得出来的路径：

| 场景 | 验证的是什么 |
|------|--------------|
| 拆包：一个请求分 3 次发 | 服务器不会把不完整的请求当成完整的（还会断言"半截请求时服务器不应响应"） |
| 粘包/管线化：一次发两个请求 | 一次 `onMessage` 里能解析出多个请求，且响应按序返回 |
| keep-alive：一条连接发 3 个请求 | `reset()` 保留了缓冲区里的剩余字节 |
| `Connection: close` | 响应完整送达**之后**才关闭 |
| 大响应（8 MiB） | **发送缓冲区 + `writeComplete` 时序**：长度与内容逐字节校验，任何截断都会失败 |
| `Expect: 100-continue` | 服务器会先回 100 再收正文 |
| 畸形请求 10 项 | 505 / 400 / 414 / 431 / 413 / 403 各就各位，含两种目录穿越写法 |

用 `curl` 手测时注意加 `--noproxy '*'`：如果你机器上配了 `HTTP_PROXY` 环境变量，curl 会把请求发给代理，代理会往响应里塞 `Proxy-Connection`、`Keep-Alive` 之类的头，让人误以为是服务器发的。

```bash
curl --noproxy '*' -v http://127.0.0.1:8080/hello
curl --noproxy '*' -v -X POST --data "abc" http://127.0.0.1:8080/api/echo
curl --noproxy '*' -v -X POST http://127.0.0.1:8080/hello          # 405 + Allow: GET, HEAD
curl --noproxy '*' -I http://127.0.0.1:8080/hello                  # HEAD：有 Content-Length，无正文
```

### 12.12 这一层没有做的事

明确列出来，免得读者以为"能跑就完整了"：

- **不分块传输**：库内部会拒绝 `Transfer-Encoding`（501），响应也总是用 `Content-Length` 定长。这意味着响应必须在内存里完整成形，没法流式生成（比如边查数据库边吐 JSON）。
- **没有 `sendfile`**：静态文件是"读进内存 → 拷进发送缓冲区 → 拷进内核"三次拷贝，而且内存里有一整份副本。真实服务器用 `sendfile(2)`（Linux）/ `TransmitFile`（Windows）让内核从文件页缓存直接 DMA 到网卡。
- **单线程**：处理器里阻塞一下，整个服务器就跟着停。这是设计选择，不是 bug。
- **没有 Range / ETag / 压缩 / Cookie / TLS / HTTP/2**。
- **没有发送缓冲区高水位**：`TcpServer` 的 `outBuf` 仍然是无上限的 `std::vector`（见 7.5 的提示）。大文件 + 慢客户端会让它一直涨。

可以自己动手改的方向，按难度排：加 `sendfile` → 加 chunked 编解码 → 加 `Range` 支持断点续传 → 上主从多线程 Reactor。

---

## 附：文件清单

```
netio/
├── include/net/
│   ├── SocketUtils.h            跨平台 socket 基础（类型/错误/选项/监听套接字）
│   ├── EventPoller.h            多路复用抽象接口 + 工厂
│   ├── SelectPoller.h           select 后端
│   ├── PollPoller.h             poll 后端（仅 Linux）
│   ├── EpollPoller.h            epoll 后端（仅 Linux）
│   ├── TcpServer.h              Reactor 服务器（含 writeComplete 通知）
│   └── http/                    ── HTTP 层（第十二章）──────────────
│       ├── HttpUtil.h           状态码文本 · MIME · HTTP-date · URL 解码
│       ├── HttpRequest.h        已解析的请求（纯数据 + 查询接口）
│       ├── HttpResponse.h       待发送的响应（纯数据 + 序列化）
│       ├── HttpContext.h        解析状态机（字节流 → 请求）★核心
│       └── HttpServer.h         路由 · 静态文件 · keep-alive · 关闭时序
├── src/
│   ├── EventPoller.cpp          工厂实现
│   ├── SelectPoller.cpp         每次 wait 重建 fd_set、O(n) 三连
│   ├── PollPoller.cpp           pollfd 数组，events/revents 分离
│   ├── EpollPoller.cpp          红黑树 + 就绪链表，O(k)
│   ├── TcpServer.cpp            accept/recv/send 循环 + 应用层发送缓冲区
│   └── http/
│       ├── HttpUtil.cpp
│       ├── HttpRequest.cpp
│       ├── HttpResponse.cpp
│       ├── HttpContext.cpp      状态机 + 各项限额校验（防走私 / 防慢速攻击）
│       └── HttpServer.cpp       一次 recv 消化多个请求 + 静态文件安全链
├── examples/
│   ├── echo_server.cpp          回显服务器（三个回调搞定）
│   ├── echo_client.cpp          压测客户端（阻塞 IO 足够用）
│   ├── http_server.cpp          HTTP 服务器（路由 + 静态目录）
│   └── http_client.cpp          HTTP 测试客户端（27 项断言，裸拼报文）
├── www/
│   └── index.html               示例静态页（含调 API 的按钮）
├── CMakeLists.txt
├── build_msvc.bat               Windows 一键构建
└── README.md                    本文件
```
