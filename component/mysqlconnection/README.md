# mysqlconnection —— 同步 / 异步 MySQL 连接池（C++17）

用 C++17 和 libmysqlclient 实现的两个 MySQL 连接池：

- **同步连接池**：多个业务线程共享一组物理连接，通过 `mutex + condition_variable`
  完成借出、归还、池满等待和超时控制；
- **异步连接池**：在同步池之上增加固定工作线程、有界任务队列和
  `std::future` / 回调，把阻塞在 MySQL 网络 I/O 上的工作从调用线程移走。

这个子项目的重点不是写一个通用中间件，而是把连接池的状态机、锁边界、容量预留、
健康检查、空闲回收、关闭顺序和背压讲清楚。代码量不大，但每一处等待、计数和
通知都对应一个并发问题。

| 项 | 说明 |
|----|------|
| 语言 / 标准 | C++17 |
| 平台 | Linux（pthread）；Windows 需自行提供客户端库与等价线程环境 |
| 构建 | CMake ≥ 3.16 |
| 客户端库 | libmysqlclient 或 MariaDB Connector/C（`mysql_config` / `mariadb_config`） |
| 当前产物 | 静态库 `libmysqlpool.a`（CMake 当前只定义了 `mysqlpool` 目标） |
| 同步池结构 | `mutex` + 条件变量 + 空闲 FIFO + RAII 句柄 |
| 异步池结构 | 同步池 + 固定 worker + 有界任务队列 + `promise/future` + 回调 |
| 日志 | 默认 `Info` 级别，输出到 `stderr`，多线程整行输出 |

---

## 1. 特性与非目标

**特性**

- 初始化时预创建 `min_size` 条连接，按需扩容到 `max_size`；
- `acquire()` 返回 `ConnGuard`，作用域结束自动归还，异常展开也不会漏还；
- 空闲连接以 FIFO 队列管理，尽量让所有空闲连接都能被均匀复用和回收；
- 池满且全部借出时，`acquireFor()` 按超时等待，超时返回无效 guard；
- 扩容时先在锁内预留名额，再在锁外调用 `mysql_real_connect()`，
  避免并发扩容超过 `max_size`；
- 可选的在借出前 `mysql_ping()` 健康检查；ping 失败会销毁旧连接并按配置重连；
- 后台维护线程按 `idle_timeout` / `max_lifetime` 回收空闲连接，并补足 `min_size`；
- 关闭时先停止借出，等待在途连接归还（最多 5 秒），再统一关闭物理连接；
- 异步池提供 `future` 与回调两种提交方式，并统计排队、取连接、SQL 执行耗时；
- 异步任务队列有上限，队列满时快速失败，形成背压而不是无限堆积；
- 异步关闭会排空已提交任务，再 join 工作线程并关闭底层同步池；
- 统计连接创建、销毁、复用、等待、超时和健康检查失败次数。

**非目标**

- 不是生产级数据库中间件：没有故障转移、读写分离、SQL 路由和负载均衡；
- 不支持 prepared statement、流式结果集、多结果集、存储过程结果集；
- 没有自动事务管理；`beginTransaction()/commit()/rollback()` 只是语法糖；
- 异步是“工作线程 + 阻塞驱动”的异步，不是内核级非阻塞 MySQL 状态机；
- 不支持取消正在执行的查询，查询超时只能依赖客户端库的读/写超时；
- 不动态调整 `min_size/max_size`，也没有基于负载的扩缩容；
- 当前 CMake 只构建静态库，没有单独的 demo/test 可执行目标；
- 未做 Windows 平台验证，未显式封装 `mysql_thread_init/mysql_thread_end`。

---

## 2. 目录结构与依赖关系

```text
mysqlconnection/
├── CMakeLists.txt              构建脚本，定位 libmysqlclient / pthread
├── include/mysqlpool/
│   ├── mysql_config.h          连接参数与环境变量读取
│   ├── logger.h                分级日志与变参格式化
│   ├── query_result.h          ResultSet / QueryResult 的 RAII 封装
│   ├── mysql_conn.h            单条物理连接的封装
│   ├── sync_connection_pool.h  同步连接池、ConnGuard、统计
│   └── async_connection_pool.h 异步连接池、AsyncQueryResult
└── src/
    ├── mysql_config.cpp
    ├── logger.cpp
    ├── query_result.cpp
    ├── mysql_conn.cpp
    ├── sync_connection_pool.cpp
    └── async_connection_pool.cpp
```

依赖方向：

```text
async_connection_pool
  └── sync_connection_pool
        ├── mysql_conn
        │     ├── mysql_config
        │     ├── query_result
        │     └── logger
        └── logger
```

模块边界：

- `MysqlConn` 只关心“一条物理连接”：建立、关闭、执行 SQL、ping、重连；
- `SyncConnectionPool` 只关心“连接怎么借、怎么还、怎么等、怎么回收”；
- `AsyncConnectionPool` 只关心“任务怎么排队、由谁执行、结果怎么交回”，
  底层连接管理完全复用同步池；
- `ResultSet` 只管理 `MYSQL_RES*` 的生命周期，不持有连接；
- `MySQLConfig` 不包含任何池参数，池参数在 `SyncPoolOptions` 中。

---

## 3. 快速开始

