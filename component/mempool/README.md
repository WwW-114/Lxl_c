# mempool —— 固定大小块内存池（C11）

用 C11 实现的小型固定大小块内存池。重点不是“写一个能跑的分配器”，而是把
**侵入式空闲链表**和**内存池管理**拆成两个独立模块，并把创建、分配、回收、
扩容和销毁的边界讲清楚。

| 项 | 说明 |
|----|------|
| 语言 / 标准 | C11，无外部依赖 |
| 平台 | Windows / Linux |
| 构建 | CMake ≥ 3.16 |
| 线程安全 | 基础版单线程，不加锁 |
| 块类型 | 固定大小块，不支持变长分配 |
| 空闲结构 | 侵入式 LIFO 单链表 |
| 扩容 | 按 chunk 批量 `malloc`，`max_chunks` 限制上限 |

---

## 1. 特性与非目标

**特性**

- 固定大小块，空闲块用侵入式 LIFO 单链表管理；空闲链表非空时分配 O(1)，
  回收 O(1)，扩容建链的开销均摊到该 chunk 的每个块；
- 按 chunk 批量申请内存，一个 chunk 只调用一次 `malloc`；
- 空闲链表非空时，分配路径完全不调用 `malloc`；
- `max_chunks` 控制 chunk 上限，0 表示不限制；
- 块大小和数据区偏移都按基本对齐向上取整；
- 提供块大小、当前容量、已用、空闲、chunk 数等统计；
- 空闲链表可独立编译、独立测试，不依赖内存池；
- 可选的 `mempool_contains()` 调试辅助，用于检查指针是否落在池内。

**非目标**

- 不支持任意大小分配、size class、split/merge；
- 不检测 double free，也不默认校验外部指针；
- 不提供对象构造/析构回调；
- 不把空闲 chunk 归还给操作系统；
- 不支持多线程并发分配；
- 不支持超对齐类型（例如 32/64 字节 SIMD）。

---

## 2. 目录结构与依赖

```text
mempool/
├── free_list.h        空闲链表公开接口
├── free_list.c        侵入式 LIFO 空闲链表
├── mempool.h          内存池公开接口（不透明句柄）
├── mempool.c          chunk 管理、扩容、生命周期、统计
├── main.c             演示程序，只使用 mempool 公开 API
├── CMakeLists.txt     构建脚本
├── README.md          本文档
└── mempool-v1.c       旧版单页原型，保留对照，不参与构建
```

依赖方向：

```text
main.c
  └── mempool.h / mempool.c
        ├── mp_chunk_t（mempool.c 私有）
        └── free_list.h / free_list.c
```

模块边界：

- `free_list.c` 不包含 `mempool.h`，不调用 `malloc/free`，不知道块大小和 chunk；
- `mempool.c` 只通过 `free_list_*` 函数操作空闲链表，不直接操作 `head`/`count`；
- `mempool.h` 不包含 `free_list.h`，`main.c` 看不到 `free_list`；
- CMake 用两个静态库 `free_list`、`mempool` 固化依赖方向；
- `mempool-v1.c` 有独立的 `main`，不要用 `file(GLOB ...)` 把它编进当前目标。

---

## 3. 核心数据结构与内存布局

### 3.1 free list：侵入式空闲链表

```c
typedef struct free_node_s {
  struct free_node_s *next;
} free_node_t;

typedef struct free_list_s {
  free_node_t *head;
  size_t count;
} free_list_t;
```

空闲块本身的前 `sizeof(free_node_t)` 字节用来存放 `next`，因此：

- 每个块必须至少 `sizeof(free_node_t)` 字节；
- 每个块必须按 `free_node_t` 对齐；
- free list 不拥有内存，不调用 `malloc/free`；
- push/pop 都是 O(1)，`count` 让统计也是 O(1)。

空闲块布局：

```text
+-------------------+------------------------------+
| next 指针         | 其余空闲空间（旧数据残留）   |
+-------------------+------------------------------+
^
块起始地址
```

块被分配出去后，用户可以使用整个 `mempool_block_size()` 字节，没有额外的每块头部。

### 3.2 chunk：一次批量申请

