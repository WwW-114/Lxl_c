# coroutine —— 基于 ucontext 的网络 I/O 协程框架

用 `ucontext` 保存 / 恢复协程栈，用 `epoll` 在 I/O 未就绪时挂起协程，最终得到一套
**单线程、非抢占式**的网络 I/O 框架：**代码按同步顺序写，底层 socket 全程非阻塞**。

本目录包含两套实现：

- **自己写的 C++17 框架**（`include/` + `src/` + `test/`，CMake 构建）——学习主线；
- **[`NtyCo-master/`](NtyCo-master/)**——保留的第三方 C 参考实现（作者 WangBoJing），
  Makefile 构建，用 `dlsym` hook libc、调度器共享栈，与自写框架形成对照。

---

## 目录内容

### 前置实验

| 文件 | 说明 |
|------|------|
| `SetJmp.c` | `setjmp` / `longjmp` 实现同一栈上的跳转，是理解「保存 / 恢复执行位置」的起点 |
| `setjmp` | 上面源文件在 Linux 下编译出的可执行文件 |
| `Ucontext.c` | `getcontext` / `makecontext` / `swapcontext` 的最小实验，并尝试过用 `dlsym` hook `read` / `write` |
| `ucontext` | 上面源文件在 Linux 下编译出的可执行文件 |
| `a.txt` | `Ucontext.c` 读写测试留下的数据文件 |

> `setjmp`、`ucontext` 没有扩展名，是 Linux 可执行文件（ELF），不是源码目录。

### 协程框架

```
coroutine/
├── CMakeLists.txt
├── include/co/
│   ├── Fiber.h             协程对象：独立栈、ucontext、状态机
│   ├── Scheduler.h         调度器：就绪队列 + epoll 等待表
│   └── Net.h               协程化的网络 I/O API
├── src/
│   ├── Fiber.cpp
│   ├── Scheduler.cpp
│   └── Net.cpp
└── test/
    └── test_coroutine.cpp  最小自测程序
```

### 第三方参考实现

