# component —— 池化结构与并发组件学习

记录用 C/C++ 实现和学习各种“池”的结构与并发模式。与 `netio/` 的网络 IO 主线不同，
这里关注的是**资源复用与并发调度**：线程池、内存池、连接池、对象池等。

池化的核心问题可以概括为：

- 资源创建/销毁的代价高，如何复用？
- 多个线程如何安全地共享同一个资源池？
- 池满、池空、关闭时，生产者和消费者如何等待与唤醒？
- 如何在不无限堆积的前提下做背压？
- 关闭时，排队中的资源和正在使用的资源分别怎么处理？

---

## 目录结构

```text
component/
├── threadpool/       [C11] 有界阻塞队列 + 固定工作线程池（Linux/pthread）
├── mempool/          [C11] 固定大小块内存池 + 侵入式空闲链表（Windows/Linux）
└── mysqlconnection/  [C++17] 同步 / 异步 MySQL 连接池（Linux/libmysqlclient）
```

---

## 当前子项目

| 目录 | 语言 | 平台 | 构建 | 说明 |
|------|------|------|------|------|
| [`threadpool/`](threadpool/) | C11 | Linux | CMake | 队列与线程池分离的有界阻塞线程池：mutex + 条件变量、背压、优雅/立即关闭、pending 统计 |
| [`mempool/`](mempool/) | C11 | Windows / Linux | CMake | 固定大小块内存池：侵入式空闲链表、chunk 批量申请、对齐、扩容上限、统计；基础版单线程 |
| [`mysqlconnection/`](mysqlconnection/) | C++17 | Linux | CMake | 同步 + 异步 MySQL 连接池：RAII 借还、健康检查、空闲回收、有界任务队列、future/回调、统计；当前提供静态库 |

详细设计、API、并发分析和测试方法见各自的 README：

- [`threadpool/README.md`](threadpool/README.md)
- [`mempool/README.md`](mempool/README.md)
- [`mysqlconnection/README.md`](mysqlconnection/README.md)

---

## 快速开始

### threadpool

```bash
cd threadpool
cmake -S . -B build
cmake --build build -j
./build/threadpool_demo
```

预期：4 个工作线程并发处理 12 个任务，队列容量为 4，最终累计完成 12 个任务并优雅关闭。

### mempool

```bash
cd mempool
cmake -S . -B build
cmake --build build -j
./build/mempool_demo
```

预期：请求块大小 32、每 chunk 8 块、最多 2 个 chunk；演示容量从 8 扩容到 16，
释放后按 LIFO 复用，达到上限后 `alloc` 返回 `NULL`，全部释放后统计归零。

### mysqlconnection

