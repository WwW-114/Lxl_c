# threadpool —— 队列与线程池分离的 C 语言线程池

用 **C11 + POSIX pthread** 实现的小型线程池。重点不是“写一个能跑的多线程程序”，
而是把**有界阻塞队列**和**线程池**拆成两个独立模块，并把生产者-消费者、背压、
关闭语义和统计计数讲清楚。

| 项 | 说明 |
|----|------|
| 语言 / 平台 | C11 / Linux（pthread） |
| 构建 | CMake ≥ 3.16 |
| 测试环境 | Linux 虚拟机（Windows 宿主机无 WSL，代码在虚拟机中编译运行） |
| 演示规模 | 4 个工作线程，队列容量 4，12 个任务 |

---

## 1. 特性

- **有界 FIFO 任务队列**：队列满时阻塞提交者，形成背压，而不是无限堆积任务。
- **固定工作线程组**：多个 worker 从同一个队列竞争取任务；任务在锁外执行。
- **mutex + 条件变量**：`not_empty` 唤醒消费者，`not_full` 唤醒生产者。
- **两种关闭语义**：
  - 优雅关闭（`graceful != 0`）：排空队列中剩余任务后退出；
  - 立即关闭（`graceful == 0`）：丢弃排队任务，正在执行的任务仍执行完。
- **`threadpool_wait`**：等待所有已提交任务完成，不依赖轮询。
- **统计信息**：排队数、未完成数（排队 + 执行中）、累计完成数。
- **队列可独立复用**：`task_queue` 只认识 `void *`，不依赖线程池。

不包含：动态扩缩容、任务优先级、任务取消、无锁队列。

---

## 2. 目录结构

```text
threadpool/
├── task_queue.h      队列公开 API
├── task_queue.c      有界阻塞队列实现（独立模块）
├── threadpool.h      线程池公开 API
├── threadpool.c      工作线程、任务封装、pending/completed
├── main.c            演示程序
├── CMakeLists.txt    CMake 构建
├── README.md         本文档
└── build/            CMake 构建产物（可删除）
```

依赖方向：

```text
main.c
  └── threadpool.h / threadpool.c
        └── task_queue.h / task_queue.c
```

- `threadpool.c` 只调用 `task_queue_*` 的公开函数，不直接操作队列节点。
- `task_queue.c` 不包含 `threadpool.h`，不知道“任务”是什么。
- 两个模块之间唯一的生命周期耦合是 `task_queue_create()` 的 `free_fn` 回调，
  用于释放被队列丢弃的元素。

---

## 3. 架构

```text
                  main.c
                    │ threadpool_add(pool, fn, arg)
                    ▼
        ┌───────────────────────────┐
        │ threadpool                │
        │  pending / completed      │
        │  mutex + all_idle         │
        └───────────┬───────────────┘
                    │ task_queue_push(task_t *)
                    ▼
        ┌───────────────────────────┐
        │ task_queue                │
        │  mutex                    │
        │  not_empty / not_full     │
        │  head → ... → tail        │
        └───────────┬───────────────┘
                    │ task_queue_pop(task_t *)
                    ▼
        worker[0]  worker[1]  ...  worker[N-1]
                    │
                    ├── task->fn(task->arg)
                    ├── free(task_t)
                    └── pending--, completed++, 必要时广播 all_idle
```

职责划分：

| 模块 | 负责 |
|------|------|
| `task_queue` | 元素在哪里排队、满了怎么办、空了怎么办、关闭后怎么排空或丢弃 |
| `threadpool` | 开几个线程、任务怎么封装、何时算全部完成、优雅/立即关闭 |
| `main.c` | 提交演示任务、观察输出 |

---

## 4. 核心数据结构

### 4.1 队列 `task_queue`