```c
typedef struct mp_chunk {
  struct mp_chunk *next;   /* 所有 chunk 串成链表，供销毁和 contains 使用 */
  size_t count;            /* 本 chunk 的块数 */
} mp_chunk_t;
```

每个 chunk 用**一次 `malloc`** 同时分配头部、对齐 padding 和块区：

```text
chunk 基址（malloc 返回，满足基本对齐）
+------------+---------------+-----------+---------+---------+ ... +-----------+
| next       | count         | padding   | block 0 | block 1 |     | block n-1 |
+------------+---------------+-----------+---------+---------+ ... +-----------+
^                                ^
chunk 基址                       data_offset（按 A 对齐后的块区起点）
```

`chunk_bytes = data_offset + block_size * blocks_per_chunk`，`malloc` 必须使用这个
总大小，而不是只分配 `sizeof(mp_chunk_t)`。销毁时直接 `free(chunk)`，头部和块区一起释放。

### 3.3 mempool：不透明句柄

`mempool.h` 只暴露：

```c
typedef struct mempool mempool_t;
```

`mempool.c` 内部定义：

```c
struct mempool {
  size_t block_size;        /* 实际块大小，已按 A 对齐 */
  size_t block_per_chunk;   /* 每个 chunk 的块数 */
  size_t data_offset;       /* chunk 基址到第一个块的偏移 */
  size_t chunk_bytes;       /* 每个 chunk 的 malloc 字节数 */
  size_t max_chunks;        /* 0 表示不限制；包含初始 chunk */
  size_t chunk_count;       /* 当前 chunk 数 */
  size_t capacity;          /* 当前总块数 */
  free_list_t free_list;    /* 所有空闲块 */
  mp_chunk_t *chunks;       /* chunk 单链表 */
};
```

不单独保存 `used`，而是：

```text
available = free_list.count
used      = capacity - available
```

好处是只有一个“空闲块数”的真实来源，不会出现两个计数器互相对不上。

### 3.4 空闲链表形态

初始建链时从最后一个块往前 push，这样链表头是 `block 0`：

```text
head -> block 0 -> block 1 -> ... -> block n-1 -> NULL
```

首次分配会按地址递增的顺序返回 `block 0, block 1, ...`，演示更直观。
释放时执行头插：

```text
free(block k) 后：
head -> block k -> (原来的 head) -> ...
```

因此后续分配会优先复用最近释放的块，即 LIFO。这对缓存局部性通常是有利的。

---

## 4. API

### 4.1 `free_list.h`

| 函数 | 语义 |
|------|------|
| `free_list_init(list)` | 清空链表，不释放内存 |
| `free_list_push(list, block)` | 头插一个空闲块，O(1) |
| `free_list_pop(list)` | 弹出链表头；空表返回 `NULL` |
| `free_list_is_empty(list)` | 是否为空 |
| `free_list_size(list)` | 当前空闲块数，O(1) |

前置条件：`list != NULL`；`block` 至少 `sizeof(free_node_t)` 字节，并按
`free_node_t` 对齐。`free_list` 是内部数据结构，这些前置条件由 `mempool` 保证。

### 4.2 `mempool.h`

| 函数 | 返回 | 语义 |
|------|------|------|
| `mempool_create(block_size, blocks_per_chunk, max_chunks)` | `mempool_t *` | 创建池，预分配第一个 chunk；失败返回 `NULL` |
| `mempool_destroy(pool)` | `void` | 释放所有 chunk；`NULL` 安全 |
| `mempool_alloc(pool)` | `void *` | 分配一个块；失败返回 `NULL`；内容未初始化 |
| `mempool_free(pool, data)` | `int` | 归还块；成功 0，参数为 `NULL` 返回 -1 |
| `mempool_block_size(pool)` | `size_t` | 实际块大小（字节） |
| `mempool_capacity(pool)` | `size_t` | 当前总块数 |
| `mempool_used(pool)` | `size_t` | 当前已分配块数 |
| `mempool_available(pool)` | `size_t` | 当前空闲块数 |
| `mempool_chunk_size(pool)` | `size_t` | 当前 chunk 数量 |
| `mempool_contains(pool, data)` | `int` | 调试辅助：地址是否为某个块的起点，O(chunks) |

> `mempool_chunk_size()` 当前返回的是 **chunk 数量**，不是字节数；命名沿用了现有接口。
> 如果后续改名，建议统一为 `mempool_chunk_count()`。

