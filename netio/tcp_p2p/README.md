# tcp_p2p —— 纯 C 的 epoll Reactor + TCP 对等通信

用 **C11** 写的一个 TCP 点对点（peer-to-peer）通信程序：**两端地位对等**，
每一端既能监听、也能主动连接，连上之后通过终端交互互相发文本。

它和 [`../multi_io/`](../multi_io/) 解决的是同一个问题（事件循环 + 非阻塞 IO），
但换了一种组织方式：**没有类、没有虚函数，用函数指针表做事件分发**。

- **平台**：仅 Linux（直接 `#include <sys/epoll.h>`）
- **构建**：CMake
- **规模**：约 1100 行

---

## 分层结构

```
┌──────────────────────────────────────────────────────────┐
│  main.c        命令行解析、交互命令、把 stdin 也注册进事件循环 │
│                /connect /listen /disconnect /status ...    │
├──────────────────────────────────────────────────────────┤
│  peer.c/h      「对等节点」：监听 + 主动连接 + 收发 + 状态机   │
│                handle_accept / handle_connect / do_read ... │
├──────────────────────────────────────────────────────────┤
│  reactor.c/h   epoll 的最小封装：注册 fd → 回调                │
│                reactor_add / modify / remove / run          │
└──────────────────────────────────────────────────────────┘
```

三层的职责边界很清楚：`reactor` 只管「fd 就绪了就调用对应的回调」，
完全不知道 TCP、连接、消息这些东西；`peer` 才是业务；`main` 是交互外壳。

---

## 文件清单

```
tcp_p2p/
├── reactor.h / reactor.c    epoll 封装（39 + 158 行）
├── peer.h    / peer.c       对等连接的状态机（56 + 588 行）
├── main.c                   命令行与交互命令（262 行）
└── CMakeLists.txt
```

### `reactor.h` —— 事件分发

```c
typedef void (*event_handler_t)(reactor_t *reactor, int fd,
                                uint32_t events, void *arg);

struct reactor {
  int epoll_id;
  bool running;
  struct epoll_event events[MAX_EVENTS];
  event_wrapper_t *slots[MAX_FDS];      // ← 按 fd 直接索引的回调表
};

int reactor_add(reactor_t *reactor, int fd, uint32_t events,
                event_handler_t handler, void *arg);
int reactor_modify(reactor_t *reactor, int fd, uint32_t events,
                   event_handler_t handler, void *arg);
int reactor_remove(reactor_t *reactor, int fd);
void reactor_run(reactor_t *reactor);
void reactor_stop(reactor_t *reactor);
int set_noblocking(int fd);
```

**核心设计是 `slots[MAX_FDS]`**：一个以 fd 为下标的数组，每个槽存
「这个 fd 就绪时该调哪个函数、带什么参数」。epoll 返回就绪事件后，
`reactor_run` 只需 `slots[fd]->handler(...)` 一次数组索引就能分发——**O(1)**，
比哈希表还快，代价是 fd 上限被固定成 `MAX_FDS`（1024）。

对比 `multi_io` 的做法：那边是 C++，用基类 `EventPoller` + 虚函数 + `std::unordered_map`
存 fd → 回调的映射；这边是 C，用**函数指针 + 数组**。两种写法要表达的东西完全一样，
这是观察「C 怎么模拟面向对象」的一个具体样本。

### `peer.h` —— 对等节点的状态

```c
typedef enum {
  PEER_DISCONNECTED = 0,
  PEER_CONNECTIING,          // 非阻塞 connect 已发出，等可写 = 连接建立
  PEER_CONNECTED,
} peer_state_t;

typedef struct peer {
  int fd;
  peer_state_t state;
  bool inbound;              // 这条连接是 accept 来的还是 connect 出去的
  bool closing;              // 正在关闭：数据发完再真正 close
  uint32_t interest;         // 当前关注的事件（Read / Write 组合）
  struct sockaddr_in peer_addr;
  char recv_buff[BUFFER_SIZE];
  char *send_buff;           // 发送缓冲区：处理非阻塞 send 的部分写
  size_t send_len, send_sent, send_cap;
} peer_t;

typedef struct peer_node {   // 一个「节点」= 一个监听 fd + 至多一条连接
  reactor_t *reactor;
  int listen_fd;
  struct sockaddr_in listen_addr;
  peer_t *conn;
} peer_node_t;
```

几个值得注意的字段：

- **`state` + `inbound`**：主动连接要处理非阻塞 connect 的中间态——
  `connect()` 立刻返回 `EINPROGRESS`，得等这个 fd **可写**了才能确认连接建立。
  `accept` 来的连接则直接就是 `PEER_CONNECTED`。
