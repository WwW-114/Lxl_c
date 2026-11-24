# kvstore —— 行文本协议的内存 KV 存储

用 **C11 + C++17** 实现的一个内存键值存储：协议层只用 C（`kvstore.c`），
存储层 `kvstore_table.cpp` 在编译期通过条件编译在三种数据结构之间切换：
**可变长数组**、**红黑树**（`MyRedBlackTree.hpp`）、**哈希表**（`std::unordered_map`）。
核心只负责「解析一行命令 → 操作数据表 → 生成响应」，不依赖 socket；
既可以单独跑 stdin demo，也可以通过 `TCP_P2P_KVSTORE` 条件编译
接入 [`../../netio/tcp_p2p/`](../../netio/tcp_p2p/) 的 epoll reactor，变成多客户端
TCP KV 服务器。

- **语言**：协议层 C11；存储层 C++17（对外保持 C 接口）
- **存储**：编译期可选 `array` / `rbtree` / `hash` 三种后端，进程退出即丢失
- **网络层**：复用 tcp_p2p 的 `reactor.c` + `peer.c`，不另起事件循环
- **协议**：按行文本，`\n` 分隔，兼容 `\r\n`
- **平台**：核心跨平台（需要 C11 + C++17 编译器）；网络版仅 Linux（依赖 `<sys/epoll.h>`）

---

## 1. 构建与两种使用方式

`project/kvstore` 没有独立的顶层 CMake 项目，`kvstore_core` 由
[`netio/tcp_p2p/CMakeLists.txt`](../../netio/tcp_p2p/CMakeLists.txt) 通过
`add_subdirectory` 引入。因此**所有构建都从 tcp_p2p 目录发起**，并通过
`KVSTORE_TABLE_BACKEND` 选择存储后端（`array` / `rbtree` / `hash`）。

> 不再提供手动 `gcc` 编译 demo 的方式：`kvstore_table.cpp` 是 C++ 编译单元，
> 三个后端分支由 CMake 注入的 `KVSTORE_TABLE_USE_*` 宏选择，直接编译会触发
> 文件内的 `#error` 守卫。

### 方式 A：独立 stdin demo（不涉及网络）

`kvstore_demo.c` 从 stdin 读一行、调用协议层、打印响应。它和库一起由 CMake
构建，不需要启动网络：

```bash
cd <仓库根目录>/netio/tcp_p2p

# 默认 array 后端
cmake -S . -B build-kv-array -DTCP_P2P_KVSTORE=ON -DKVSTORE_TABLE_BACKEND=array
cmake --build build-kv-array -j

printf 'SET a 1\nGET a\nMOD a 2\nGET a\nDEL a\nGET a\n' \
  | ./build-kv-array/kvstore/kvstore_demo
```

预期输出：

```text
+OK
+VALUE 1
+OK
+VALUE 2
+OK
-ERR NOTFOUND a
```

### 方式 B：TCP KV 服务器

同一个 build 目录里还会生成 `tcp_p2p_kv`：

```bash
./build-kv-array/tcp_p2p_kv -l 9001
```

基础版 `tcp_p2p` 的行为、日志、单连接语义保持不变；KV 版通过宏
`TCP_P2P_KVSTORE` 启用多客户端、行分派和 KV 专用发送策略。

---

## 2. 文件清单

```
project/kvstore/
├── kvstore_config.h      key/value/请求/响应/表项/客户端数量上限
├── kvstore.h             对外协议接口 kvstore_handle_request()
├── kvstore.c             命令解析、参数校验、响应生成（C11）
├── kvstore_table.h       数据表 CRUD 接口（extern "C"，供 C 侧调用）
├── kvstore_table.cpp     三种存储后端的条件编译实现（C++17）
├── MyRedBlackTree.hpp    红黑树模板，仅 rbtree 后端使用
├── kvstore_demo.c        stdin 独立 demo
├── CMakeLists.txt        后端选项 + kvstore_core 静态库 + kvstore_demo
└── README.md
```

网络侧的接入点不在这里复制，而是位于：

```
netio/tcp_p2p/
├── peer.h / peer.c       条件编译：基础单连接 / KV 多客户端 + 行分派
├── main.c                条件编译：基础交互 / KV 服务器命令
└── CMakeLists.txt        基础目标 + 可选 tcp_p2p_kv 目标
```

---

## 3. 协议

### 3.1 请求

```text
SET <key> <value...>
GET <key>
MOD <key> <value...>
DEL <key>
```

规则：