参数约束：

| 参数 | 约束 | 说明 |
|------|------|------|
| `block_size` | `> 0` | 请求的负载大小；实际块大小会向上对齐 |
| `blocks_per_chunk` | `> 0` | 每个 chunk 切多少块；允许为 1，但效率低 |
| `max_chunks` | `0` 或 `> 0` | 总 chunk 数上限；**0 表示不限制**；包含 create 时预分配的第一个 chunk |

关键行为：

- `mempool_create` 预分配第一个 chunk，成功返回的句柄立即可用；
- `mempool_alloc` 在空闲链表非空时 O(1)；为空时尝试扩容，达到 `max_chunks`
  或 `malloc` 失败返回 `NULL`，池仍然有效；
- `mempool_free` 只把块压回空闲链表，O(1)，不调用 `free`；
- `mempool_destroy(NULL)` 是空操作；同一个非 NULL 句柄只能销毁一次；
- 统计函数在 `pool == NULL` 时返回 0。

错误约定：

| 情况 | 行为 |
|------|------|
| `block_size == 0`、`blocks_per_chunk == 0`、算术溢出 | `mempool_create` 返回 `NULL` |
| `max_chunks` 达到上限或扩容 `malloc` 失败 | `mempool_alloc` 返回 `NULL`，池仍然有效 |
| `pool == NULL` / `data == NULL` | `mempool_alloc` 返回 `NULL`；`mempool_free` 返回 -1 |
| 释放外部指针、块内指针、double free | 基础版为未定义行为；`MEMPOOL_DEBUG` 下可用 `mempool_contains` 拦截一部分外部指针 |

---

## 5. 创建、分配、回收、销毁

### 5.1 创建

```text
mempool_create(block_size, blocks_per_chunk, max_chunks):
    block_size == 0 或 blocks_per_chunk == 0 -> NULL
    A = MEMPOOL_ALIGN
    actual = align_up(max(block_size, sizeof(free_node_t)), A)   // 检查溢出
    data_offset = align_up(sizeof(mp_chunk_t), A)
    chunk_bytes = data_offset + actual * blocks_per_chunk        // 先除后乘检查溢出
    max_chunks > 0 时，检查 max_chunks * blocks_per_chunk 溢出
    pool = calloc(1, sizeof(*pool))
    初始化字段和 free_list
    grow(pool)  // 预分配第一个 chunk
    grow 失败 -> free(pool); 返回 NULL
    返回 pool
```

### 5.2 扩容

```text
grow(pool):
    max_chunks != 0 且 chunk_count >= max_chunks -> 失败
    chunk_count == SIZE_MAX -> 失败
    capacity > SIZE_MAX - blocks_per_chunk -> 失败
    chunk = malloc(chunk_bytes)          // 必须一次分配完整 chunk
    malloc 失败 -> 失败，池状态不变
    chunk->count = blocks_per_chunk
    chunk 挂到 chunk 链表头部
    chunk_count++; capacity += blocks_per_chunk
    base = (unsigned char *)chunk + data_offset
    从最后一个块往前 push 所有块到 free_list
    成功
```

扩容的唯一失败点是 `malloc`，并且发生在修改池状态之前；不会留下半初始化的 chunk。

### 5.3 分配

```text
mempool_alloc(pool):
    pool == NULL -> NULL
    如果 free_list 为空:
        如果 grow 失败 -> NULL
    返回 free_list_pop(pool->free_list)
```

顺序是“判空 → 扩容 → pop”，不要先 pop 再发现为空。

### 5.4 回收

```text
mempool_free(pool, data):
    pool == NULL 或 data == NULL -> -1
    MEMPOOL_DEBUG 下检查 mempool_contains(pool, data)，失败返回 -1
    free_list_push(pool->free_list, data)
    返回 0
```

`mempool_free` 不调用 `free`，块仍然属于池；只有 `mempool_destroy` 才释放 chunk 内存。

### 5.5 销毁

```text
mempool_destroy(pool):
    pool == NULL -> 返回
    chunk = pool->chunks
    while chunk:
        next = chunk->next
        free(chunk)          // 头和块区一起释放
        chunk = next
    free(pool)
```