### 3.1 安装依赖

Ubuntu / Debian（MySQL）：

```bash
sudo apt update
sudo apt install -y build-essential cmake libmysqlclient-dev mysql-server
```

Ubuntu / Debian（MariaDB）：

```bash
sudo apt install -y build-essential cmake libmariadb-dev mariadb-server
```

CentOS / RHEL：

```bash
sudo dnf install -y gcc-c++ cmake make mysql-devel mysql-server
# 或者 mariadb-devel / mariadb-server
```

确认客户端库能被 CMake 找到：

```bash
mysql_config --version
mysql_config --include
mysql_config --libs

# MariaDB 环境
mariadb_config --version
```

### 3.2 准备数据库

启动服务并进入 MySQL：

```bash
sudo systemctl start mysql
# MariaDB 可能是：sudo systemctl start mariadb

sudo mysql
```

执行下面的 SQL。默认数据库名与 `MySQLConfig::database` 一致，都是
`pool_learning`：

```sql
CREATE DATABASE IF NOT EXISTS pool_learning
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_general_ci;

-- 仅本地学习环境可以使用 '%'；生产环境请限制来源主机。
CREATE USER IF NOT EXISTS 'pool'@'%' IDENTIFIED BY 'pool123';
GRANT ALL PRIVILEGES ON pool_learning.* TO 'pool'@'%';
FLUSH PRIVILEGES;

USE pool_learning;

CREATE TABLE IF NOT EXISTS pool_demo (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(64) NOT NULL,
    note VARCHAR(255) NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

如果 `root` 使用 `auth_socket`，通过 TCP 登录可能会失败，建议使用上面的
`pool` 账号。设置环境变量：

```bash
export MYSQL_HOST=127.0.0.1
export MYSQL_PORT=3306
export MYSQL_USER=pool
export MYSQL_PASSWORD=pool123
export MYSQL_DATABASE=pool_learning
export MYSQL_CHARSET=utf8mb4
```

先验证命令行能连通：

```bash
mysql -h "$MYSQL_HOST" -P "$MYSQL_PORT" -u "$MYSQL_USER" \
      -p"$MYSQL_PASSWORD" "$MYSQL_DATABASE" \
      -e "SELECT 1 AS ok, VERSION() AS version;"
```

### 3.3 构建

```bash
cd component/mysqlconnection

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

构建产物是静态库：

```text
build/libmysqlpool.a
```

如果需要 AddressSanitizer / UBSan：

```bash
cmake -S . -B build-asan \
      -DCMAKE_BUILD_TYPE=Debug \
      -DMYSQLPOOL_ASAN=ON
cmake --build build-asan -j"$(nproc)"
```

CMake 会优先执行 `mysql_config --include` 和 `mysql_config --libs`；
找不到 `mysql_config/mariadb_config` 时，退回 `find_path` + `find_library`。
库目标通过 `Threads::Threads` 链接 pthread。

### 3.4 最小同步示例

当前仓库没有单独的示例可执行文件，可以把下面代码保存为 `sync_demo.cpp`，
再链接 `libmysqlpool.a`：

```cpp
#include "mysqlpool/sync_connection_pool.h"

#include <chrono>
#include <iostream>
#include <string>

int main() {
    using namespace mysqlpool;

    const MySQLConfig config = configFromEnv();

    SyncPoolOptions options;
    options.min_size = 2;
    options.max_size = 4;
    options.acquire_timeout = std::chrono::milliseconds(2000);

    SyncConnectionPool pool(config, options);

    std::string error;
    if (!pool.init(&error)) {
        std::cerr << "init failed: " << error << '\n';
        return 1;
    }

    {
        auto guard = pool.acquire();
        if (!guard) {
            std::cerr << "acquire failed\n";
            return 1;
        }

        auto result = guard->query(
            "SELECT CONNECTION_ID() AS cid, 1 + 1 AS answer");

        if (!result.ok) {
            std::cerr << "query failed: " << result.error << '\n';
            return 1;
        }

        if (result.result.next()) {
            std::cout << "pool conn #" << guard->id()
                      << ", server thread " << guard->serverThreadId()
                      << ", answer = " << result.result.value(1) << '\n';
        }
    }   // guard 析构，连接自动归还

    const auto stats = pool.stats();
    std::cout << "total=" << stats.total
              << " idle=" << stats.idle
              << " busy=" << stats.busy
              << " created=" << stats.created
              << " reused=" << stats.reused << '\n';

    return 0;
}
```

编译并运行：

```bash
g++ -std=c++17 -Iinclude $(mysql_config --include) sync_demo.cpp \
    build/libmysqlpool.a $(mysql_config --libs) -pthread -o sync_demo

./sync_demo
```

MariaDB 环境把 `mysql_config` 换成 `mariadb_config` 即可。

### 3.5 最小异步示例