- 行分隔符为 `\n`；兼容 `\r\n`，解析前去掉行尾 `\r`。
- 命令使用**大写**；key 区分大小写；value 保留原样。
- value 是第二个空格之后的**整行剩余部分**，允许包含空格。
- key 不允许包含空格、NUL、CR、LF 和控制字符。
- value 不允许包含 NUL、CR、LF 和控制字符；长度至少为 1。
- 空行、纯空格行忽略，不产生响应。
- 支持流水线：一次 TCP 读取可包含多条命令，按顺序处理、按顺序响应。
- 支持分片：一行被拆到多次 `read` 中，由 `peer_t::recv_buff` 累积。

### 3.2 响应

```text
+OK
+VALUE <value...>
-ERR <CODE> <message...>
```

- 响应正文内部**不带结尾换行**，由 `peer_queue` 统一追加 `\n`。
- `+VALUE` 后跟一个空格，再跟完整 value。
- 错误响应的第一个 token 是错误码，便于客户端判断。

错误码：

| 错误码 | 含义 |
|---|---|
| `BADREQ` | 命令未知、参数缺失、key/value 非法、行内嵌 NUL |
| `NOTFOUND` | key 不存在（GET/MOD/DEL） |
| `TOOLONG` | 请求行或 key/value 超过限制 |
| `NOMEM` | 内存分配失败 |
| `LIMIT` | 表项数达到上限 |
| `INTERNAL` | 服务端内部错误 |

### 3.3 命令语义

| 命令 | 成功 | 失败 |
|---|---|---|
| `SET key value` | key 存在则覆盖，不存在则插入；返回 `+OK` | `BADREQ` / `NOMEM` / `LIMIT` |
| `GET key` | 返回 `+VALUE <value>` | `-ERR NOTFOUND <key>` |
| `MOD key value` | 仅当 key 存在时更新；返回 `+OK` | `-ERR NOTFOUND <key>` |
| `DEL key` | 仅当 key 存在时删除；返回 `+OK` | `-ERR NOTFOUND <key>` |
| 其他 | — | `-ERR BADREQ unknown command` |

`DEL` 和 `MOD` 采用严格语义：key 不存在时返回 `NOTFOUND`，不是幂等成功。

### 3.4 限制

定义在 `kvstore_config.h`，可用 CMake 的 `-D` 覆盖：

| 宏 | 默认值 | 含义 |
|---|---:|---|
| `KVSTORE_KEY_MAX` | 128 | key 最大字节数 |
| `KVSTORE_VALUE_MAX` | 2048 | value 最大字节数 |
| `KVSTORE_REQUEST_MAX` | 3072 | 单行请求最大字节数（不含 `\n`） |
| `KVSTORE_RESPONSE_MAX` | 2304 | 响应缓冲区大小 |
| `KVSTORE_TABLE_MAX_ENTRIES` | 10000 | 表项数上限 |
| `KVSTORE_MAX_CLIENTS` | 64 | KV 模式最大并发客户端数 |

约束：

- `KVSTORE_REQUEST_MAX` 必须小于 tcp_p2p 的 `BUFFER_SIZE`（当前 4096）。
- `KVSTORE_RESPONSE_MAX` 必须大于 `KVSTORE_VALUE_MAX`。
- 如果放大限制，需要同时评估 `peer_t::recv_buff`、发送缓冲上限和内存占用。

### 3.5 交互示例

```text
C: SET user:1 alice
S: +OK
C: GET user:1
S: +VALUE alice
C: MOD user:1 bob
S: +OK
C: GET user:1
S: +VALUE bob
C: DEL user:1
S: +OK
C: GET user:1
S: -ERR NOTFOUND user:1
C: GET
S: -ERR BADREQ missing key
C: SET k
S: -ERR BADREQ missing value
C: FOO x
S: -ERR BADREQ unknown command
```

---

## 4. 架构与数据流

### 4.1 分层

```
TCP 字节流
   │
   ▼
peer.c  do_read()
   │   按 '\n' 切行，去掉行尾 '\r'，处理分片/粘包/超长行
   ▼
#ifdef TCP_P2P_KVSTORE
   │
   ▼
kvstore_handle_request()
   │   解析命令、校验参数、调用数据表、生成响应
   ▼
kvstore_table_*()
   │   存储后端（编译期三选一）：
   │     array  = 可变长数组（malloc / realloc / free）
   │     rbtree = MyRedBlackTree<std::string, std::string>
   │     hash   = std::unordered_map<std::string, std::string>
   ▼
响应字符串
   │
   ▼
peer_send_to() → peer_queue() → do_write()
   │   非阻塞发送缓冲区；部分写时关注 EPOLLOUT
   ▼
TCP 字节流
```