```c
struct task_queue {
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;    /* 生产者等待：size < capacity */
    pthread_cond_t  not_empty;   /* 消费者等待：size > 0 */

    queue_node_t *head;
    queue_node_t *tail;
    size_t size;
    size_t capacity;

    int closed;                  /* 1 = 不再接收新元素 */
    void (*free_fn)(void *);     /* 丢弃元素时的析构回调，可为 NULL */
};
```

节点只保存 `void *data`：

```c
typedef struct queue_node {
    void *data;
    struct queue_node *next;
} queue_node_t;
```

队列不解释元素内容，因此同一个队列既可用于任务指针，也可用于其他指针类型。

### 4.2 线程池 `threadpool`

```c
struct threadpool {
    task_queue_t *queue;

    pthread_t *thread;
    int thread_num;

    pthread_mutex_t mutex;       /* 只保护 pending / completed */
    pthread_cond_t  all_idle;    /* pending == 0 时广播 */

    unsigned long pending;       /* 已提交但未完成：排队 + 执行中 */
    unsigned long completed;     /* 已完成 */
};
```

任务节点由线程池私有：

```c
typedef struct {
    task_fn_t func;
    void *arg;
    threadpool_t *owner;   /* 被队列丢弃时，用它回退 pending */
} task_t;
```

`task_t *` 以 `void *` 的形式进入队列；worker 取出后转回 `task_t *`。

---

## 5. API

### 5.1 队列 `task_queue.h`

| 函数 | 语义 |
|------|------|
| `task_queue_create(capacity, free_fn)` | 创建容量为 `capacity` 的队列；`capacity` 必须 > 0；`free_fn` 可为 `NULL` |
| `task_queue_destroy(q)` | 销毁队列，释放所有剩余元素；不能与其他操作并发 |
| `task_queue_push(q, data)` | 阻塞入队；队列满时等待；关闭后返回 `TQ_CLOSED` |
| `task_queue_try_push(q, data)` | 非阻塞入队；满时返回 `TQ_FULL` |
| `task_queue_pop(q, &data)` | 阻塞出队；队列空时等待；关闭且为空返回 `TQ_CLOSED` |
| `task_queue_try_pop(q, &data)` | 非阻塞出队；空时返回 `TQ_EMPTY`，关闭且为空返回 `TQ_CLOSED` |
| `task_queue_close(q, graceful)` | 关闭队列并唤醒所有等待者；可重复调用 |
| `task_queue_is_closed(q)` | 是否已关闭 |
| `task_queue_size(q)` | 当前排队元素数 |
| `task_queue_capacity(q)` | 队列容量 |

状态码：

| 值 | 含义 |
|----|------|
| `TQ_OK` | 成功 |
| `TQ_FULL` | 队列已满（仅 `try_push`） |
| `TQ_EMPTY` | 队列为空（仅 `try_pop`） |
| `TQ_CLOSED` | 已关闭，且没有可取元素 |
| `TQ_NOMEM` | 节点分配失败 |
| `TQ_INVAL` | 参数非法 |

### 5.2 线程池 `threadpool.h`

| 函数 | 语义 |
|------|------|
| `threadpool_create(thread_num, queue_num)` | 创建工作线程和队列；参数 ≤ 0 时分别取 1 / 64 |
| `threadpool_add(pool, fn, arg)` | 提交任务；队列满时阻塞；关闭或分配失败返回 -1 |
| `threadpool_wait(pool)` | 等待 `pending == 0`；应在提交完成后调用 |
| `threadpool_destroy(pool, graceful)` | `graceful != 0` 排空队列；`graceful == 0` 丢弃排队任务 |
| `threadpool_pending(pool)` | 已提交但未完成（排队 + 执行中） |
| `threadpool_completed(pool)` | 累计已完成 |
| `threadpool_queued(pool)` | 仅排队中（不含执行中） |

---

## 6. 并发设计

### 6.1 队列的等待与唤醒

入队：

