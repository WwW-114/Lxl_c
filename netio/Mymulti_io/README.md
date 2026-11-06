# Mymulti_io —— 手写练习版 Reactor

对照 [`../multi_io/`](../multi_io/) **凭自己的理解重写**的一份实现，用来检验是否真的掌握了
Reactor 那一套东西。**合上原码写，写不出来的地方就是没真懂的地方。**

> 文件的命名带 `My` 前缀（`MyTcpServer.h` 等），但内部的 `namespace net` 和类名
> （`TcpServer`、`EpollPoller` …）与 `multi_io` 完全一致——因为是照着同一套设计写的。
> 两个库彼此独立、不会被同时链接，所以不冲突。

---

## 当前状态

**尚在编写中，还不能编译通过。** 下面是阻止编译的代表性问题（写 README 时核对的结果）：

| 文件 | 问题 | 应该是什么 |
|------|------|-----------|
| `net/MySocket_Utils.h:21` | `using kInVaildSocket = -1;` | `using` 别名只能指向**类型**，`-1` 是表达式。要写成 `constexpr socket_t kInVaildSocket = -1;` |
| `net/MyEventPoller.h:22` | 用了 `kInValidSocket`（大写 V） | 与定义处的 `kInVaildSocket` 拼写不一致，会报未定义标识符 |
| `net/MyEventPoller.h` | `unsigned uint32_t None = 0x000;` | 两个类型说明符叠在一起。应为 `constexpr uint32_t None = 0;` |
| `net/MyEventPoller.h` | `uint32_t events = ev::Node;` | `ev::Node` 不存在，应为 `ev::None` |
| `net/MyEpollPoller.h` | `#include <epoll.h>` | 系统头文件是 `<sys/epoll.h>` |
| `net/MyEpollPoller.h` | `~EpollPoller() = override;` | 多了 `=`。虚析构重写写 `~EpollPoller() override;` |
| `net/MyPollPoller.h` | `size_t size const override{ ... }` | 少了括号且顺序错。应为 `size_t size() const override { ... }` |
| `net/MySelectPoller.h` | `std::vector<Events>& ready` | 类型名是 `Event`（单数），不是 `Events` |

**另外，本目录还没有 `CMakeLists.txt`**——补一个之后就能和 `multi_io` 一样编译：

```cmake
cmake_minimum_required(VERSION 3.16)
project(Mymulti_io LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra)

add_library(mynetio STATIC
    src/MyEventPoller.cpp
    src/MySelectPoller.cpp
    src/MyPollPoller.cpp
    src/MyEpollPoller.cpp
    src/MyTcpServer.cpp
)
target_include_directories(mynetio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

> 还缺一个 `examples/`：`multi_io` 靠 `echo_server` 验证 poller + TcpServer 是否真的能跑，
> 这里同样需要一个小程序来接上回调、跑起事件循环，否则写完没法验证。

---

## 文件清单

```
Mymulti_io/
├── net/                          头文件
│   ├── MySocket_Utils.h          socket_t 类型、错误码、非阻塞设置、监听套接字创建
│   ├── MyEventPoller.h           多路复用抽象基类 + ev:: 事件位掩码 + Event 结构
│   ├── MySelectPoller.h          select 后端
│   ├── MyPollPoller.h            poll 后端
│   ├── MyEpollPoller.h           epoll 后端（含 rearm，为 EPOLLONESHOT 预留）
│   └── MyTcpServer.h             Reactor 服务器
└── src/
    ├── MyEventPoller.cpp         工厂实现
    ├── MySelectPoller.cpp
    ├── MyPollPoller.cpp
    ├── MyEpollPoller.cpp
    └── MyTcpServer.cpp           事件循环与连接管理
```

规模：约 1188 行。`multi_io` 去掉 HTTP 层和 `examples/` 之后的对应部分是 1667 行，
两者量级相当——说明这份重写覆盖了同样的内容面。

---

## 与 multi_io 的对应关系

| 本目录 | multi_io | 说明 |
|--------|----------|------|
| `net/MySocket_Utils.h` | `include/net/SocketUtils.h` | `multi_io` 那份做了 Windows 适配，这里只针对 Linux，所以短得多 |
| `net/MyEventPoller.h` | `include/net/EventPoller.h` | 抽象接口 + `Backend` 枚举 + 工厂 |
| `net/MySelectPoller.h` 等 | `include/net/SelectPoller.h` 等 | 三个后端一一对应 |
| `net/MyTcpServer.h` | `include/net/TcpServer.h` | 接口形状一致：`Options` + 三个回调 + `send`/`closeConnection` |
| —— | `include/net/http/` | **本目录没有 HTTP 层**，只做到 Reactor |

`Mymulti_io` 只重写「多路复用 + Reactor」这两层，不包含 HTTP 解析。想覆盖 HTTP 那一层，
得另起一个练习。

---

## 写这个练习时值得盯住的点

这些是 `multi_io` 里踩过、也最容易在重写时再次踩到的地方
（完整版见 [`../multi_io/README.md`](../multi_io/README.md) 第十章）：

1. **`accept` 之后必须设非阻塞** —— ET 模式的硬性前提，漏了整个循环会卡死
2. **读到 `EAGAIN` 才算读完** —— LT/ET 都按这个写，一份代码两种模式都对
3. **应用层发送缓冲区** —— 非阻塞 `send` 可能只发出去一部分，剩下的必须缓存
4. **数据发完要取消对可写事件的关注** —— 否则 LT 下每轮都返回"可写"，CPU 100% 空转
5. **`epoll_ctl(DEL)` 必须在 `close()` 之前** —— 顺序反了，fd 号被复用时会指向别的连接
6. **回调里可能关掉连接** —— 回调返回后必须重新确认 fd 还有效
7. **忽略 `SIGPIPE`** —— Linux 上往已关闭的 socket 写会直接杀掉进程
8. **`SelectPoller` 要检查 `fd >= FD_SETSIZE`** —— 不检查的话 `FD_SET` 会越界写内存

---

## 参考

- 完整实现与逐项讲解：[`../multi_io/README.md`](../multi_io/README.md)
- 纯 C 的另一种组织方式（函数指针表而非虚函数）：[`../tcp_p2p/`](../tcp_p2p/)