基础版不定义 `TCP_P2P_KVSTORE`，`do_read` 仍然按原逻辑打印收到的行，
不 include、不链接任何 kvstore 文件。

### 4.2 条件编译边界

- `TCP_P2P_KVSTORE` 通过 CMake 的 `target_compile_definitions` 对整个
  `tcp_p2p_kv` 目标生效，保证 `main.c` 和 `peer.c` 看到的 `peer_t /
  peer_node_t` 布局一致。
- `KVSTORE_TABLE_BACKEND` 通过 `target_compile_definitions` 给 `kvstore_core`
  注入 `KVSTORE_TABLE_USE_ARRAY` / `KVSTORE_TABLE_USE_RBTREE` /
  `KVSTORE_TABLE_USE_HASH` 之一，`kvstore_table.cpp` 用 `#if / #elif`
  选择实现；三者互斥，不会同时参与编译。
- `peer.c` 中所有 KV 行为都在 `#ifdef TCP_P2P_KVSTORE ... #else 原代码 ... #endif` 内。
- `reactor.c/h` 完全不需要修改。
- 基础目标 `tcp_p2p` 不定义宏、不链接 `kvstore_core`。

### 4.3 多客户端

基础版 tcp_p2p 刻意只维持一条连接；KV 版在 `peer_node_t` 中增加连接链表：

```c
peer_t *conns;
size_t conn_count;
```

每个入向连接独立分配 `peer_t`，拥有自己的 `recv_buff` 和 `send_buff`。
所有客户端共享同一张 KV 表，但整个程序仍然是**单线程单 reactor**，
因此数据表不需要加锁。

超过 `KVSTORE_MAX_CLIENTS` 的连接会被立即拒绝并关闭。

### 4.4 发送路径与 use-after-free

`peer_send_to()` 最终会走到 `peer_queue() → do_write()`。在 KV 模式下：

- 发送缓冲超过 1MB 或 `realloc` 失败时，直接关闭连接并返回 `-1`。
- `do_write()` 写失败时也会 `peer_close()`，释放 `peer_t`。

因此 `do_read()` 内有一条固定规则：

> 任何 `peer_send_to()` 返回 `-1`，立即 `return -1`，不得再访问 `conn`、
> `conn->recv_buff`、`node->conn` 或继续处理后续行。

否则会在连接已被释放后继续 `memmove` 接收缓冲区，产生 use-after-free。

### 4.5 超长行处理

- 请求缓冲区写满仍没有 `\n` 时，KV 模式回复一次
  `-ERR TOOLONG request too long`，然后进入 `discarding` 状态。
- `discarding` 状态下持续丢弃字节，直到遇到下一个 `\n`，再恢复正常解析。
- 已经出现完整换行、但长度超过 `KVSTORE_REQUEST_MAX` 的行，直接回复
  `TOOLONG` 并跳过，不影响后续命令。

这样超长请求不会把连接协议打乱，也不会无限占用内存。

---

## 5. 数据表实现：三种可切换后端

数据表的对外接口固定在 `kvstore_table.h`，内部实现由 `kvstore_table.cpp`
中的条件编译在三种数据结构之间选择：

| `KVSTORE_TABLE_BACKEND` | 宏 | 数据结构 | 查找 / 插入 / 删除 |
|---|---|---|---|
| `array`（默认） | `KVSTORE_TABLE_USE_ARRAY` | 可变长数组 `kv_entry_t[]` | O(n) 线性查找；删除用末项覆盖空洞 |
| `rbtree` | `KVSTORE_TABLE_USE_RBTREE` | `MyRedBlackTree<std::string, std::string>` | O(log n) |
| `hash` | `KVSTORE_TABLE_USE_HASH` | `std::unordered_map<std::string, std::string>` | 平均 O(1)，最坏 O(n) |

源码结构：

```cpp
#if defined(KVSTORE_TABLE_USE_ARRAY)
    /* kv_entry_t / kv_table_t + malloc / realloc / free（原 C 实现） */
#elif defined(KVSTORE_TABLE_USE_RBTREE)
    /* MyRedBlackTree<std::string, std::string> */
#elif defined(KVSTORE_TABLE_USE_HASH)
    /* std::unordered_map<std::string, std::string> */
#else
#error "... 请通过 CMake 选项 KVSTORE_TABLE_BACKEND 选择后端"
#endif
```

为什么整个文件是 `.cpp`：

