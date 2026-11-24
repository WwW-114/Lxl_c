# project —— 综合项目

与 `netio/` 的网络 IO 练习不同，这里放的是把多个知识点组合起来的完整小项目。
目前包含一个项目：

| 目录 | 语言 | 平台 | 构建 | 一句话说明 | 详细文档 |
|------|------|------|------|-----------|---------|
| [`kvstore/`](kvstore/) | C11 + C++17 | 核心跨平台；网络版 Linux | CMake（随 tcp_p2p 构建） | 行文本协议的内存 KV 存储：`SET/GET/MOD/DEL`；存储层可在可变长数组 / 红黑树 / 哈希表之间由 CMake 条件编译选择；既可独立 stdin demo，也可接入 `netio/tcp_p2p` 的 epoll reactor 变成多客户端 TCP KV 服务器 | [README.md](kvstore/README.md) |

---

## kvstore 与 tcp_p2p 的关系

`kvstore` 的核心是纯协议 + 存储，不知道 socket 的存在：

```
project/kvstore/
├── kvstore.c / kvstore.h          命令解析、参数校验、响应生成（C11）
├── kvstore_table.cpp / .h         数据表：数组 / 红黑树 / 哈希条件编译（C++17 实现，C 接口）
├── MyRedBlackTree.hpp             红黑树模板（rbtree 后端）
├── kvstore_config.h               长度、表项、客户端数量上限
├── kvstore_demo.c                 stdin 独立 demo
└── CMakeLists.txt                 后端选项 + kvstore_core 静态库 + demo
```

网络接入点位于 `netio/tcp_p2p/`，通过 **条件编译** 复用同一份 reactor/peer：

```
netio/tcp_p2p/
├── reactor.c / reactor.h          epoll 封装，未修改
├── peer.c / peer.h                基础版单连接；KV 版多客户端 + 行分派
├── main.c                         基础交互；KV 版服务器命令
└── CMakeLists.txt                 基础目标 + 可选 tcp_p2p_kv 目标
```

- 不定义 `TCP_P2P_KVSTORE`：构建原来的 `tcp_p2p`，行为与改造前一致。
- 定义 `TCP_P2P_KVSTORE`：构建 `tcp_p2p_kv`，启用 KV 协议、多客户端和
  KV 专用的发送失败策略。

---

## 快速构建与运行

```bash
cd <仓库根目录>/netio/tcp_p2p

# 基础版
cmake -S . -B build-base -DTCP_P2P_KVSTORE=OFF
cmake --build build-base -j

# KV 服务器版（默认 array 后端）
cmake -S . -B build-kv -DTCP_P2P_KVSTORE=ON
cmake --build build-kv -j

# 指定存储后端：array（默认）/ rbtree / hash
cmake -S . -B build-kv-rbtree -DTCP_P2P_KVSTORE=ON -DKVSTORE_TABLE_BACKEND=rbtree
cmake --build build-kv-rbtree -j

./build-kv/tcp_p2p_kv -l 9001
```

另一个终端：

```bash
printf 'SET a 1\nGET a\n' | timeout 2 nc 127.0.0.1 9001
```

预期：

```text
+OK
+VALUE 1
```

独立验证 KV 核心（不需要网络；kvstore 没有独立顶层 CMake，构建入口仍是 tcp_p2p）：

```bash
cd <仓库根目录>/netio/tcp_p2p
cmake -S . -B build-kv-array -DTCP_P2P_KVSTORE=ON -DKVSTORE_TABLE_BACKEND=array
cmake --build build-kv-array -j
printf 'SET a 1\nGET a\n' | ./build-kv-array/kvstore/kvstore_demo
```

完整协议、边界行为、内存与并发说明见
[`kvstore/README.md`](kvstore/README.md)。