```cpp
#include "mysqlpool/async_connection_pool.h"

#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace mysqlpool;

    const MySQLConfig config = configFromEnv();

    SyncPoolOptions sync_options;
    sync_options.min_size = 2;
    sync_options.max_size = 4;

    AsyncPoolOptions async_options;
    async_options.worker_threads = 3;
    async_options.max_pending = 16;

    AsyncConnectionPool pool(config, sync_options, async_options);

    std::string error;
    if (!pool.init(&error)) {
        std::cerr << "init failed: " << error << '\n';
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();

    std::vector<std::future<AsyncQueryResult>> futures;
    for (int i = 1; i <= 3; ++i) {
        futures.push_back(pool.submit(
            "SELECT SLEEP(0.2) AS slept, " + std::to_string(i) + " AS n"));
    }

    for (auto& future : futures) {
        auto result = future.get();
        if (!result.ok) {
            std::cerr << "query failed: " << result.error << '\n';
            continue;
        }

        std::cout << "conn #" << result.connection_id
                  << " query=" << result.query_time.count() << "us"
                  << " total=" << result.total_time.count() << "us\n";
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    std::cout << "wall time: " << elapsed.count() << "ms\n";
    return 0;
}
```

编译命令与同步示例相同，只需替换源文件名。3 个 `SLEEP(0.2)` 由 3 个 worker
并行执行，实际耗时应接近 0.2 秒，而不是 0.6 秒。

---

## 4. 同步连接池

### 4.1 状态模型与不变量

池内部为每条物理连接维护一个槽位：

```cpp
struct Slot {
    std::unique_ptr<MysqlConn> conn;
    bool in_use;
    steady_clock::time_point created_at;
    steady_clock::time_point last_returned;
};
```

槽位有两种稳定状态：

- **空闲**：下标在 `idle_slots_` 中，`in_use == false`；
- **借出**：被一个 `ConnGuard` 持有，`in_use == true`。

关键不变量：

- 同一个槽位不可能同时出现在空闲队列和被借出状态；
- `idle_slots_` 存的是下标而不是 `Slot*`，`slots_` 扩容时 guard 仍然能定位槽位；
- `total_` 包含“已有连接 + 正在锁外创建的预留名额”，因此可能暂时大于
  `idle + busy`；
- 每个借出的槽位恰好由一个 `ConnGuard` 负责归还；
- `total_ <= max_size` 在任意稳定时刻成立。

`idle_slots_` 使用 FIFO：

- 归还时 `push_back`；
- 借出时 `pop_front`。

这样所有空闲连接都会轮流被复用，不会出现“队头几条被频繁使用、队尾一直闲置”
的情况，空闲超时回收也更自然。

### 4.2 acquire 的三条路径

```text
acquireFor(timeout)
        |
        v
+------------------------+
| 空闲队列非空？          | --是--> 弹出队头，标记 in_use
+------------------------+        锁外 ping/重连
        | 否                      成功则返回 ConnGuard
        v
+------------------------+
| total_ < max_size？     | --是--> 锁内 total_++ / creating_++
+------------------------+        解锁，mysql_real_connect()
        | 否                      成功后加回池并标记借出
        v
+------------------------+
| 池满，全部借出          | --> available_cv_.wait_until(deadline)
+------------------------+     被唤醒后重新检查
```

三条路径对应三种资源状态：**有空闲可复用**、**可扩容**、**必须等待归还**。

### 4.3 为什么扩容要先在锁内占名额

如果直接在锁外做：

```text
if (total_ < max_size) {          // 多个线程可能同时判断为真
    conn = mysql_real_connect();  // 网络耗时较长
    total_++;
}
```

多个线程可能同时看到“还能扩容”，最终连接数超过 `max_size`。因此实现中：

1. 锁内执行 `++total_; ++creating_;`，先占住名额；
2. 解锁执行 `mysql_real_connect()`；
3. 重新加锁，`--creating_`；
4. 建连失败则 `--total_` 归还名额；成功则把槽位加入池并标记借出。

这把“判断容量”和“占用容量”合并成一个原子动作，代价是建连期间 `total_`
会包含尚未建成的连接，统计上看到 `total > idle + busy` 是正常的。

### 4.4 健康检查与重连

`validate_on_acquire == true` 时，借出空闲连接后会先在锁外执行
`MysqlConn::ensureAlive()`：

```text
mysql_ping()
    | 成功 -> 直接借出
    | 失败 -> close()
             connect(config_)   // 用建立连接时保存的配置重连
                | 成功 -> 借出重连后的连接
                | 失败 -> 销毁该槽位，重新走 acquire 流程
```

为什么要放在锁外：ping 是网络往返，持有池锁执行会阻塞所有其他借还线程。
此时槽位已经被标记为 `in_use`，其他线程不会操作它。

**重连会丢失会话状态**。事务、临时表、用户变量、会话级参数都不会恢复。
因此连接池适合“短查询、借出即用、用完即还”的模式；不要把连接借出后跨越
长时间事务，也不要指望健康检查能恢复事务上下文。

如果对每次借出的延迟敏感，可以关闭 `validate_on_acquire`，依靠服务端
`wait_timeout` 和业务侧错误处理；本示例默认开启，是为了让“失效连接被替换”
的过程可见。

### 4.5 维护线程与空闲回收

`init()` 成功后启动一个维护线程，每隔 `maintenance_interval` 巡检一次：

1. 扫描空闲队列，找出满足以下任一条件的连接：
   - 空闲时间超过 `idle_timeout`；
   - 创建时间超过 `max_lifetime`。
2. 在保证 `total_ > min_size` 的前提下销毁这些连接；
3. 如果 `total_ < min_size`，补建连接直到达到最小水位。

