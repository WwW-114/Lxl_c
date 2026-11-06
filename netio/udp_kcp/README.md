# udp_kcp —— 基于 KCP 的 UDP 文字聊天室

在 **UDP** 上做可靠传输的练习：用 [KCP](https://github.com/skywind3000/kcp) 协议
实现一个多人在线聊天室——昵称登录、群聊广播、心跳与断线检测。

选 KCP 而不是 TCP，是为了亲眼看到**「不可靠的数据报之上怎么补出可靠性」**：
丢包重传、乱序重排、消息分片重组，这些都是应用层自己做出来的。

- **平台**：仅 Linux（POSIX socket）
- **构建**：CMake
- **依赖**：无第三方库。`ikcp.c` / `ikcp.h` 是 vendored 的 KCP 源码，直接编译进项目

---

## 功能范围

| 有 | 没有 |
|----|------|
| 昵称登录（重名会被拒绝） | 在线用户列表 |
| 群聊广播 | 私聊 |
| 心跳保活 + 超时断线检测 | 历史消息 |
| 主动退出即时通知他人 | 认证 / 加密 |
| 超长消息自动分片重组（上限 4096 字节） | NAT 穿透 |

---

## 架构

### 服务端（单线程 poll 事件循环）

```
                 一个 UDP socket 服务所有客户端
                            │
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
   ┌─────────┐         ┌─────────┐         ┌─────────┐
   │Session A│         │Session B│         │Session C│      每个 Session：
   │ ikcpcb  │         │ ikcpcb  │         │ ikcpcb  │      · 一个独立的 KCP 控制块
   │ addr    │         │ addr    │         │ addr    │      · 对端地址
   │ name    │         │ name    │         │ name    │      · 昵称 / 登录状态
   │ lastRecv│         │ lastRecv│         │ lastRecv│      · 最后活跃时刻
   └─────────┘         └─────────┘         └─────────┘
        │                   │                   │
        └───────────────────┼───────────────────┘
                            ▼
                  会话表：unordered_map<(IP<<16)|port, unique_ptr<Session>>
```

**没有为每个客户端开线程**。所有 KCP 实例都靠同一个 `poll` 循环驱动，
用 `ikcp_check()` 算出「下一次必须调用 `ikcp_update` 的时刻」当作 `poll` 的超时——
既不忙等，也不会延迟发包。

### 客户端（单线程 poll）

同时盯着两个 fd：`stdin`（用户输入）和 UDP socket（服务器消息）。
`SIGINT` 只置一个标志，真正的收尾在主循环里做。

### 应用层协议

```
+--------+---------------------------+
| 1 字节 |        变长载荷            |
|  类型  |                           |
+--------+---------------------------+
```

**没有长度前缀**——因为 KCP 在非 stream 模式下已经保留了消息边界：
一次 `ikcp_send()` 发出去的消息，对端 `ikcp_recv()` 一定原样收回来。

| 方向 | 类型 | 载荷 |
|------|------|------|
| C→S | `MSG_LOGIN` / `MSG_CHAT` / `MSG_LOGOUT` / `MSG_PING` | 昵称 / 正文 / 空 / 空 |
| S→C | `MSG_BROADCAST` / `MSG_SYSTEM` / `MSG_PONG` | 拼好格式的整行文本 / 系统提示 / 空 |

服务器的 `MSG_BROADCAST` 直接发**已经拼好的整行文本**（如 `[alice] 你好`），
客户端拿到就打印，不做任何解析——让精力集中在 KCP 本身而不是协议编解码上。

---

## 文件清单

```
udp_kcp/
├── ikcp.h / ikcp.c        KCP 协议实现（skywind3000，第三方 vendored 源码，未做修改）
├── common.h               两端共用：iclock()、消息类型、常量、pack/msgBody/trim
├── server.cpp             服务端
├── client.cpp             客户端
└── CmakeLists.txt         构建脚本
```

| 文件 | 关键内容 |
|------|---------|
| `common.h` | `iclock()`（KCP 要的毫秒时钟）、`MsgType` 枚举、`KCP_OVERHEAD = 24`、`PACK()`/`msgBody()` |
| `server.cpp` | `Session`、`createSession`、`kcpOutput`、`handleMessage`、`reapSessions`、`dispatchOutbox`、主循环 |
| `client.cpp` | `resolve`、`kcpOutput`、`display`、`drainMessages`、`onSocketReadable`、`onStdinReadable`、主循环 |

---

## 编译与运行

> ⚠️ **构建文件的文件名有坑**：当前是 `CmakeLists.txt`（小写 `m`）。
> CMake 在**区分大小写的文件系统**上严格查找 `CMakeLists.txt`，名字不对会直接报
> 「does not appear to contain CMakeLists.txt」。在 Windows 上通过 `/mnt/d/...`
> 访问时（DrvFs 不区分大小写）能侥幸跑通，但把代码挪到 Linux 原生目录就会失败。
> **建议 `git mv CmakeLists.txt CMakeLists.txt`。**

```bash
cd udp_kcp
cmake -S . -B build
cmake --build build -j
```

产物：`build/server`、`build/client`。

### 运行

```bash
# 终端 1
./build/server 8888

# 终端 2、3、4
./build/client 127.0.0.1 8888 alice
./build/client 127.0.0.1 8888 bob
```

客户端命令：直接输入文字回车发送，`/quit` 或 `Ctrl+C` 退出。

### 验证清单

| 测什么 | 怎么测 | 期望 |
|--------|--------|------|
| 群聊广播 | alice 输入文字 | bob/carol 都收到 `[alice] ...` |
| 中文 | 输入中文 | 不乱码 |
| 昵称重名 | 再开一个叫 alice 的 | 只有它自己收到「已被占用」 |
| **分片重组** | 粘贴 >1376 字节的长文本 | 对端**完整**收到（KCP 自动切成多个包再拼回来） |
| 主动退出 | `/quit` | 其他人**立刻**看到离开提示，**且服务器不会多出"新连接"日志** |
| 异常退出 | `kill -9 <pid>` | 约 30 秒后其他人看到「心跳超时」 |
| **丢包下的可靠性** | 见下 | 消息不丢、不重、不乱序 |

**丢包测试**（这组最能体现 KCP 的价值）：

```bash
sudo tc qdisc add dev lo root netem loss 30%    # 给回环注入 30% 丢包
./build/client 127.0.0.1 8888 dave              # 照常聊天，一条都不会丢
sudo tc qdisc del dev lo root                   # 测完清除
```

对照实验：把 `ikcp_send()` 换成直接 `sendto()`，同样 30% 丢包下消息会成片丢失。

**内存检查**：

```bash
cmake -S . -B build-asan -DENABLE_ASAN=ON
cmake --build build-asan -j && ./build-asan/server 8888
```

---

## 代码依赖的 KCP 行为

下面这些是**读 `ikcp.c` 源码确认过的**，实现时依赖它们（改代码前务必留意）：

| 结论 | 依据 |
|------|------|
| `ikcp_recv` 在非 stream 模式下**保留消息边界**，一次 send 对应一次 recv | `ikcp.c:413` 用 `fragment == 0` 判定消息结束 |
| 缓冲区不足时返回 `-3`，所以要先 `ikcp_peeksize` | `ikcp.c:383` |
| `ikcp_recv` 的 buffer 可以传 `nullptr`，用来丢弃超长消息 | `ikcp.c:395` 只在 buffer 非空时才 memcpy |
| **`ikcp_flush` 开头就 `if (kcp->updated == 0) return;`**，且全程读 `kcp->current` | `ikcp.c:976,991` |
| `ikcp_input` 自带长度校验（< 24 字节返回 -1）和 conv 校验 | `ikcp.c:794,805` |
| `ikcp_send` 超 mss 自动分片（默认 mss = 1400 - 24 = **1376**） | `ikcp.c:475` |
| `IKCP_OVERHEAD` 等常量是 `ikcp.c` 内的 `const` 变量，**头文件不导出** | `ikcp.c:40`，所以 `common.h` 里自己定义了一份 |
| 默认 `rx_rto = IKCP_RTO_DEF = 200ms`，数据发出去 200ms 没 ACK 就重传 | `ikcp.c:27,278,1123` |

**由此得出的实现原则**：不在应用层手动调 `ikcp_flush`，统一由主循环的 `ikcp_update`
驱动（配合 `ikcp_nodelay(kcp, 1, 10, 2, 1)`，延迟上限只有 10ms）。
这样既避开了 `updated` / `current` 的时序陷阱，也符合 KCP「定时驱动」的设计意图。

---

## 真实踩过的坑

三个 bug 都是实际撞上并修掉的，很有代表性：

### 1. `emplace` 静默接管了裸指针 → double free

```cpp
std::unordered_map<uint64_t, std::unique_ptr<Session>> g_sessions;

Session *raw = sp.get();
g_sessions.emplace(key, raw);        // ❌ 编译器一声不吭
```

`unique_ptr` 的指针构造函数是 `explicit` 的，但 `std::pair` 的转发构造函数用
`is_constructible_v` 做约束，而它检查的是**直接初始化**——`explicit` 在直接初始化里合法。
于是 `emplace` **悄悄新建了一个 `unique_ptr` 接管 `raw`**，而 `sp` 仍持有同一对象。
函数返回时 `sp` 析构，map 里就留下一个悬空指针，最后 `clear()` 时二次释放。

**修法**：`g_sessions.emplace(key, std::move(sp));`

### 2. 漏接返回值 → 首包被丢弃 + 悬空指针

```cpp
if (it == g_sessions.end()) {
    createSession(src, ikcp_getconv(data));   // ❌ 返回值没赋给 s
    if (!s) return;                           //    s 恒为 nullptr，必然 return
}
```

后果是双重的：新会话的**第一个包**（含 LOGIN）被丢弃，永远登不上；
而第二次及以后的包会走 `else` 分支取到那个悬空指针，
`ikcp_input` 在**已释放的内存**上读写，把堆写坏——表现就是
`malloc(): double free or corruption (top)` 和一堆看不懂的乱码输出。

**修法**：`s = createSession(...);`

### 3. 客户端退出时丢弃了 ACK → 服务器多出「幽灵会话」

客户端 `Ctrl+C` 后的收尾循环里，`recv` 到的包直接 `while(recv(...)){}` 扔掉了，
**没有喂给 `ikcp_input`**。于是 KCP 永远不知道 `LOGOUT` 已被确认，
`snd_buf` 里的段不会被清除，**200ms 后（`rx_rto`）触发重传**。

而此时服务器早已删掉了这个会话 —— 重传包来自一个「未知地址」，
服务器就为它新建了一个会话，日志里凭空多出一行 `[*] 新连接`，
并且这个幽灵会话要等 30 秒心跳超时才被清掉。

**修法**：收尾循环里把包喂给 `ikcp_input`，并用 `ikcp_waitsnd() == 0`
判断发送队列已排空，排空后立刻退出（而不是死等 300ms）。

> 共同教训：**凡是收到了包，就要么喂给 `ikcp_input`，要么明确知道自己在丢弃什么。**
> KCP 这类状态机对「喂数据」很敏感，漏喂不会报错，只会在别处以诡异的方式爆发。

---

## 已知限制

有意保持简单，需要时可以自己扩展：

- **没有认证和握手**：任何人往端口发一个合法 KCP 包头就能建会话
- **服务器对未知地址的包一律建会话**：客户端退出瞬间的重传就会产生幽灵会话（30 秒后自动清理）。
  想更稳可以加「墓碑」冷却期——记录刚回收的地址，短时间内不为它建新会话
- **掉线判定是单向的**：最坏要等 30 秒
- **不发历史消息**：后进来的人看不到之前的聊天
- **单进程单线程**：`poll` 在几十个连接的量级完全够用；上万连接应换 `epoll` + 定时器堆，
  但 `ikcp_check` 的用法不变

---

## 参考

- KCP 原项目：<https://github.com/skywind3000/kcp>
- 同目录下的其他网络编程练习：[`../README.md`](../README.md)
- IO 多路复用基础：[`../multi_io/README.md`](../multi_io/README.md)