销毁不遍历 free list；free list 的节点都指向即将被释放的 chunk 内存，直接释放 chunk 即可。

---

## 6. 对齐、容量与溢出

### 6.1 对齐公式

```text
A            = _Alignof(max_align_t)          // GCC/Clang + C11
               // MSVC 或旧编译器回退到 16，可用 MEMPOOL_ALIGN 覆盖
block_size   = align_up(max(请求大小, sizeof(free_node_t)), A)
data_offset  = align_up(sizeof(mp_chunk_t), A)
chunk_bytes  = data_offset + block_size * blocks_per_chunk
```

`malloc` 按 C 标准返回满足基本对齐的地址；`data_offset` 和 `block_size` 都是
`A` 的倍数，所以每个块的地址都满足基本对齐。

示例（x86-64，`A = 16`）：

| 请求大小 | 实际块大小 | 说明 |
|----------|------------|------|
| 1 | 16 | 至少容纳 free node，并对齐 |
| 16 | 16 | 正好 |
| 17 | 32 | 向上取整 |
| 33 | 48 | 向上取整 |

用户应通过 `mempool_block_size()` 获取实际可用大小，不要假设它等于请求值。

### 6.2 溢出检查

所有尺寸计算先检查再计算，避免无符号回绕：

- `align_up` 时检查 `value > SIZE_MAX - padding`；
- 分配 chunk 前检查 `blocks_per_chunk <= (SIZE_MAX - data_offset) / block_size`；
- `max_chunks > 0` 时检查 `blocks_per_chunk <= SIZE_MAX / max_chunks`；
- 扩容时检查 `capacity <= SIZE_MAX - blocks_per_chunk`；
- `chunk_count` 达到 `SIZE_MAX` 时拒绝扩容。

### 6.3 碎片与容量

- **内部碎片**：请求 17 字节实际得到 32 字节，多出的部分无法被其他分配使用。
- **外部碎片**：固定大小池内不存在“大小不一导致无法利用”的外部碎片；
  一个 chunk 内的所有块等大。
- 不同大小需求应创建多个 `mempool`，每个尺寸一个池，而不是在一个池里混用不同大小。
- 一个 chunk 的字节数约为 `data_offset + block_size * blocks_per_chunk`，
  没有页大小取模造成的尾部浪费。
- chunk 头部和块区之间的 padding 是唯一的固定开销。

---

## 7. 统计与不变式

| 函数 | 来源 |
|------|------|
| `mempool_block_size` | `pool->block_size` |
| `mempool_capacity` | `pool->capacity` |
| `mempool_available` | `free_list_size(&pool->free_list)` |
| `mempool_used` | `pool->capacity - mempool_available(pool)` |
| `mempool_chunk_size` | `pool->chunk_count` |

任意时刻应满足：

```text
每个块要么已分配，要么在 free list 中，二者必居其一
used + available == capacity
capacity == chunk_count * blocks_per_chunk
```

`mempool_used()` 在 Debug 下会断言 `capacity >= available`；如果这个断言触发，
通常说明 free list 被破坏（例如 double free、释放外部指针或越界写）。

---

## 8. 所有权、生命周期与限制

必须明确的契约：

1. `mempool_alloc` 返回的是**块起始地址**，只能把这个地址原样传给 `mempool_free`；
   传块内部指针、外部指针或另一个池的指针属于 UB。
2. 块在分配状态下，整个 `mempool_block_size` 字节都可以由用户使用；池没有每块头部。
3. 块被释放后，前 `sizeof(free_node_t)` 字节会被写成 `next` 指针；其余字节保留旧数据。
   再次分配时块内容未初始化，用户应先写入再读取。
4. `mempool_free` 不把内存还给 OS，只把块放回 free list；chunk 一直保留到
   `mempool_destroy`。因此池的内存占用是高水位，不会因为集中释放而下降。
5. `mempool_destroy` 会释放所有 chunk，即使还有块未归还；这些块的指针随即失效。
   池不管理块内对象的资源（如 `FILE *`、内部 `malloc` 的内存），这些必须由用户先释放。
6. 基础版不支持 double free 检测。第二次释放同一个块会在 free list 中形成环，
   之后同一块可能被分配多次；这是 UB。`mempool_contains` 只能检查地址是否在池内，
   不能判断块是否已释放。