```c
pthread_mutex_lock(&q->mutex);

while (q->size >= q->capacity && !q->closed)
    pthread_cond_wait(&q->not_full, &q->mutex);

if (q->closed) {
    pthread_mutex_unlock(&q->mutex);
    free(node);
    return TQ_CLOSED;
}

尾插节点; ++q->size;
pthread_cond_signal(&q->not_empty);
pthread_mutex_unlock(&q->mutex);
```

出队：

```c
pthread_mutex_lock(&q->mutex);

while (q->size == 0 && !q->closed)
    pthread_cond_wait(&q->not_empty, &q->mutex);

if (q->size == 0) {
    pthread_mutex_unlock(&q->mutex);
    return TQ_CLOSED;
}

取队头; --q->size;
pthread_cond_signal(&q->not_full);
pthread_mutex_unlock(&q->mutex);
```

要点：

- **用 `while` 而不是 `if`**：条件变量允许虚假唤醒。
- 入队 `signal(not_empty)`：新增一个任务，只唤醒一个消费者。
- 出队 `signal(not_full)`：空出一个位置，只唤醒一个生产者。
- 关闭时 `broadcast` 两个条件变量：唤醒所有可能阻塞的 worker 和提交者。

### 6.2 为什么队列不用自旋锁

这里的等待是**阻塞语义**，不是忙等：

1. `pthread_cond_wait` 只能配合 `pthread_mutex_t`，POSIX 没有“自旋锁版条件变量”。
2. 队列空时工作线程必须睡眠，队列满时提交者也必须睡眠；自旋会白白占用 CPU。
3. 无竞争时 `pthread_mutex` 走 futex 快速路径，开销已经很低。
4. 在单核或 CPU 超卖的虚拟机上，自旋锁可能让持锁线程得不到调度，反而更慢。
5. **自旋锁不等于无锁**。如果要追求 lock-free，应该用 C11 原子 CAS 实现队列，
   而不是 `pthread_spinlock_t`。

如果只是想练自旋锁，建议单独写一个只提供 `try_push/try_pop` 的环形队列，
用 `pthread_spinlock_t` 保护，再用多线程轮询测试；不要替换这里的阻塞队列。

### 6.3 `pending` 计数

`pending` 表示“已提交但尚未完成”的任务数，包含**排队中**和**执行中**两部分。

- `threadpool_add()` 在调用 `task_queue_push()` **之前**先 `pending++`；
- worker 正常执行完任务后：`pending--`、`completed++`；
- 任务被队列丢弃时（立即关闭或入队失败）：`pending--`，但 `completed` 不增加；
- `pending` 减到 0 时广播 `all_idle`，唤醒 `threadpool_wait()`。

为什么先 `pending++` 再入队？否则存在“任务已经进入队列，但还没计入 pending”的窗口，
`threadpool_wait()` 可能在这个窗口里看到 `pending == 0` 并提前返回。

### 6.4 锁顺序：队列锁与池锁不嵌套

- `threadpool_add()`：先拿 `pool->mutex` 做 `pending++`，**释放后**再调用 `task_queue_push()`；
- worker：先在队列锁内 `pop`，**释放队列锁后**才执行任务，最后才拿 `pool->mutex` 更新计数；
- `task_queue_close()` / `task_queue_destroy()`：摘下链表、**释放队列锁后**，
  才通过 `free_fn`（`task_release()`）去拿 `pool->mutex`。

因此不存在“持有队列锁再拿池锁”与反向获取形成的死锁。

---

## 7. 关闭语义

### 7.1 优雅关闭 `threadpool_destroy(pool, 1)`

1. `task_queue_close(queue, 1)`：设置 `closed`，广播 `not_empty` / `not_full`，
   但**不丢弃**队列中的元素；
2. worker 继续 `pop` 剩余任务；队列排空后 `pop` 返回 `TQ_CLOSED`，worker 退出；
3. `pthread_join` 所有 worker；
4. `task_queue_destroy(queue)`：销毁队列的 mutex/cond；
5. 销毁线程池的 `all_idle` / `mutex`，释放内存。

