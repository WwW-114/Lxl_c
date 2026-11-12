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
└── mempool/          [C11] 固定大小块内存池 + 侵入式空闲链表（Windows/Linux）
```

---

## 当前子项目

| 目录 | 语言 | 平台 | 构建 | 说明 |
|------|------|------|------|------|
| [`threadpool/`](threadpool/) | C11 | Linux | CMake | 队列与线程池分离的有界阻塞线程池：mutex + 条件变量、背压、优雅/立即关闭、pending 统计 |
| [`mempool/`](mempool/) | C11 | Windows / Linux | CMake | 固定大小块内存池：侵入式空闲链表、chunk 批量申请、对齐、扩容上限、统计；基础版单线程 |

详细设计、API、并发分析和测试方法见各自的 README：

- [`threadpool/README.md`](threadpool/README.md)
- [`mempool/README.md`](mempool/README.md)

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

> `mempool` 不依赖 pthread，Windows 下也可以用 CMake/Visual Studio 构建；
> `threadpool` 依赖 POSIX pthread，建议在 Linux 虚拟机或 WSL 中运行。

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

---

## 环境要求

- **threadpool**：Linux（pthread）、CMake ≥ 3.16、GCC/Clang、C11。
- **mempool**：Windows / Linux、CMake ≥ 3.16、支持 C11 的编译器；无外部依赖。
- 当前开发机是 Windows，没有 WSL；依赖 pthread 的 threadpool 在 Linux 虚拟机中
  编译和测试，mempool 可在 Windows 或 Linux 下构建。

---

## 后续方向

- 对象池：在固定块内存池之上增加构造/析构回调。
- 连接池：连接复用、健康检查、超时回收。
- 动态线程池：按队列长度扩缩容，空闲线程超时退出。
- 线程安全内存池：池级 mutex 或 per-thread cache，回答“多线程共享资源池”的问题。
- 内存池 size class：多个固定块池组合，支持一组常用尺寸。
- 自旋锁 / 无锁队列对照实验：只实现 `try_push/try_pop`，与阻塞队列比较吞吐和 CPU 占用。
- 任务优先级、延迟任务、任务取消与 cleanup 回调。