```bash
cd mysqlconnection
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

构建产物是静态库 `build/libmysqlpool.a`，需要本机可访问的 MySQL/MariaDB 服务端
以及 `libmysqlclient` / `libmariadb` 开发包。当前子项目只提供核心库，最小同步/
异步使用示例、数据库准备和手工验证步骤见
[`mysqlconnection/README.md`](mysqlconnection/README.md)。

> `mempool` 不依赖 pthread，Windows 下也可以用 CMake/Visual Studio 构建；
> `threadpool` 与 `mysqlconnection` 依赖 POSIX pthread，建议在 Linux 虚拟机或
> WSL 中运行。

---

## 设计关注点

### threadpool

- **模块边界**：任务队列（`task_queue`）与线程池（`threadpool`）分离，
  队列只认识 `void *`，可独立复用。
- **生产者-消费者**：`mutex` + `not_empty` / `not_full` 两个条件变量。
- **背压**：有界队列满时阻塞提交者，而不是无限堆积任务。
- **完成等待**：`pending` 统计“已提交但未完成”，`all_idle` 条件变量唤醒 `wait`。
- **关闭语义**：优雅关闭排空队列；立即关闭丢弃排队任务，但不取消正在执行的任务。
- **锁顺序**：队列锁与线程池锁不嵌套，避免死锁。
- **同步原语选择**：阻塞队列使用 `mutex + 条件变量`；自旋锁只适合极短、
  不可睡眠的临界区，不适合等待任务/等待空位的场景。

### mempool

- **模块边界**：`free_list` 只负责空闲块的 LIFO 链表，`mempool` 负责 chunk 扩容、
  对齐、生命周期和统计；`mempool.h` 不暴露 `free_list`。
- **批量申请**：一个 chunk 只调用一次 `malloc`，内部切成固定大小块；
  空闲链表非空时，分配路径不再调用 `malloc`。
- **侵入式空闲链表**：空闲块的前几个字节存放 `next`，没有每块头部；
  块分配出去后整块都是负载。
- **对齐**：块大小和块区偏移按 `_Alignof(max_align_t)` 向上取整。
- **统计不变式**：`used + available == capacity`，容量与 chunk 数同步增长。
- **基础版单线程**：线程安全版本作为后续扩展，不把锁和空闲链表耦合在一起。

### mysqlconnection

- **模块边界**：`MysqlConn` 只负责一条物理连接的建立、查询和重连；
  `SyncConnectionPool` 只负责借还、等待、超时和空闲回收；
  `AsyncConnectionPool` 只在同步池之上增加任务队列和工作线程，不重复实现
  连接管理。
- **同步池**：`mutex` + 条件变量 + 空闲 FIFO。池满时阻塞等待；需要扩容时先在
  锁内预留名额，再在锁外执行 `mysql_real_connect()`，避免并发扩容超过
  `max_size`。
- **RAII 借还**：`ConnGuard` 析构自动归还，异常展开也不会漏还；借出前可选
  `mysql_ping` 健康检查，失败时先关闭并按保存的配置重连，重连仍失败才销毁槽位。
- **维护与关闭**：后台线程按 `idle_timeout` / `max_lifetime` 回收空闲连接并
  补足 `min_size`；关闭时先停止借出，再等待在途连接归还，最后统一关闭物理连接。
- **异步池**：libmysqlclient 的普通 C API 是阻塞的，异步池用固定 worker +
  有界任务队列 + `std::future` 把阻塞 I/O 移出调用线程；队列满时快速失败，
  形成背压。
- **关闭语义**：异步池先拒绝新任务，再排空队列、join worker，最后关闭同步池。
- **易错点**：重连会丢会话状态；`ConnGuard` 不能比池活得久；异步回调运行在
  worker 上，不能长阻塞，也不能在回调里调用 `shutdown()`。

---

## 环境要求

- **threadpool**：Linux（pthread）、CMake ≥ 3.16、GCC/Clang、C11。
- **mempool**：Windows / Linux、CMake ≥ 3.16、支持 C11 的编译器；无外部依赖。
- **mysqlconnection**：Linux（pthread）、CMake ≥ 3.16、C++17、GCC/Clang，
  `libmysqlclient` 或 `libmariadb` 开发包，以及可访问的 MySQL/MariaDB 服务端。
- 当前开发机是 Windows，没有 WSL；threadpool 与 mysqlconnection 在 Linux 虚拟机
  中编译和验证，mempool 可在 Windows 或 Linux 下构建。

---

## 后续方向

- 对象池：在固定块内存池之上增加构造/析构回调。
- 连接池增强：prepared statement、流式结果、多结果集、事务守卫、
  动态扩缩容、指标导出。
- 非阻塞 MySQL 驱动 / 协程化异步池：用事件循环替代固定 worker 阻塞等待。
- 动态线程池：按队列长度扩缩容，空闲线程超时退出。
- 线程安全内存池：池级 mutex 或 per-thread cache，回答“多线程共享资源池”的问题。
- 内存池 size class：多个固定块池组合，支持一组常用尺寸。
- 自旋锁 / 无锁队列对照实验：只实现 `try_push/try_pop`，与阻塞队列比较吞吐和 CPU 占用。
- 任务优先级、延迟任务、任务取消与 cleanup 回调。