7. 单线程使用。多个线程同时调用 alloc/free/stats 会竞争 free list 和计数，
   必须加外部锁或等待线程安全版本。
8. 不提供 `realloc`；块大小固定，扩容意味着新分配一块并自行拷贝。
9. 不保证超对齐（例如 32/64 字节 SIMD 类型）；只保证基本对齐。
10. `max_chunks = 0` 表示不限制；如果程序泄漏块，池会一直扩容到 `malloc` 失败。

---

## 9. v1 对照与本次调试记录

### 9.1 `mempool-v1.c` 的问题

`mempool-v1.c` 是一个单页原型，保留了“块内空闲链表 + 批量申请”的正确方向，
但有几处关键问题：

| 位置 | 问题 | 后果 |
|------|------|------|
| 初始化建链 | 循环给每个块写 `next`，最后又在块区外写 `NULL` | `size` 整除 4096 时越界写；最后一块的 `next` 指向非块地址 |
| `mem_alloc` | 空闲计数写成 `free_cnt++` | 计数只增不减，`free_cnt == 0` 永远不成立，最终取到越界地址或解引用 `NULL` |
| 块大小 | 只保证 `>= 16`，没有按对齐向上取整 | 请求 17 字节时相邻块相差 17，后续块可能不满足基本对齐 |
| 容量 | 固定一页 4096 字节 | 不能按需扩容 |
| `mem_destroy` | `free` 后未重置字段 | 重复销毁会 double free；销毁后调用 alloc 会访问已释放内存 |
| `mem_free` | 不校验指针归属 | 外部指针/野指针写入空闲链表；double free 形成环 |

v1 做对的地方要保留：空闲链表节点放在块内部、分配/回收 O(1)、按批申请内存。
v2 把它们拆成模块，并补上对齐、扩容和统计。

### 9.2 本次实现中修过的两个典型问题

**问题 1：`mempool_grow` 只分配了 chunk 头部**

最初写成了：

```c
mp_chunk_t *chunk = (mp_chunk_t*)malloc(sizeof(mp_chunk_t));
```

但后面把 `chunk + data_offset` 当作 `blocks_per_chunk` 个块的起点。
`sizeof(mp_chunk_t)` 只有 16 字节，块区完全在分配之外，导致：

- 块地址与已经分配的块重叠，例如第二个 chunk 的 `p8` 与第一个 chunk 的 `p2` 相同；
- free list 被写进不属于自己的内存；
- 最后同一个地址被释放两次，free list 形成重复节点/环；
- `free_list_pop` 的 `assert(list->count > 0)` 触发。

修复：

```c
mp_chunk_t *chunk = (mp_chunk_t*)malloc(pool->chunk_bytes);
```

`chunk_bytes` 已经包含“头部 + padding + 所有块”，一次分配完整 chunk。
这个 bug 在 `create` 第一次扩容时就已经越界写，ASan 可以直接定位到
`free_list_push` 写入超出 16 字节分配的位置。

**问题 2：`main.c` 里多分配了一次 p8**

演示程序里连续调用了两次 `mempool_alloc`，但只保存了第二次的返回值：

```c
p = mempool_alloc(pool);   /* 第一次：触发扩容，但 p 被覆盖 */
...
p = mempool_alloc(pool);   /* 第二次：真正保存为 p8 */
blocks[count++] = p;
```

第一次分配的块既没有保存也没有释放，导致：

- 扩容后实际分配了 10 个块，`已用=10` 而不是 9；
- 后面填满容量时少一个可用块，最终 `alloc` 失败。

修复：删除第一次 `mempool_alloc`，只保留真正保存并打印的那一次。

**关于 `free_list_pop` 的断言**

```c
assert(list->count > 0);
```

这个断言是**正确的保护**，不是 bug。它检测到 `head != NULL` 但 `count == 0`
的链表不一致状态。上面的 chunk 重叠和重复释放会破坏 free list，所以断言触发。
修复根因后断言自然消失；不要通过删除断言来“解决”问题。

---

## 10. 构建与运行

### 10.1 Linux

```bash
cd component/mempool
cmake -S . -B build
cmake --build build -j
./build/mempool_demo
```

