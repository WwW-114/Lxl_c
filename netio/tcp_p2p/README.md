# tcp_p2p —— 纯 C 的 epoll Reactor + TCP 对等通信 / KV 服务器

用 **C11** 写的一个 TCP 点对点（peer-to-peer）通信程序：**两端地位对等**，
每一端既能监听、也能主动连接，连上之后通过终端交互互相发文本。

同一份 `reactor.c / peer.c / main.c` 还通过 `TCP_P2P_KVSTORE` 条件编译提供
**KV 服务器变体**：复用 epoll reactor 和非阻塞收发，把 peer 收到的完整行交给
[`../../project/kvstore/`](../../project/kvstore/) 的协议层，支持多客户端
`SET/GET/MOD/DEL`。基础版 `tcp_p2p` 的行为保持不变。

它和 [`../multi_io/`](../multi_io/) 解决的是同一个问题（事件循环 + 非阻塞 IO），
但换了一种组织方式：**没有类、没有虚函数，用函数指针表做事件分发**。

- **平台**：仅 Linux（直接 `#include <sys/epoll.h>`）
- **构建**：CMake；`-DTCP_P2P_KVSTORE=ON` 额外生成 `tcp_p2p_kv`
- **规模**：基础版约 1100 行；KV 变体复用同一套网络层，协议/存储核心在 `project/kvstore/`

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
├── reactor.h / reactor.c    epoll 封装（基础版与 KV 版共用，未修改）
├── peer.h    / peer.c       对等连接状态机；KV 模式增加多客户端链表与行分派
├── main.c                   基础交互命令；KV 模式增加服务器命令
├── CMakeLists.txt           基础目标 + 可选 tcp_p2p_kv 目标
└── README.md
```

KV 版额外链接 `../../project/kvstore` 的 `kvstore_core`，本目录不复制 KV
协议代码。`peer.h` 中只有多客户端链表、超长行丢弃状态等字段受
`TCP_P2P_KVSTORE` 保护。

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

### KV 服务器模式（可选）

同一份 `peer.c / main.c` 通过 `TCP_P2P_KVSTORE` 条件编译出第二种形态：

```bash
cmake -S . -B build-kv -DTCP_P2P_KVSTORE=ON
cmake --build build-kv -j

./build-kv/tcp_p2p_kv -l 9001
```

基础版 `tcp_p2p` 仍然会同时构建，行为与原来一致。KV 版的协议核心位于
[`../../project/kvstore/`](../../project/kvstore/)，本目录只负责：

- `do_read` 按 `\n` 切行、去掉行尾 `\r`、处理分片/粘包；
- 把完整行交给 `kvstore_handle_request()`；
- 把响应通过 `peer_send_to()` 放回非阻塞发送缓冲区；
- KV 模式下维护多客户端连接链表；
- 超长请求回 `TOOLONG` 并丢弃到下一个换行；
- 发送缓冲溢出或分配失败时关闭连接，避免客户端永久等待。

协议命令：

| 请求 | 含义 |
|---|---|
| `SET <key> <value...>` | 插入或覆盖；value 可取整行剩余部分，允许空格 |
| `GET <key>` | 查询 |
| `MOD <key> <value...>` | 仅更新已存在的 key |
| `DEL <key>` | 删除已存在的 key |

响应：

| 响应 | 含义 |
|---|---|
| `+OK` | SET/MOD/DEL 成功 |
| `+VALUE <value>` | GET 成功 |
| `-ERR <CODE> <message>` | 错误；CODE 包括 `BADREQ` / `NOTFOUND` / `TOOLONG` / `NOMEM` / `LIMIT` / `INTERNAL` |

用 `nc` 验证：

```bash
printf 'SET user:1 alice\nGET user:1\nMOD user:1 bob\nGET user:1\nDEL user:1\nGET user:1\n' \
  | timeout 2 nc 127.0.0.1 9001
```

预期：

```text
+OK
+VALUE alice
+OK
+VALUE bob
+OK
-ERR NOTFOUND user:1
```

基础版客户端也可以直接连接 KV 服务器：它把输入按行发出去，把响应按行打印。

```bash
# 终端 1
./build-kv/tcp_p2p_kv -l 9001