### 7.2 立即关闭 `threadpool_destroy(pool, 0)`

1. `task_queue_close(queue, 0)`：设置 `closed`，把整个链表摘下，`size = 0`，广播；
2. 锁外对每个被丢弃的任务调用 `task_release()`：
   释放 `task_t`、`pending--`，必要时广播 `all_idle`；
3. **正在执行的任务不会被取消**，worker 执行完后退出；
4. `join` 所有 worker；
5. `task_queue_destroy`，销毁线程池的同步对象并释放内存。

### 7.3 `close` 与 `destroy` 的区别

- `task_queue_close()` 只负责“关闭 + 唤醒”，**不销毁 mutex/cond**；
  关闭后 worker 仍需要 `pop` 来排空队列。
- `task_queue_destroy()` 才负责释放剩余元素并销毁同步对象。
- 这是本项目调试时最容易踩的坑：如果在 `close()` 里 `pthread_mutex_destroy()`，
  后续 worker 再调用 `task_queue_pop()` 就会访问已销毁的锁。

---

## 8. 任务参数的所有权

线程池只管理 `task_t` 节点，不解释 `arg`：

- 正常执行：worker 调用 `fn(arg)`，然后 `free(task_t)`；
  `arg` 由任务函数负责释放（演示程序中 `job_run()` 自己 `free(job)`）。
- 入队失败：`threadpool_add()` 调用 `task_release()` 释放 `task_t`；`arg` 仍由提交者处理。
- 立即关闭丢弃任务：队列释放 `task_t`，但**不会释放 `arg`**。
  如果 `arg` 是堆内存，会泄漏。

演示程序使用优雅关闭，所以不会出现这个问题。要彻底解决，可以给任务增加一个
`cleanup(arg)` 回调，在 `task_release()` 和任务函数中统一释放参数。

---

## 9. 使用约定与限制

- `threadpool_destroy()` 必须在所有生产者停止调用 `threadpool_add()` 之后调用，
  且不能与 `threadpool_wait()` 并发，否则可能出现 use-after-free。
- 不要重复调用 `threadpool_destroy()`。
- 不要在任务函数内部调用 `threadpool_destroy()`：会 `pthread_join` 自己而死锁。
- 不要在任务函数内部调用 `threadpool_wait()`：`pending` 包含当前任务，永远等不到 0。
- 不建议在任务函数中向同一个线程池提交任务：队列满时所有 worker 可能互相等待。
- `task_queue_destroy()` 不能与其他队列操作并发；线程池会在 `join` 完 worker 后调用它。
- 线程池关闭后，`threadpool_add()` 返回 -1；队列是否已关闭可通过
  `task_queue_is_closed()` 查询。

---

## 10. 构建与运行

### 10.1 环境

- Linux（pthread）
- CMake ≥ 3.16
- GCC / Clang，C11

### 10.2 普通构建

```bash
cd threadpool
cmake -S . -B build
cmake --build build -j
./build/threadpool_demo
```

CTest：

```bash
ctest --test-dir build --output-on-failure
```

### 10.3 ThreadSanitizer

```bash
cmake -S . -B build-tsan -DTHREADPOOL_TSAN=ON
cmake --build build-tsan -j
./build-tsan/threadpool_demo
```

如果 TSan 在新内核上报 `unexpected memory mapping`：

```bash
setarch "$(uname -m)" -R ./build-tsan/threadpool_demo
```

### 10.4 valgrind

```bash
cmake -S . -B build
cmake --build build -j
valgrind --leak-check=full --show-leak-kinds=all ./build/threadpool_demo
```

### 10.5 平台说明

代码依赖 pthread 和 `usleep`，面向 Linux/POSIX。
原生 Windows + MSVC 不能直接构建；请在 Linux 虚拟机或 WSL 中编译运行。

---

## 11. 预期输出

