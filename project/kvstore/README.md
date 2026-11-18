# kvstore —— 行文本协议的内存 KV 存储

用 **C11** 实现的一个内存键值存储：核心只负责「解析一行命令 → 操作数据表 → 生成响应」，
不依赖 socket；既可以单独跑 stdin demo，也可以通过 `TCP_P2P_KVSTORE` 条件编译
接入 [`../../netio/tcp_p2p/`](../../netio/tcp_p2p/) 的 epoll reactor，变成多客户端
TCP KV 服务器。

- **语言**：C11
- **存储**：内存动态数组，进程退出即丢失
- **网络层**：复用 tcp_p2p 的 `reactor.c` + `peer.c`，不另起事件循环
- **协议**：按行文本，`\n` 分隔，兼容 `\r\n`
- **平台**：核心可独立编译；网络版仅 Linux（依赖 `<sys/epoll.h>`）

---

## 1. 两种使用方式

### 方式 A：独立 stdin demo

`kvstore_demo.c` 从 stdin 读一行、调用协议层、打印响应。适合先单独验证
解析、存储和内存管理，不涉及网络。

```bash
cd <仓库根目录>/project/kvstore
gcc -std=c11 -Wall -Wextra -Wpedantic -I. \
    kvstore.c kvstore_table.c kvstore_demo.c -o kvstore_demo

printf 'SET a 1\nGET a\nMOD a 2\nGET a\nDEL a\nGET a\n' | ./kvstore_demo
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

在 tcp_p2p 目录用 CMake 打开 `TCP_P2P_KVSTORE`，会同时构建基础版 `tcp_p2p`
和 KV 版 `tcp_p2p_kv`：

```bash
cd <仓库根目录>/netio/tcp_p2p
cmake -S . -B build-kv -DTCP_P2P_KVSTORE=ON
cmake --build build-kv -j

./build-kv/tcp_p2p_kv -l 9001
```

基础版 `tcp_p2p` 的行为、日志、单连接语义保持不变；KV 版通过宏
`TCP_P2P_KVSTORE` 启用多客户端、行分派和 KV 专用发送策略。

---

## 2. 文件清单

```
project/kvstore/
├── kvstore_config.h      key/value/请求/响应/表项/客户端数量上限
├── kvstore.h             对外协议接口 kvstore_handle_request()
├── kvstore.c             命令解析、参数校验、响应生成
├── kvstore_table.h       数据表 CRUD 接口
├── kvstore_table.c       动态数组存储、扩容、SET/MOD/DEL
├── kvstore_demo.c        stdin 独立 demo
├── CMakeLists.txt        生成 kvstore_core 静态库与 kvstore_demo
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
   │   动态数组：查找 / 覆盖 / 删除 / 扩容
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

## 5. 数据表实现

`kvstore_table.c` 使用动态数组：

```c
typedef struct {
  char *key;
  char *value;
} kv_entry_t;

typedef struct {
  kv_entry_t *entries;
  size_t count;
  size_t cap;
} kv_table_t;
```

操作语义：

- `SET`：线性查找；存在则替换 value，不存在则追加；达到上限返回 `LIMIT`。
- `GET`：线性查找，返回内部指针；调用方在本次请求内立即复制到响应。
- `MOD`：只更新已存在的 key；不存在返回 `NOT_FOUND`。
- `DEL`：释放 key/value，用最后一项覆盖空洞，`count--`。
- `destroy`：释放全部 key/value 和数组。

为什么先用线性数组：

- 当前定位是网络编程学习项目，表项规模不大；
- 动态数组比哈希表更容易验证内存管理和边界；
- 对外接口稳定，后续可以把 `kvstore_table.c` 内部替换成链式哈希表，
  不影响协议层和网络层。

---

## 6. 构建与运行

### 6.1 网络版

```bash
cd <仓库根目录>/netio/tcp_p2p

# 基础版：行为与改造前一致
cmake -S . -B build-base -DTCP_P2P_KVSTORE=OFF
cmake --build build-base -j

# KV 服务器版
cmake -S . -B build-kv -DTCP_P2P_KVSTORE=ON
cmake --build build-kv -j
```

启动：

```bash
./build-kv/tcp_p2p_kv -l 9001
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

### 6.2 用 `nc` 测试

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

### 6.3 基础版客户端兼容测试

基础版 tcp_p2p 客户端不解析 KV 协议，只是按行发送、按行打印收到的内容，
因此可以直接拿来当 KV 客户端：

```bash
# 终端 1：KV 服务器
./build-kv/tcp_p2p_kv -l 9001

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

### 6.4 独立 demo

见本文第 1 节。网络版构建时也会生成
`build-kv/kvstore/kvstore_demo`，可以直接运行。

---

## 7. 内存与并发说明

- 单线程 reactor：所有客户端和 KV 表操作都在同一个线程里，没有数据竞争，
  因此不需要 mutex。
- `GET` 返回表内部指针，调用方必须在本次请求内复制到响应；当前实现是
  「取出 → `resp_append` 复制」的立即使用模式。
- `SET/MOD` 覆盖已有 key 时，先分配新 value，成功后再释放旧 value，
  分配失败不会破坏旧数据。
- 表项预算：`KVSTORE_TABLE_MAX_ENTRIES × (key + value)`，默认最坏约
  10000 × 2KB 量级，实际远小于此。生产使用应进一步限制总字节数。
- 程序退出时 `kvstore_table_destroy()` 释放全部表项；网络连接在
  `peer_node_destory()` 中统一关闭。

---

## 8. 已知限制与可扩展方向

当前实现刻意保持小而完整，以下内容**不在**当前范围内：

- 没有持久化（RDB/AOF/快照），重启即丢失。
- 没有认证、TLS、访问控制；默认监听 `0.0.0.0`，仅适合本地实验。
- 没有 TTL/过期、订阅/发布、事务、批量命令。
- 数据表是 O(n) 线性查找；数据量大时可替换为链式哈希表。
- 单线程处理所有客户端；需要更高吞吐时可引入多 reactor/线程池，
  并为数据表加锁或分片。
- 协议是文本行协议，value 不能包含 NUL/CR/LF；需要二进制安全时可改成
  长度前缀协议。

---

## 9. 快速检查清单

```bash
# 1. 核心 demo
printf 'SET a 1\nGET a\nMOD a 2\nGET a\nDEL a\nGET a\n' | ./kvstore_demo

# 2. 基础版回归
cd ../../netio/tcp_p2p
./build-base/tcp_p2p -l 9001
./build-base/tcp_p2p -c 127.0.0.1 9001

# 3. KV 版
./build-kv/tcp_p2p_kv -l 9001
printf 'SET a 1\nGET a\nDEL a\nGET a\n' | timeout 2 nc 127.0.0.1 9001

# 4. 错误路径
printf 'GET\nSET k\nGET k extra\nFOO x\n' | timeout 2 nc 127.0.0.1 9001

# 5. 多客户端
# 两个终端分别 nc 127.0.0.1 9001，一个 SET，另一个 GET

# 6. 内存检查
valgrind --leak-check=full ./build-kv/tcp_p2p_kv -l 9001
```

---

## 10. 相关文档

- 网络层与基础版 tcp_p2p：[`../../netio/tcp_p2p/README.md`](../../netio/tcp_p2p/README.md)
- netio 目录导航：[`../../netio/README.md`](../../netio/README.md)
- 仓库根目录：[`../../README.md`](../../README.md)