`NtyCo-master/` 是 [WangBoJing/NtyCo](https://github.com/wangbojing/NtyCo) 的源码快照，
用 C 写成，核心同样是 `ucontext` + `epoll`，但接入 I/O 的方式不同：

| 维度 | 本目录自写框架 | NtyCo-master |
|------|----------------|--------------|
| 语言 / 构建 | C++17 / CMake | C / Makefile |
| 协程栈 | 每个协程独立栈 | 调度器共享栈，切换时 `memcpy` 保存 / 恢复 |
| I/O 接入 | 显式 `co::read` / `co::write` / `co::accept` | `dlsym(RTLD_NEXT, ...)` hook libc 同名函数 |
| 定时器 | 无 | sleeping 红黑树 + `epoll_wait` 超时 |
| 等待表 | `fd -> 协程` 的哈希映射 | 按 fd 排序的红黑树 |
| 多核 | 无 | 示例中 fork 多进程 + CPU 亲和 + 共享内存 |
| 示例 | 最小自测 | echo / HTTP / WebSocket / MySQL / Redis / webbench 压测 |

两套代码解决同一个问题，取舍不同：自写框架胜在结构清晰、显式 API 容易调试；
NtyCo 胜在功能完整、示例多，但错误处理和状态位实现有不少粗糙之处，阅读时注意甄别。
NtyCo 的构建方式与代码导读见 [`NtyCo-master/README.md`](NtyCo-master/README.md)。

---

## 核心设计

### 1. Fiber：ucontext 协程

- 每个协程有独立栈（默认 128 KiB）和自己的 `ucontext_t`。
- 创建时用 `getcontext` 初始化上下文，`makecontext` 把入口设为 `Fiber::trampoline`。
- 协程入口结束后显式 `swapcontext` 回调度器；`ctx_.uc_link` 也指向调度器上下文，作为兜底。
- 协程入口不通过 `makecontext` 传参，而是从 `Scheduler::current()->current_` 取当前
  协程对象，避免「把指针塞进 `makecontext` 可变参数」在 32 / 64 位下的可移植性问题。

状态机：

```
READY --调度--> RUNNING --yield()--> READY
                    |
                    +--waitFd()--> BLOCKED --epoll 事件--> READY
                    |
                    +--函数返回--> DEAD
```

### 2. Scheduler：单线程 round-robin

- `ready_`：就绪协程的双端队列，每次取队首执行。
- `waiters_`：`fd -> 协程` 映射。协程因 `EAGAIN` 调用 `waitFd()` 时注册进 epoll 并切回
  调度器；`epoll_wait` 返回后，对应协程重新进入 `ready_`。
- 唤醒采用「一次注册、一次唤醒」：事件到达后 `EPOLL_CTL_DEL`，下次等待时重新注册。
  这样不依赖 LT / ET 细节，也不会漏事件。
- `run()` 在「就绪队列为空且没有 fd 在等待」时自然退出；也可以由协程调用 `stop()`。

### 3. Net：显式协程化 I/O

没有劫持 libc 的 `read` / `write`（`Ucontext.c` 里的 hook 仅作原理实验），而是提供显式 API：

| API | 行为 |
|-----|------|
| `co::read` / `co::write` | 非阻塞调用；遇到 `EAGAIN` 挂起当前协程，就绪后重试 |
| `co::readn` / `co::writen` | 读满 / 写满指定字节数 |
| `co::accept` | 监听 fd 不可读时挂起 |
| `co::connect` / `co::connectTcp` | 非阻塞 connect，等待可写后检查 `SO_ERROR` |
| `co::listenTcp` | 创建非阻塞、带 `SO_REUSEADDR` 的监听 socket |
| `co::yield` | 主动让出 CPU |

显式 API 的好处：不会意外影响文件 I/O，不需要处理 `dlsym(RTLD_NEXT, ...)` 的递归，
调用点也能一眼看出「这里可能让出协程」。

---

## API 速览

```cpp
co::Scheduler sched;
sched.spawn([] {
    int fd = co::connectTcp("127.0.0.1", 9000);
    co::writen(fd, "hello", 5);

    char buf[16];
    co::readn(fd, buf, 5);
    ::close(fd);
});
sched.run();
```

---

## 构建与运行

仅支持 **Linux**（`ucontext` + `epoll`）。在 `coroutine/` 目录下：

```bash
cmake -S . -B build
cmake --build build -j
./build/test_coroutine
```

Windows 下请使用 WSL、虚拟机或远程 Linux 机器；MSVC 没有 `ucontext.h`，也没有 `epoll`。
当前这台开发机是 Windows，没有安装 WSL，所以实际编译与测试放在 **虚拟机中的 Linux** 上完成。

> 顶层 `CMakeLists.txt` 只构建自写框架（`co` 静态库 + `test_coroutine`），不会编译
> `NtyCo-master/`。NtyCo 使用它自己的 Makefile：

```bash
cd NtyCo-master
make          # 生成 objs/*.o 与 libntyco.a
make bin      # 生成 bin/ 下的示例（部分示例需要 OpenSSL / MySQL / hiredis）
```

---

## 测试程序

`test/test_coroutine.cpp` 只做两件事：

1. **协程切换**：3 个协程各执行 3 次 `yield`，检查计数器为 9。
2. **协程 echo**：同一个线程里跑一个协程服务器和一个协程客户端。服务器监听临时端口
   （`port = 0`，由内核分配），客户端连接后发送 `hello, coroutine!`，服务器原样写回；
   客户端校验收到的内容与发送内容一致，服务器在客户端关闭后读到 EOF。

预期输出（端口号每次不同，部分行的先后顺序可能略有差异）：

```
=== test 1: fiber switch ===
  fiber 0, step 0
  ...
counter = 9 (expected 9): OK

=== test 2: coroutine echo ===
  server listening on 127.0.0.1:xxxxx
  server accepted a connection
  client received 17 bytes: hello, coroutine!
  server finished (n = 0)
server ok: YES, echo ok: YES

=== ALL TESTS PASSED ===
```

---

## 已知边界与可扩展方向

- 单线程调度，没有跨线程唤醒；也不支持在协程里嵌套调用 `run()`。
- 同一个 fd 同一时刻只允许一个协程等待（重复等待返回 `EBUSY`）。
- 没有定时器：`epoll_wait` 目前无限等待。可以加 `timerfd` 或时间轮实现超时。
- 没有栈溢出保护；生产级实现可加 guard page（`mprotect`）。
- 想进一步接近「透明异步」，可以沿用 `Ucontext.c` 的思路，用 `dlsym` hook
  `read` / `write` / `accept` / `connect`，把所有 fd 设为非阻塞并接入同一个调度器。
  更完整的参考可以直接读 [`NtyCo-master/`](NtyCo-master/)：它采用的就是「hook + 共享栈 +
  红黑树定时器 + 多进程示例」这条路线。