几个细节：

- 回收只发生在**空闲**连接上，不会把正在事务中的连接换掉；
- `total_ <= min_size` 时停止回收，保证最小可用连接数；
- `idle_timeout == 0` 或 `max_lifetime == 0` 表示关闭对应维度的回收；
- 当前实现为了结构直观，在持有池锁时创建补充连接。生产实现通常会把建连
  移到锁外，或者用异步健康检查，避免网络抖动时长时间占锁。

### 4.6 RAII 归还

```cpp
{
    auto guard = pool.acquire();
    auto result = guard->query("INSERT ...");
}   // guard 析构 -> releaseSlot()
```

`ConnGuard` 是 move-only 的：

- 默认构造得到“无效 guard”，`static_cast<bool>(guard) == false`；
- `release()` 可以提前归还，归还后 guard 变为无效；
- 移动构造/移动赋值转移所有权，被移动后的 guard 不再归还；
- 析构函数调用 `releaseSlot()`，即使中途抛出异常也能归还。

因此业务代码要养成两个习惯：

1. 使用 `acquire()` 后先检查 `if (!guard)`；
2. 用一个明确的作用域包住 guard，不要把它长期保存在成员变量里。

### 4.7 关闭顺序

`shutdown()`：

```text
1. stopping_ = true
2. notify 所有等待空闲连接的线程
3. 唤醒并 join 维护线程
4. 等待 busy_ == 0 且 creating_ == 0（最多 5 秒）
5. 关闭所有 MYSQL*，清空槽位
```

先停止新借出，再等待在途连接归还，最后才关闭物理连接。

如果 5 秒后仍有连接被借出，池会打印警告并强制关闭。这是程序员使用错误的
信号：**`ConnGuard` 的生命周期必须短于 `SyncConnectionPool`**。池析构后再
析构 guard 会访问已经销毁的池对象，属于未定义行为。

`shutdown()` 可以重复调用；`stopping_` 置位后，这个池对象不能再次 `init()`。

### 4.8 统计指标

| 字段 | 含义 | 观察方式 |
|------|------|----------|
| `total` | 当前池内连接总数（含创建中的预留名额） | 并发扩容时可能暂时大于 idle + busy |
| `idle` | 当前空闲连接数 | 正常归还后应增加 |
| `busy` | 当前借出连接数 | 所有 guard 离开作用域后应为 0 |
| `created` | 累计创建物理连接数 | 没有异常时一般不超过 max_size |
| `destroyed` | 累计销毁物理连接数 | 健康检查失败、空闲回收会增加 |
| `reused` | 成功复用空闲连接的次数 | 顺序借还时应持续增长 |
| `waits` | 进入“池满等待”状态的次数 | 并发线程数超过 max_size 时增长 |
| `timeouts` | 获取连接超时次数 | 池容量不足或连接被长期占用时增长 |
| `validation_failures` | 借出前 ping 失败的次数 | KILL、服务端重启、wait_timeout 后可能增长 |

注意：

- `reused` 是“复用空闲槽位”的次数，不是 SQL 执行次数；
- `waits` 统计进入等待的次数，不统计等待了多久；
- `timeouts` 只在等待达到截止时间等情况下增加，池被关闭导致的获取失败
  不计入超时；
- `stats()` 返回的是某一时刻的副本，后续池继续变化不会影响该副本。

---

## 5. 异步连接池

### 5.1 为什么还需要工作线程

libmysqlclient 的普通 C API 是阻塞的：

```text
mysql_real_query() 内部会阻塞在 socket send/recv 上
mysql_store_result() 还要等结果全部到齐
```

因此这里的“异步”不是把 MySQL 协议改写成内核级非阻塞状态机，而是：

```text
调用线程                       工作线程                    同步连接池
   |                              |                             |
   | submit(sql)                  |                             |
   |----------------------------->| 任务入队                     |
   | 立即拿到 future              |                             |
   |                              | 出队                         |
   |                              | acquire() ----------------->|
   |                              |<-------------------- guard   |
   |                              | query()                     |
   |                              | release guard ------------->|
   |                              | set_value(result)           |
   |<-----------------------------|                             |
   | future.get() 得到结果         |                             |
```

异步池并没有让 MySQL 驱动变得非阻塞，只是把阻塞操作搬到了内部线程。
调用线程的收益是：**提交任务后可以立即去干别的事**。

### 5.2 任务生命周期与耗时字段

一次任务的生命周期：

```text
submit()
  |  记录 submitted_at
  v
任务队列（queue<unique_ptr<Task>>）
  |
  |  queue_time：从提交到 worker 取到任务
  v
worker 取任务
  |
  |  acquire_time：从 worker 开始执行到拿到 ConnGuard
  v
从同步池 acquire 连接
  |
  |  query_time：guard->query() 的驱动耗时
  v
执行 SQL，归还连接
  |
  |  total_time：从提交到结果准备完成
  v
finishTask：promise.set_value() 或调用回调
```

`AsyncQueryResult` 同时携带：

- `ok / error`：任务是否成功；
- `sql`：原始 SQL，便于日志定位；
- `connection_id`：使用的池内连接编号；
- `queue_time / acquire_time / query_time / total_time`；
- `has_result_set / affected_rows / insert_id / result`。