# 终端 2：基础版目标在 build-kv 中也会一起构建
./build-kv/tcp_p2p -c 127.0.0.1 9001
```

在终端 2 输入 `SET a 1`、`GET a`，会看到 `[Received] +OK`、`[Received] +VALUE 1`。

**条件编译边界**：

- 基础目标 `tcp_p2p` 不定义 `TCP_P2P_KVSTORE`，不 include、不链接任何 kvstore 文件。
- KV 目标 `tcp_p2p_kv` 在 target 级定义宏，保证 `main.c` 与 `peer.c` 的结构体布局一致。
- `reactor.c/h` 完全不需要修改。
- `peer.c / main.c` 中所有差异都放在 `#ifdef TCP_P2P_KVSTORE ... #else 原代码 ... #endif` 内。
- KV 模式下 `peer_node_t` 使用连接链表，默认最多 `KVSTORE_MAX_CLIENTS`（64）个客户端；
  基础模式仍然只维持一条连接并拒绝第二个入向连接。
- KV 模式禁用主动连接 `/connect`，服务器只负责监听和响应。

完整协议、限制、错误语义和数据表实现见
[`../../project/kvstore/README.md`](../../project/kvstore/README.md)。

---

## 与 multi_io 的对照

同一个问题的两种解法，对照着看收获最大：

| 维度 | `multi_io` | `tcp_p2p` |
|------|-----------|-----------|
| 语言 | C++17 | C11 |
| 事件分发 | 基类 + 虚函数 `EventPoller::wait()` | 函数指针 + `slots[fd]` 数组 |
| 回调存储 | `std::unordered_map<socket_t, ...>` | 以 fd 为下标的定长数组 |
| 后端选择 | 运行期可切 select / poll / epoll | 固定 epoll |
| 连接管理 | `std::unordered_map<socket_t, Connection>` | 基础版单个 `peer_t *conn`；KV 版连接链表（默认最多 64） |
| 缓冲 | `std::vector<char>` 自动增长 | `char recv_buff[4096]` 定长 + `send_buff` 堆分配 |
| 业务入口 | 三个 `std::function` 回调 | 基础版 stdin 命令 + 直接打印；KV 版行协议 → `kvstore_handle_request()` |
| 关闭时序 | `closing_` 集合 + `onWriteComplete` | `closing` 标志 + `peer_flush_before_exit` |
| 编译期形态 | 运行期选择 poller 后端 | 基础版/KV 版由 `TCP_P2P_KVSTORE` 条件编译选择 |

> KV 模式不是另起一套事件循环，而是复用同一份 `reactor.c`/`peer.c`，只把
> `do_read` 的「收到一行」分派点换成 KV 协议层。

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

6. **在 `do_read` 里发响应要注意对象失效**
   `peer_send_to()` 最终会走 `peer_queue() → do_write()`；KV 模式下发送队列
   溢出或 `realloc` 失败会直接 `peer_close()` 释放 `peer_t`。因此必须遵守：

   > `peer_send_to()` 返回 `-1` 时立即 `return -1`，不得再访问 `conn`、
   > `conn->recv_buff` 或 `node->conn`。

   否则会在连接已释放后继续 `memmove` 接收缓冲区，产生 use-after-free。

7. **条件编译要用 target 级宏**
   `TCP_P2P_KVSTORE` 必须通过 CMake 的 `target_compile_definitions` 对整个
   `tcp_p2p_kv` 目标生效，不能只在某个 `.c` 里 `#define`。否则 `main.c` 和
   `peer.c` 看到的 `peer_t / peer_node_t` 布局不同，会出现难以定位的问题。

8. **应用层分帧与超长请求**
   TCP 不保留消息边界，`do_read` 必须自己按 `\n` 切行，并处理：
   一行被拆到多次 `read`、多条命令粘在一次 `read`、`\r\n`、空行、
   行内嵌 NUL、以及超长行。KV 版对超长行的策略是「回 `TOOLONG` +
   丢弃到下一个换行」，而不是清空整个接收缓冲区。

---

## 参考

- 同一问题的 C++ 完整实现（含 HTTP 层）：[`../multi_io/README.md`](../multi_io/README.md)
- 手写练习版：[`../Mymulti_io/`](../Mymulti_io/)
- 可靠 UDP 方向：[`../udp_kcp/`](../udp_kcp/)
- KV 协议、存储与网络化说明：[`../../project/kvstore/README.md`](../../project/kvstore/README.md)
- 上级目录导航：[`../README.md`](../README.md)