- 红黑树是 C++ 模板，`std::unordered_map` 也是 C++ 容器，编译单元必须按 C++ 编译；
- 数组分支的 `kv_entry_t` / `kv_table_t`、`malloc / realloc / free` 和删除时
  “末项覆盖空洞”的逻辑与原来的 C 实现逐行一致，改由 C++ 编译器编译后行为不变；
- 对外函数在 `kvstore_table.h` 的 `extern "C"` 块中声明，C 侧（`kvstore.c`、
  `peer.c`、`main.c`）按普通 C 函数调用，不需要任何修改。

操作语义（三种后端一致）：

- `SET`：key 存在则覆盖 value；不存在则插入。覆盖不受上限影响；插入时若
  `size >= KVSTORE_TABLE_MAX_ENTRIES` 返回 `LIMIT`。
- `GET`：查找并返回内部指针；调用方（`kvstore.c`）在本次请求内立即复制到响应。
- `MOD`：只更新已存在的 key；不存在返回 `NOT_FOUND`。
- `DEL`：仅删除已存在的 key；不存在返回 `NOT_FOUND`。
- `NOMEM`：数组后端对应 `malloc / realloc` 失败；C++ 后端统一捕获
  `std::bad_alloc`，异常不会越过 `extern "C"` 边界。

为什么保留三种实现：这是学习项目，用来对比同一接口下线性结构、平衡树和哈希表
在代码形态、复杂度和内存管理上的差异；协议层和网络层完全不需要感知后端变化。

---

## 6. 构建与运行

### 6.1 三种后端

```bash
cd <仓库根目录>/netio/tcp_p2p

# array（默认，可省略 -DKVSTORE_TABLE_BACKEND）
cmake -S . -B build-kv-array -DTCP_P2P_KVSTORE=ON -DKVSTORE_TABLE_BACKEND=array
cmake --build build-kv-array -j

# rbtree
cmake -S . -B build-kv-rbtree -DTCP_P2P_KVSTORE=ON -DKVSTORE_TABLE_BACKEND=rbtree
cmake --build build-kv-rbtree -j

# hash
cmake -S . -B build-kv-hash -DTCP_P2P_KVSTORE=ON -DKVSTORE_TABLE_BACKEND=hash
cmake --build build-kv-hash -j
```

- configure 阶段会打印 `kvstore table backend: <name>`，可直接确认选中的实现。
- `KVSTORE_TABLE_BACKEND` 是 CMake cache 变量：切换后端建议使用新的 build
  目录；若在同一目录切换，重新 configure 时必须显式带 `-DKVSTORE_TABLE_BACKEND=...`。
- 基础版 `tcp_p2p` 的构建方式不变：

```bash
cmake -S . -B build-base -DTCP_P2P_KVSTORE=OFF
cmake --build build-base -j
```

### 6.2 启动与交互

启动（以 hash 后端为例，其他后端替换 build 目录名即可）：

```bash
./build-kv-hash/tcp_p2p_kv -l 9001
```

服务器交互命令：

| 命令 | 行为 |
|---|---|
| `/listen <port>` | 开始监听 |
| `/unlisten` | 停止监听（已有连接保留） |
| `/disconnect` | 断开所有客户端 |
| `/status` | 显示监听地址、客户端数、最近客户端、数据项数 |
| `/help` | 帮助 |
| `/quit` | 退出 |
| 其他文本 | KV 服务器模式不接受本地文本转发，只提示使用 KV 客户端 |

KV 服务器模式不支持 `/connect`；需要客户端时使用基础版 `tcp_p2p -c`、
`nc` 或自己的 TCP 客户端。

### 6.3 用 `nc` 测试

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

部分 `nc` 版本需要用 `-q 1` 或 `-N` 让它在 stdin 结束后关闭连接。

测试 CRLF：

```bash
printf 'SET a 1\r\nGET a\r\n' | timeout 2 nc 127.0.0.1 9001
```

测试分包：

```bash
python3 - <<'PY'
import socket, time
s = socket.create_connection(("127.0.0.1", 9001))
s.sendall(b"SET fr")
time.sleep(0.1)
s.sendall(b"ag 123\nGET frag\n")
time.sleep(0.1)
print(s.recv(4096).decode(), end="")
s.close()
PY
```

测试超长行后继续发送：

```bash
python3 - <<'PY'
import socket, time
s = socket.create_connection(("127.0.0.1", 9001))
s.sendall(b"SET k " + b"x" * 5000 + b"\nGET after\n")
time.sleep(0.2)
print(s.recv(4096).decode(), end="")
s.close()
PY
```

### 6.4 基础版客户端兼容测试

基础版 tcp_p2p 客户端不解析 KV 协议，只是按行发送、按行打印收到的内容，
因此可以直接拿来当 KV 客户端：