如果是任务队列满、池未初始化、池已关闭等“未执行”情况，future 会得到一个
`ok == false` 的结果，时间是 0，而不是 `broken_promise`。

### 5.3 提交、队列与背压

```cpp
auto future = pool.submit("SELECT ...");
auto result = future.get();

bool accepted = pool.submit("SELECT ...", [](AsyncQueryResult r) {
    // 这个回调在 worker 线程执行
});
```

`submit()` 的提交路径：

```text
加锁 queue_mutex_
   |
   +-- stopping_ == true  -> 拒绝
   +-- started_ == false  -> 拒绝
   +-- 队列已满（max_pending > 0）-> 拒绝
   +-- 入队、解锁、notify_one
```

`max_pending` 是背压开关：

- `0` 表示队列不限制（不建议在不可控的生产流量下使用）；
- 队列满时 future 版本立即返回错误结果，回调版本立即返回 `false`；
- 不阻塞提交线程，避免异步接口退化成“慢速同步接口”。

如果需要“提交线程阻塞直到队列有空位”的语义，可以自行在上层加信号量，
本项目刻意保留快速失败，方便观察背压。

### 5.4 future 版本与回调版本的区别

| | future 版本 | 回调版本 |
|---|---|---|
| 返回值 | `std::future<AsyncQueryResult>` | `bool`，表示是否接收 |
| 结果交付 | `future.get()` | 回调参数 |
| 执行线程 | 调用 `get()` 的线程等待 | 内部 worker 线程执行回调 |
| 被拒绝 | future 中是错误结果 | 返回 `false`，回调不会调用 |
| 异常 | 结果里是错误信息 | 回调抛出的异常被捕获并记录日志，不会传播 |

回调版本的注意事项：

1. 回调运行在 worker 线程上，**不要做长时间阻塞操作**，否则会拖住 worker；
2. 不要在回调里调用本池的 `shutdown()`，那会尝试 join 当前线程，造成死锁；
3. 回调里访问共享数据要自己加锁；多个 worker 的回调可能并发执行；
4. 回调里可以继续 `submit()`，但池已经进入关闭流程时提交会被拒绝。

### 5.5 shutdown 的排空语义

```text
shutdown()
   |
   +-- stopping_ = true，拒绝新的 submit
   +-- notify 所有 worker
   +-- worker 继续执行队列中已有的任务（排空）
   +-- join 所有 worker
   +-- 关闭底层同步连接池
```

因此下面这种写法是安全的：

```cpp
auto future = pool.submit("SELECT 1");
pool.shutdown();
auto result = future.get();   // 仍然能拿到结果
```

`shutdown()` 会等待队列中最慢的任务完成，不会取消正在执行的 SQL。
如果任务可能执行很久，关闭耗时也会相应变长。

### 5.6 worker 数量与连接数量

- `worker_threads` 是同时执行 SQL 的上限；
- `SyncPoolOptions::max_size` 是物理连接上限；
- 通常建议 `worker_threads <= max_size`，否则多出来的 worker 只会在
  `acquire()` 上排队，增加上下文切换；
- worker 超过连接数并不错误，但 `queue_time` 会变大，`poolStats()` 的
  `waits` 也会增加。

异步池并没有为每个 worker 固定绑定一条连接，worker 每次任务结束都会归还，
下一次任务重新从同步池借出。

### 5.7 异步池的易错点

- 在回调里调用 `shutdown()`：可能 join 当前线程，死锁；
- 在回调里执行 `sleep`、等待锁或其他长阻塞操作：占住 worker；
- 把 `ConnGuard` 存到回调外部，导致连接被长期占用；
- 以为 `submit()` 返回就代表 SQL 执行完成：必须 `get()` 或等回调；
- 以为池会取消队列中的任务：shutdown 是排空，不是丢弃；
- 把 `max_pending` 设为 0 后无限提交：内存会持续增长。

---

## 6. 连接与结果集封装

### 6.1 MysqlConn

`MysqlConn` 是对单条 `MYSQL*` 的 RAII 封装：

- `connect(config, error)`：`mysql_init` + 超时/字符集选项 + `mysql_real_connect`；
- `close()`：`mysql_close`，可重复调用；
- `ping()`：只 ping，不重连；
- `ensureAlive()`：ping 失败后按保存的配置重连；
- `query(sql)`：执行 SQL，返回 `QueryResult`；
- `escape(input)`：`mysql_real_escape_string` 的封装；
- `beginTransaction()/commit()/rollback()`：执行对应 SQL 的便捷方法；
- `id()`：连接池分配的编号，仅用于日志/观察；
- `serverThreadId()`：服务端 `CONNECTION_ID()`，连接复用时保持不变；
- `serverInfo()`：服务端版本信息；
- `raw()`：获取底层 `MYSQL*`，用于调用未封装的 C API。

`MysqlConn` 是 move-only 的，且**不保证线程安全**：同一时刻只能由一个线程
使用。连接池通过 `ConnGuard` 保证这一点。

`connect()` 首次调用时通过 `std::call_once` 执行一次
`mysql_library_init()`，避免多线程重复初始化客户端库。项目没有显式调用
`mysql_library_end()`，进程退出时由操作系统回收；如果业务需要在退出前
干净卸载客户端库，可以自行补充。