- **`closing`**：`/disconnect` 不能立刻 `close`，否则发送缓冲区里没发完的数据就丢了。
  先置 `closing`，等 `do_write` 把队列清空再真正关闭（对应 `multi_io` 里的 `closing_` 集合）。
- **`send_buff` 是堆上可增长的**，不像 `recv_buff` 是定长数组——因为待发数据量取决于业务。

### `main.c` —— 交互外壳

把 **stdin 也设成非阻塞并注册进同一个 reactor**，所以键盘输入和网络事件在同一套
机制里处理，不需要额外线程。输入的内容按前缀分流：

| 输入 | 行为 |
|------|------|
| `/connect <host> <port>` | 主动连接对端（已有连接会先断开） |
| `/listen <port>` | 开始监听（**可与主动连接并存**） |
| `/unlisten` | 停止监听（已建立的连接保留） |
| `/disconnect` | 断开当前连接（待发数据会先写完） |
| `/status` | 查看监听与连接状态 |
| `/help` | 显示帮助 |
| `/quit` | 退出 |
| 其它文本 | 直接发给当前连接的对端 |

---

## 编译与运行

```bash
cd tcp_p2p
cmake -S . -B build
cmake --build build -j
```

CMake 配置摘要：C11、`-Wall -Wextra -Wpedantic`、默认 `Debug`（`-O0 -g3 -DDEBUG`）。

### 用法

```bash
# 三种启动方式
./build/tcp_p2p -l 9001                  # 直接监听
./build/tcp_p2p -c 127.0.0.1 9001        # 直接连接对端
./build/tcp_p2p                         # 交互模式，启动后再 /listen 或 /connect
```

**最小验证**（两个终端）：

```bash
# 终端 1
./build/tcp_p2p -l 9001

# 终端 2
./build/tcp_p2p -c 127.0.0.1 9001
```

连上后，任意一端直接敲文字回车，另一端就会收到。

也可以用 `nc` 当对端来验证（`nc 127.0.0.1 9001`）——因为传输的就是裸字节流，
没有任何自定义的协议头。

---

## 与 multi_io 的对照

同一个问题的两种解法，对照着看收获最大：

| 维度 | `multi_io` | `tcp_p2p` |
|------|-----------|-----------|
| 语言 | C++17 | C11 |
| 事件分发 | 基类 + 虚函数 `EventPoller::wait()` | 函数指针 + `slots[fd]` 数组 |
| 回调存储 | `std::unordered_map<socket_t, ...>` | 以 fd 为下标的定长数组 |
| 后端选择 | 运行期可切 select / poll / epoll | 固定 epoll |
| 连接管理 | `std::unordered_map<socket_t, Connection>` | 单个 `peer_t *conn`（只维持一条连接） |
| 缓冲 | `std::vector<char>` 自动增长 | `char recv_buff[4096]` 定长 + `send_buff` 堆分配 |
| 业务入口 | 三个 `std::function` 回调 | stdin 命令 + 直接打印 |
| 关闭时序 | `closing_` 集合 + `onWriteComplete` | `closing` 标志 + `peer_flush_before_exit` |

---

## 学习要点

这个项目里有几处是 C 语言网络编程特有的、`multi_io` 里看不到的东西：

1. **非阻塞 connect 的三步**
   `connect()` 返回 -1 且 `errno == EINPROGRESS` 是**正常的**，不是错误。
   之后要等这个 fd 变成**可写**，再用 `getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)`
   取真正的结果——直接看可写就以为连上了是错的。

2. **`EINTR` 必须重试**
   C 里信号处理更常见（`SIGINT`、`SIGTERM`），被信号打断的系统调用要 `continue`，
   而不是当成错误处理。

3. **fd 作为数组下标**
   `slots[MAX_FDS]` 这种写法只在 fd 数量可控时成立。fd 是**进程内最小可用整数**，
   上限由 `RLIMIT_NOFILE` 决定；一旦超过 `MAX_FDS` 就会越界——所以 `reactor_add`
   必须检查边界。

4. **stdin 也是 fd**
   把 `STDIN_FILENO` 设成非阻塞、注册进 epoll，就能用同一套事件循环处理键盘输入，
   不需要开线程或 `select` 单独轮询。

5. **发送缓冲区的必要性**
   和 `multi_io` 一样：非阻塞 `send` 只保证「能发多少发多少」，
   剩下的必须自己排队，并在 `interest` 里加上 `EPOLLOUT`；
   队列排空后要立刻**取消**对 `EPOLLOUT` 的关注，否则 LT 模式下会忙轮询。

---

## 参考

- 同一问题的 C++ 完整实现（含 HTTP 层）：[`../multi_io/README.md`](../multi_io/README.md)
- 手写练习版：[`../Mymulti_io/`](../Mymulti_io/)
- 可靠 UDP 方向：[`../udp_kcp/`](../udp_kcp/)