### 10.2 Windows

```powershell
cd component\mempool
cmake -S . -B build
cmake --build build --config Debug
.\build\Debug\mempool_demo.exe
```

如果使用 MinGW / Ninja 等单配置生成器，可执行文件通常在 `build\mempool_demo.exe`。

### 10.3 可选构建选项

```bash
# 在 mempool_free 中启用 O(chunks) 的地址归属检查
cmake -S . -B build -DMEMPOOL_DEBUG_CHECKS=ON

# GCC/Clang 下开启 ASan/UBSan
cmake -S . -B build-asan -DMEMPOOL_ASAN=ON
cmake --build build-asan -j
./build-asan/mempool_demo
```

- MSVC 下 `MEMPOOL_ASAN` 只打印提示，请在 Visual Studio 项目属性中开启 AddressSanitizer；
- 手动用 MSVC 编译时加 `/utf-8`，源文件中的中文注释和演示输出需要 UTF-8 编码；
- `mempool-v1.c` 有独立的 `main`，不要加入当前 CMake 目标；
- CTest 里的 `mempool_demo` 只是冒烟运行，真正的回归测试建议单独写 `test_mempool.c`。

---

## 11. 演示与预期输出

`main.c` 使用：

```text
block_size        = 32
blocks_per_chunk  = 8
max_chunks        = 2
```

演示流程与预期关系：

| 阶段 | 预期 |
|------|------|
| 创建 | 容量 8，已用 0，空闲 8，chunk 1 |
| 分配前 8 个块 | 地址互不相同，按块区顺序返回 |
| 分配 p8 | 触发扩容，容量 16，已用 9，空闲 7，chunk 2；p8 与 p0~p7 都不相同 |
| 释放 p3 | 已用 8，空闲 8 |
| 再次分配 p9 | 返回 p3 的地址，验证 LIFO |
| 填满 16 个块 | 容量 16，已用 16，空闲 0 |
| 再分配 p16 | 返回 `NULL`，达到 `max_chunks` 上限 |
| 释放一个块后分配 | 成功复用，不新增 chunk |
| 全部释放 | 已用 0，空闲 16，容量 16，chunk 2 |
| 销毁 | 所有 chunk 释放，程序正常退出 |

示例输出（地址每次运行不同，关注相等关系和统计数字）：

```text
=== C 固定块内存池演示 ===
配置: 请求块大小=32, 每 chunk 块数=8, 最大 chunk 数=2
创建: 实际块大小=32, 容量=8, 已用=0, 空闲=8, chunk=1
分配 p0 = 0x...
...
第一个 chunk 用尽: 容量=8, 已用=8, 空闲=0, chunk=1
扩容后分配 p8 = 0x...
扩容后: 容量=16, 已用=9, 空闲=7, chunk=2
释放 p3 = 0x...
释放 p3 后: 容量=16, 已用=8, 空闲=8, chunk=2
再次分配 p9 = 0x..., 与 p3 相同 (LIFO)
复用后: 容量=16, 已用=9, 空闲=7, chunk=2
达到容量上限: 容量=16, 已用=16, 空闲=0, chunk=2
达到 max_chunks 后再分配 p16: 返回 NULL (符合预期)
释放一个块后: 容量=16, 已用=15, 空闲=1, chunk=2
释放后再次分配 p16: ...
再次分配后: 容量=16, 已用=16, 空闲=0, chunk=2
全部释放: 容量=16, 已用=0, 空闲=16, chunk=2
销毁完成
```

---

## 12. 测试清单

### 12.1 `free_list` 独立测试

- 初始化后为空，`free_list_pop` 返回 `NULL`，`size == 0`；
- push A、B、C 后依次 pop 得到 C、B、A（LIFO），size 正确；
- 反复 push/pop 1000 次，`count` 与预期一致；
- 架构检查：`free_list.c` 不包含 `mempool.h`，不引用 `malloc/free`；
- `gcc -c free_list.c` 可以单独通过。

### 12.2 创建与边界