### 6.2 QueryResult

```cpp
struct QueryResult {
    bool ok;
    bool has_result_set;
    ResultSet result;
    unsigned long long affected_rows;
    unsigned long long insert_id;
    std::string error;
    std::chrono::microseconds elapsed;
};
```

- `SELECT` 成功时 `has_result_set == true`，行数据在 `result` 中；
- `INSERT/UPDATE/DELETE/DDL` 成功时通常 `has_result_set == false`，
  关注 `affected_rows` 和 `insert_id`；
- 失败时 `ok == false`，`error` 是 `mysql_error()` 的内容；
- `elapsed` 是驱动程序执行 SQL 的耗时，不包含排队和借连接时间。

本项目使用“错误值”而不是异常：查询失败通过 `ok/error` 返回，避免在连接池
内部频繁抛出异常。

### 6.3 ResultSet

`ResultSet` 管理 `MYSQL_RES*`，只支持缓冲结果集（`mysql_store_result`）：

- `next()`：读取下一行；
- `value(i)`：读取当前行第 i 列，SQL NULL 返回字符串 `"NULL"`；
- `isNull(i)`：区分 SQL NULL 和普通字符串 `"NULL"`；
- `currentRow()`：当前行转为 `vector<string>`；
- `columnNames()` / `columnCount()` / `rowCount()`；
- `rewind()`：把游标移回第一行之前；
- `toString()`：输出简单文本表格，会从第一行读到最后并重置游标；
- `release()`：放弃所有权，返回底层 `MYSQL_RES*`。

`ResultSet` 是 move-only 的，且不是线程安全的。

`toString()` 会扫描全部行并计算列宽，只适合学习演示和小结果集；不要把
它用于日志打印大查询的结果。

### 6.4 字符集、转义与事务

- 字符集通过 `MYSQL_SET_CHARSET_NAME` 在建立连接前设置，默认 `utf8mb4`；
- `escape()` 使用当前连接执行转义，连接未建立时返回空字符串；
- 手工拼 SQL 仍然有注入风险，生产代码应优先使用 prepared statement；
- `beginTransaction()` 只是执行 `START TRANSACTION`，连接池不会自动回滚；
- 事务必须在一个 `ConnGuard` 生命周期内完成，不能跨连接。

### 6.5 日志

- 默认级别 `Info`；
- `setLogLevel(LogLevel::Debug)` 可以看到维护线程回收/补充连接的日志；
- 日志输出到 `stderr`，多线程整行加锁，避免行内交错；
- 日志内容包含时间戳、级别、源文件和行号。

---

## 7. 配置与 API 参考

### 7.1 MySQLConfig

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `host` | `127.0.0.1` | MySQL 主机 |
| `port` | `3306` | MySQL 端口 |
| `user` | `root` | 用户名 |
| `password` | 空 | 密码 |
| `database` | `pool_learning` | 默认数据库 |
| `charset` | `utf8mb4` | 连接字符集 |
| `connect_timeout_sec` | 3 | 建连超时，秒；0 表示不显式设置 |
| `read_timeout_sec` | 5 | 读超时，秒 |
| `write_timeout_sec` | 5 | 写超时，秒 |

### 7.2 环境变量

`configFromEnv()` 支持：

| 变量 | 对应字段 | 默认值 |
|------|----------|--------|
| `MYSQL_HOST` | `host` | `127.0.0.1` |
| `MYSQL_PORT` | `port` | `3306` |
| `MYSQL_USER` | `user` | `root` |
| `MYSQL_PASSWORD` | `password` | 空 |
| `MYSQL_DATABASE` | `database` | `pool_learning` |
| `MYSQL_CHARSET` | `charset` | `utf8mb4` |
| `MYSQL_CONNECT_TIMEOUT` | `connect_timeout_sec` | 3 |
| `MYSQL_READ_TIMEOUT` | `read_timeout_sec` | 5 |
| `MYSQL_WRITE_TIMEOUT` | `write_timeout_sec` | 5 |

空字符串变量会被忽略，使用默认值；端口和超时解析失败也会回退默认值。

### 7.3 SyncPoolOptions

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `min_size` | 2 | 预创建、维护时保持的最小连接数 |
| `max_size` | 8 | 池内连接总数上限（含创建中名额） |
| `acquire_timeout` | 3000ms | `acquire()` 默认等待时间 |
| `idle_timeout` | 60000ms | 空闲回收阈值；0 表示不按空闲时间回收 |
| `max_lifetime` | 0 | 连接最大存活时间；0 表示不限制 |
| `maintenance_interval` | 10000ms | 维护线程巡检周期 |
| `validate_on_acquire` | true | 借出前是否 ping |

构造函数会做几项兜底：

- `max_size == 0` 时改为 1；
- `min_size > max_size` 时令 `min_size = max_size`；
- `maintenance_interval <= 0` 时改为 1 秒，避免维护线程忙等。

### 7.4 SyncPoolStats