```text
=== C 线程池演示（队列与线程池分离） ===
工作线程: 4，队列容量: 4，任务总数: 12

主线程: 提交任务 #01，队列中 0 个，未完成 1 个
  [线程 140234...] 开始任务 #01（预计 100 ms）
主线程: 提交任务 #02，队列中 0 个，未完成 2 个
...
主线程: 提交任务 #09，队列中 4 个，未完成 8 个
...
主线程: 等待剩余任务执行完毕...
主线程: 全部完成，累计完成 12 个任务
主线程: 线程池已优雅关闭
```

观察点：

1. **任务输出交错**：4 个 worker 并发执行，线程 ID 每次不同。
2. **队列中 N 个 ≤ 4**：队列满后，主线程的提交会停顿，这就是背压。
3. **未完成 = 排队 + 执行中**：最多可能达到 `队列容量 + 工作线程数`。
4. **累计完成 12 个**：所有任务都执行完毕。
5. 进程正常退出；TSan / valgrind 无报错、无泄漏。

---

## 12. 学习点

1. **模块边界**：队列只认识 `void *`，线程池只使用队列 API，CMake 用两个静态库
   `task_queue`、`threadpool` 体现依赖方向。
2. **条件变量必须配 `while`**：防止虚假唤醒。
3. **`signal` 与 `broadcast` 的选择**：单个任务/单个空位用 `signal`，
   关闭或全部完成用 `broadcast`。
4. **背压**：有界队列让提交者在队列满时等待，避免任务无限堆积。
5. **`pending` 计数**：先计数再入队，解决 `wait` 的检查窗口问题。
6. **关闭协议**：先置 `closed`、再广播、再 `join`；`close` 不销毁同步对象。
7. **锁顺序**：队列锁和池锁不嵌套，避免死锁。
8. **mutex vs 自旋锁**：阻塞等待用条件变量；自旋锁适合极短、不可睡眠的临界区。

---

## 13. 可继续扩展

1. **任务析构回调**：给 `task_t` 增加 `void (*cleanup)(void *)`，
   解决立即关闭时 `arg` 泄漏的问题。
2. **动态线程池**：维护最小/最大线程数，根据 `queue_size` 扩容、空闲缩容。
3. **优先级队列 / 延迟任务**：使用优先堆或 `pthread_cond_timedwait`。
4. **环形缓冲区队列**：减少每个任务的 `malloc/free`，比较缓存局部性。
5. **自旋锁 / 无锁队列实验**：只实现 `try_push/try_pop`，与阻塞队列做吞吐对比。
6. **运行时指标**：增加任务等待时间、执行时间、worker 利用率等统计。

---

## 14. 常见问题

| 现象 | 排查方向 |
|------|----------|
| `undefined reference to pthread_create` | 链接时漏了 `Threads::Threads` / `-pthread` |
| `threadpool_create` 返回 NULL | `pthread_*_init` **成功返回 0**，检查判断是否写反；同时确认 `thread_num` 已保存 |
| `destroy` 卡死 | `task_queue_close()` 是否广播并解锁；是否误在 close 中销毁 mutex；`thread_num` 是否为 0 |
| 只有一个线程干活 | 是否在持有锁时执行了 `task->fn()` |
| valgrind 报泄漏 | 立即关闭时 `arg` 未释放；使用优雅关闭或增加 cleanup 回调 |
| 程序随机崩溃 | `destroy` 与 `add` / `wait` 并发，或重复 `destroy` |
| TSan 报 `unexpected memory mapping` | 用 `setarch "$(uname -m)" -R` 启动程序 |

---

## 15. 相关文档

- 父目录导航：[`../README.md`](../README.md)
- 队列模块：[`task_queue.h`](task_queue.h) / [`task_queue.c`](task_queue.c)
- 线程池模块：[`threadpool.h`](threadpool.h) / [`threadpool.c`](threadpool.c)
- 演示程序：[`main.c`](main.c)
- 构建脚本：[`CMakeLists.txt`](CMakeLists.txt)