- `block_size = 0`、`blocks_per_chunk = 0` → `NULL`；
- `block_size = SIZE_MAX`、`blocks_per_chunk = 2` → `NULL`；
- `blocks_per_chunk` 大到 chunk 字节数溢出 → `NULL`；
- `max_chunks = 0` 合法；`max_chunks = 1` / `2` 合法；
- 创建成功后：实际块大小 ≥ 请求值，且是 `A` 的倍数；容量 = `blocks_per_chunk`；
  已用 0；空闲 = 容量；chunk 数 1；
- 所有失败路径在 ASan/valgrind 下无泄漏。

### 12.3 分配、扩容与上限

- 分配完第一个 chunk 的所有块：全部非 NULL、互不相同、地址对齐；
- 再分配一个：触发扩容，容量增加 `blocks_per_chunk`，chunk 数加 1；
- `max_chunks = 1`：分配完容量后 `alloc` 返回 `NULL`，不扩容；
  释放一块后 `alloc` 成功，chunk 数不变；
- `max_chunks = 2`：最多两个 chunk；达到上限后 `alloc` 返回 `NULL`；
  释放后不扩容即可复用；
- `blocks_per_chunk = 1` 的边界配置也能正常工作。

### 12.4 回收、复用与统计

- 释放一个块后 `available` 加 1、`used` 减 1；
- 紧接着 `alloc` 返回同一个指针（LIFO 复用）；
- 释放多个块，再分配时按最近释放顺序复用；
- 全部释放后 `used == 0`、`available == capacity`，容量和 chunk 数不变；
- 每次操作后断言 `used + available == capacity`；
- `mempool_free(pool, NULL)` 和 `mempool_free(NULL, p)` 返回 -1。

### 12.5 数据完整性与对齐

- 分配完所有块，用一个互不相同的字节模式填满每块的 `mempool_block_size` 字节，
  再逐块校验；若块重叠或越界会失败；
- 特别写每块最后一个字节，检查没有块间重叠；
- 请求非对齐大小（1、7、17、33），检查实际块大小向上取整，所有返回指针对齐；
- 用 `mempool_block_size` 而不是请求值来填充。

### 12.6 生命周期与工具