字段含义见 [4.8 统计指标](#48-统计指标)。

### 7.5 AsyncPoolOptions

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `worker_threads` | 2 | 工作线程数；0 会被改为 1 |
| `max_pending` | 1024 | 任务队列上限；0 表示不限制 |
| `acquire_timeout` | 3000ms | worker 从同步池取连接的超时 |

### 7.6 AsyncQueryResult

| 字段 | 说明 |
|------|------|
| `ok` | 任务是否成功 |
| `error` | 错误信息 |
| `sql` | 原始 SQL |
| `connection_id` | 使用的池内连接编号（0 表示没拿到连接） |
| `queue_time` | 从 submit 到 worker 开始执行 |
| `acquire_time` | 从 worker 开始执行到拿到连接 |
| `query_time` | 驱动执行 SQL 的耗时 |
| `total_time` | 从 submit 到结果准备完成 |
| `has_result_set` | 是否有结果集 |
| `affected_rows` | 影响行数 |
| `insert_id` | 自增主键 |
| `result` | `ResultSet` |

### 7.7 主要方法

`SyncConnectionPool`：

| 方法 | 说明 |
|------|------|
| `init(error)` | 预创建连接、启动维护线程；可重复调用 |
| `shutdown()` | 停止维护、等待归还、关闭连接；可重复调用 |
| `initialized()` | 是否处于可用状态 |
| `acquire()` | 使用默认超时借出连接 |
| `acquireFor(timeout)` | 指定超时借出连接 |
| `stats()` | 获取统计快照 |
| `config()/options()` | 只读访问配置 |

`AsyncConnectionPool`：

| 方法 | 说明 |
|------|------|
| `init(error)` | 初始化底层同步池并启动 worker |
| `shutdown()` | 拒绝新任务、排空队列、join worker、关闭同步池 |
| `submit(sql)` | 返回 `future<AsyncQueryResult>` |
| `submit(sql, callback)` | 回调版本，返回是否接收 |
| `pending()` | 当前排队任务数（不含执行中） |
| `poolStats()` | 底层同步池统计 |

---

## 8. 手工验证清单

当前 CMake 没有单独的自动化测试目标，可以用上面的最小示例和下面的实验来
验证核心行为。

### 8.1 连接复用

把同步示例中的借还放进一个 6 次循环，打印：

```cpp
std::cout << "pool conn #" << guard->id()
          << " server thread " << guard->serverThreadId() << '\n';
```

使用 `min_size = 2, max_size = 4`：顺序借还时应该只看到两个服务端线程 ID
反复出现，说明走的是复用路径，而不是每次都新建连接。

### 8.2 池满与超时

```cpp
SyncPoolOptions options;
options.min_size = 1;
options.max_size = 1;
options.acquire_timeout = std::chrono::milliseconds(1000);

SyncConnectionPool pool(config, options);
pool.init();

auto first = pool.acquire();                            // 成功
auto second = pool.acquireFor(std::chrono::milliseconds(200));
// second 为无效 guard；pool.stats().timeouts 增加

first.release();
auto third = pool.acquireFor(std::chrono::milliseconds(1000)); // 成功
```

预期：`second` 的 `operator bool` 为 false，`timeouts >= 1`。

### 8.3 RAII 归还

```cpp
try {
    auto guard = pool.acquire();
    throw std::runtime_error("boom");
} catch (const std::exception&) {
}

// 这里 busy 应该回到 0
```

### 8.4 异步并行

使用 `worker_threads = 3`，同时提交 3 个 `SELECT SLEEP(0.2)`：

```cpp
pool.submit("SELECT SLEEP(0.2)");
pool.submit("SELECT SLEEP(0.2)");
pool.submit("SELECT SLEEP(0.2)");
```

预期墙钟约 0.2 秒；把 `worker_threads` 改成 1 后应接近 0.6 秒。

### 8.5 队列背压

使用 `worker_threads = 1, max_pending = 1`：

```cpp
auto slow = pool.submit("SELECT SLEEP(0.6)");
auto second = pool.submit("SELECT 2");
auto third = pool.submit("SELECT 3");

auto r3 = third.get();
// r3.ok == false，r3.error 包含“任务队列已满（背压）”
```

被拒绝的提交仍然会得到一个完整的错误结果，不会触发 broken promise。

### 8.6 关闭排空

```cpp
auto future = pool.submit("SELECT 99");
pool.shutdown();
auto result = future.get();   // 仍然成功
```

`shutdown()` 之后再提交，future 中的结果应为 `ok == false` 且错误信息包含
“关闭”。

### 8.7 健康检查（可选）

用一条连接 KILL 另一条池内连接：

```sql
KILL CONNECTION <server_thread_id>;
```

下次借出被 KILL 的连接时，`ensureAlive()` 会发现 ping 失败并重连。
注意：这是“借出前修复”，会话状态会全部丢失；不要在事务进行中依赖这个机制。

### 8.8 统计观察

运行并发借还后查看：

```cpp
auto stats = pool.stats();
```

重点观察：

- `busy` 是否回到 0；
- `total` 是否始终不超过 `max_size`；
- `reused` 是否随顺序借还增长；
- 池满等待时 `waits` 是否增长；
- 超时实验后 `timeouts` 是否增长。

---

## 9. 线程安全与生命周期约束

**可以多线程共享的：**

- `SyncConnectionPool` 的 `acquire/acquireFor/release/stats/initialized`；
- `AsyncConnectionPool` 的 `submit/pending/poolStats`；
- 日志模块。

**不能多线程共享的：**

- 同一个 `MysqlConn`（除非外部保证串行）；
- 同一个 `ResultSet`；
- 同一个 `QueryResult`（查询结果本身是 move-only 的）；
- 一个 `ConnGuard` 不能同时被两个线程持有或释放。

**生命周期约束：**

- `ConnGuard` 必须比 `SyncConnectionPool` 先销毁；
- `AsyncConnectionPool` 析构会 `shutdown()`，回调不能在析构过程中再提交；
- `shutdown()` 后不能再次 `init()`；
- 池对象不可拷贝；
- 连接池对象建议放在 `main` 或明确的作用域中，不要在回调里捕获裸指针。

---

## 10. 常见问题排查

1. **CMake 找不到 `mysql.h`**
   - 安装 `libmysqlclient-dev` 或 `libmariadb-dev`；
   - 确认 `mysql_config` / `mariadb_config` 在 PATH 中；
   - 删除 `build/` 后重新配置。

2. **链接时报 `undefined reference to mysql_*`**
   - 不要手工只链接 `libmysqlpool.a`；
   - 用 `mysql_config --include --libs` 提供完整头文件和链接参数；
   - 注意 `libmysqlpool.a` 要放在 MySQL 库参数之前。

3. **`Access denied for user ...`**
   - 检查 `MYSQL_USER` / `MYSQL_PASSWORD`；
   - Ubuntu 的 root 可能只允许 `auth_socket`，改用 `sudo mysql` 创建 `pool` 账号；
   - 注意 `'pool'@'localhost'` 与 TCP `127.0.0.1` 的匹配问题，本地学习可用 `'pool'@'%'`。

4. **`caching_sha2_password` 认证失败**
   - MySQL 8 默认认证插件较新，客户端库过旧可能不支持；
   - 优先升级 libmysqlclient / libmariadb，或按驱动支持情况调整账号认证插件。

5. **`Too many connections`**
   - `max_size × 进程数` 不要超过服务端 `max_connections`；
   - 检查是否有 `ConnGuard` 长期未释放。

6. **关闭时打印“仍有连接在使用”**
   - 有 guard 没离开作用域；
   - 对 guard 使用明确的作用域，不要把它保存在比池更长寿的对象里。

7. **回调没有被调用 / 提交返回 false**
   - 队列满、池未初始化或正在关闭；
   - 回调版本返回 false 时不会执行回调，需要由调用方决定重试或记录。

8. **ASan 报告客户端库泄漏**
   - libmysqlclient 自身可能有全局分配；
   - 我们的进程没有显式调用 `mysql_library_end()`；
   - 如果泄漏栈只来自客户端库，可以排除噪声，但不要忽略自己代码的栈。

---

## 11. 已知限制与后续方向

- 增加 prepared statement 封装（`MYSQL_STMT` 的 RAII、参数绑定、结果绑定）；
- 支持 `mysql_use_result` 流式读取，降低大结果集的内存占用；
- 支持 `mysql_next_result()`，处理多结果集和存储过程；
- 增加事务守卫 `TransactionGuard`，禁止跨连接事务，并在析构时自动回滚；
- 把维护线程的建连移到锁外，减少网络抖动对借还路径的影响；
- 支持按负载动态调整连接数和 worker 数；
- 增加指标导出（创建/复用/等待/超时/队列长度）；
- 实现真正的非阻塞 MySQL 客户端 + 事件循环，减少 worker 阻塞；
- 增加 per-thread 的 `mysql_thread_init/mysql_thread_end` 封装；
- 增加自动化测试和示例目标（当前 CMake 只构建静态库）；
- 支持连接池级别的负载均衡、故障转移和读写分离。

---

## 12. 建议阅读顺序

1. `include/mysqlpool/mysql_config.h`：连接参数和默认值；
2. `include/mysqlpool/logger.h` 与 `src/logger.cpp`：日志如何做到线程安全；
3. `include/mysqlpool/query_result.h` 与 `src/query_result.cpp`：结果集 RAII；
4. `include/mysqlpool/mysql_conn.h` 与 `src/mysql_conn.cpp`：一条连接的生命周期；
5. `include/mysqlpool/sync_connection_pool.h`：先看状态字段和注释，再看
   `src/sync_connection_pool.cpp` 的 `acquireFor()`、`releaseSlot()`、
   `maintenanceLoop()` 和 `shutdown()`；
6. `include/mysqlpool/async_connection_pool.h` 与
   `src/async_connection_pool.cpp`：任务队列、worker、future 和排空关闭；
7. `CMakeLists.txt`：项目如何定位 libmysqlclient 和 pthread。

几个可以边读边想的问题：

- 为什么空闲队列使用 FIFO，而不是 LIFO？
- 为什么扩容时必须在锁内先 `total_++`？
- 健康检查为什么放在锁外？如果放在锁内会怎样？
- 如果 `ConnGuard` 比连接池活得久，会发生什么？
- 异步池队列满时应该阻塞、丢弃还是快速失败？各自的代价是什么？
- 为什么异步池在 `finishTask` 之前要释放 `ConnGuard`？
- 如果 worker 数和连接数不匹配，瓶颈会出现在哪里？