```bash
# 终端 1：KV 服务器
./build-kv-array/tcp_p2p_kv -l 9001

# 终端 2：基础版客户端
./build-base/tcp_p2p -c 127.0.0.1 9001
```

在终端 2 输入：

```text
SET a 1
GET a
```

客户端会打印：

```text
[Received] +OK
[Received] +VALUE 1
```

这同时验证了「基础版行为未被破坏」和「KV 服务器可被普通 TCP 客户端访问」。

### 6.5 独立 demo

见本文第 1 节。任意后端的构建目录都会生成
`build-kv-<backend>/kvstore/kvstore_demo`，可以直接运行。

---

## 7. 内存与并发说明

- 单线程 reactor：所有客户端和 KV 表操作都在同一个线程里，没有数据竞争，
  因此不需要 mutex。
- `GET` 返回表内部指针，调用方必须在本次请求内复制到响应；当前实现是
  「取出 → `resp_append` 复制」的立即使用模式。
- 三种后端的内存管理：
  - `array`：每个 key/value 用 `malloc` 复制，扩容时 `realloc`，`destroy`
    逐个 `free`；删除时释放空洞项并用最后一项覆盖。
  - `rbtree`：每个节点 `new` / `delete`，哨兵 `_nil` 在树析构时释放；
    覆盖 value 时先构造新 `std::string` 再赋值，分配失败不会破坏旧值。
  - `hash`：节点由 `std::unordered_map` 管理；`destroy` 调用 `clear()`
    释放全部节点，静态表对象在进程退出时析构。
- 表项预算：`KVSTORE_TABLE_MAX_ENTRIES × (key + value)`，默认最坏约
  10000 × 2KB 量级，实际远小于此。生产使用应进一步限制总字节数。
- 程序退出时 `kvstore_table_destroy()` 释放当前后端的全部表项；网络连接在
  `peer_node_destory()` 中统一关闭。

---

## 8. 已知限制与可扩展方向

当前实现刻意保持小而完整，以下内容**不在**当前范围内：

- 没有持久化（RDB/AOF/快照），重启即丢失。
- 没有认证、TLS、访问控制；默认监听 `0.0.0.0`，仅适合本地实验。
- 没有 TTL/过期、订阅/发布、事务、批量命令。
- 默认 `array` 后端是 O(n) 线性查找；需要更好性能时通过
  `-DKVSTORE_TABLE_BACKEND=rbtree`（O(log n)）或
  `-DKVSTORE_TABLE_BACKEND=hash`（平均 O(1)）切换，协议层和网络层无需改动。
- 单线程处理所有客户端；需要更高吞吐时可引入多 reactor/线程池，
  并为数据表加锁或分片。
- 协议是文本行协议，value 不能包含 NUL/CR/LF；需要二进制安全时可改成
  长度前缀协议。

---

## 9. 快速检查清单

```bash
# 1. 构建三种后端之一（以 hash 为例；array / rbtree 同理）
cd <仓库根目录>/netio/tcp_p2p
cmake -S . -B build-kv-hash -DTCP_P2P_KVSTORE=ON -DKVSTORE_TABLE_BACKEND=hash
cmake --build build-kv-hash -j

# 2. 核心 demo（不需要网络）
printf 'SET a 1\nGET a\nMOD a 2\nGET a\nDEL a\nGET a\n' \
  | ./build-kv-hash/kvstore/kvstore_demo

# 3. 基础版回归
cmake -S . -B build-base -DTCP_P2P_KVSTORE=OFF
cmake --build build-base -j
./build-base/tcp_p2p -l 9001
./build-base/tcp_p2p -c 127.0.0.1 9001

# 4. KV 版
./build-kv-hash/tcp_p2p_kv -l 9001
printf 'SET a 1\nGET a\nDEL a\nGET a\n' | timeout 2 nc 127.0.0.1 9001

# 5. 错误路径
printf 'GET\nSET k\nGET k extra\nFOO x\n' | timeout 2 nc 127.0.0.1 9001

# 6. 多客户端
# 两个终端分别 nc 127.0.0.1 9001，一个 SET，另一个 GET

# 7. 内存检查
valgrind --leak-check=full ./build-kv-hash/tcp_p2p_kv -l 9001
```

---

## 10. 相关文档

- 网络层与基础版 tcp_p2p：[`../../netio/tcp_p2p/README.md`](../../netio/tcp_p2p/README.md)
- netio 目录导航：[`../../netio/README.md`](../../netio/README.md)
- 仓库根目录：[`../../README.md`](../../README.md)