- 全部释放后 destroy：ASan/valgrind 无泄漏、无越界；
- 仍有未释放块时 destroy：chunk 被释放，不崩溃；之后不再使用这些块指针；
- 不要测试 double destroy 或 destroy 后 alloc 作为“支持的功能”；
- Linux VM 下用 ASan/UBSan + valgrind；Windows 下可用 MSVC ASan 或 Debug CRT 堆检查。

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer ...
valgrind --leak-check=full --show-leak-kinds=all ./mempool_demo
```

### 12.7 随机压力测试

- 维护一个“当前存活指针”数组；
- 随机执行 alloc/free，alloc 失败时检查是否确实达到 `max_chunks` 上限；
- 每次操作后检查统计不变式、存活指针无重复；
- 最后释放所有存活块，断言 `available == capacity`；
- 用多个随机种子和不同配置（块大小 1/17/32、每 chunk 1/4/8 块、max_chunks 1/2/0）跑。

### 12.8 v1 对照实验

- 单独编译 `mempool-v1.c`（不要放进 v2 的 CMake 目标），用 ASan 运行，
  观察初始化阶段的越界；
- 在 v1 的 alloc 前后打印 `free_cnt`，观察它只增不减；
- 这个实验只用于理解问题，不作为 v2 的测试。

---

## 13. 学习点与扩展方向

这个项目最值得带走的东西：

1. **侵入式数据结构的零额外开销**：块空闲时，`next` 就放在块里；块分配出去后，
   整块都是负载。没有每块头，也没有单独的空闲数组。
2. **固定大小 + 批量申请**：分配路径在 free list 非空时完全不调用 `malloc`；
   扩容时一个 chunk 只调用一次 `malloc`，把 n 次建链摊薄。
3. **数据结构与策略解耦**：`free_list` 只负责 LIFO 栈，`mempool` 负责 chunk、
   扩容、对齐和生命周期。换空闲结构不需要动池的公开 API。
4. **对齐是硬约束**：块大小、chunk 数据偏移、最小块大小三者必须一起考虑；
   `malloc` 的基本对齐保证是整条链的基础。
5. **单一数据源**：`available` 来自 free list 的 `count`，`used` 由
   `capacity - available` 推导，减少计数器漂移。
6. **所有权契约**：精确块指针、一次释放、destroy 后失效、内存不归还 OS；
   这些比运行时检查更重要。
7. **v1 和本次调试的教训**：看似能跑的代码可能已经在初始化时越界；
   只分配 chunk 头部、重复 alloc、删除断言，都是典型的“表面修复”。

可继续扩展的方向（按建议优先级）：

1. **线程安全版**：池级 mutex 保护 alloc/free/grow/stats，直接回答父 README 里
   “多个线程共享资源池”的问题；可以复用 threadpool 的并发测试和 TSan 经验。
2. **`mempool_reset`**：把所有 chunk 的块重新建链，丢弃未归还块但不释放 chunk；
   适合“每轮请求结束统一回收”的场景。
3. **调试能力**：`MEMPOOL_DEBUG` 下做 `contains` 校验、free list 遍历计数、
   canary 检测越界；需要时再加 per-block 状态位检测 double free。
4. **size class**：调用者创建多个不同 `block_size` 的池，或写一层分配器按大小选择池；
   这是从固定块池走向 slab 的第一步。
5. **对象池**：在块上增加构造/析构回调，明确“内存池管理字节，对象池管理生命周期”的分层。
6. **chunk 级回收**：允许完全空闲的 chunk 归还 OS；需要 per-chunk 空闲计数或分段
   free list，复杂度明显上升，适合作为进阶练习。
7. **自定义分配器与故障注入**：给池传入 `malloc/free` 函数指针，便于测试 OOM 路径
   和嵌入式环境。
8. **页对齐 / 超对齐**：用 `aligned_alloc` 或平台 API 支持 4KB 页、32/64 字节 SIMD 类型。
9. **无锁 free list**：Treiber 栈 + CAS，配合 ABA 防护；这是并发数据结构的高阶练习，
   不建议作为第一版。

---

## 14. 常见问题

| 现象 | 排查方向 |
|------|----------|
| `mempool_create` 返回 `NULL` | 检查 `block_size > 0`、`blocks_per_chunk > 0`、乘加是否溢出、`max_chunks` 是否导致容量乘法溢出；再检查 `malloc` 是否失败 |
| `mempool_alloc` 返回 `NULL` | 达到 `max_chunks` 上限或扩容 `malloc` 失败；释放一些块或增大上限 |
| 程序在 `mempool_free` 崩溃 | 检查是否 double free、传了块内部指针、传了其他池的指针；开启 `MEMPOOL_DEBUG_CHECKS` 做地址归属检查 |
| 两个块地址相同 / chunk 重叠 | 检查 `mempool_grow` 是否用 `malloc(pool->chunk_bytes)` 而不是 `malloc(sizeof(mp_chunk_t))` |
| `free_list_pop` 的 `assert(list->count > 0)` 失败 | free list 被破坏，常见原因是块重叠、double free、释放外部指针；不要删除断言，修根因 |
| `used + available != capacity` | free list 计数被破坏；检查是否有 double free、越界写或释放了外部指针 |
| 分配到的块不对齐 | 检查 `MEMPOOL_ALIGN`、`block_size`、`data_offset` 是否都按 A 对齐 |
| valgrind 报泄漏 | `mempool_destroy` 会释放 chunk；泄漏通常来自块内对象的资源，或池外分配 |
| Windows 编译报 `_Alignof(max_align_t)` 相关错误 | 使用 C11 模式；或在编译时定义 `MEMPOOL_ALIGN`（例如 16） |
| CMake 报重复 `main` | 把 `mempool-v1.c` 排除出构建；不要用 `file(GLOB ...)` 收集源文件 |
| MSVC 下中文输出乱码 | 编译时加 `/utf-8`；或把演示输出改为英文 |

---

## 15. 相关文档

- 父目录导航：[`../README.md`](../README.md)
- 同级线程池：[`../threadpool/README.md`](../threadpool/README.md)
- 空闲链表：[`free_list.h`](free_list.h) / [`free_list.c`](free_list.c)
- 内存池：[`mempool.h`](mempool.h) / [`mempool.c`](mempool.c)
- 演示程序：[`main.c`](main.c)
- 构建脚本：[`CMakeLists.txt`](CMakeLists.txt)
- 旧版原型：[`mempool-v1.c`](mempool-v1.c)
