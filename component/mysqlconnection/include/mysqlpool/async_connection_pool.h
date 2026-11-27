#pragma once

#include "mysqlpool/mysql_config.h"
#include "mysqlpool/query_result.h"
#include "mysqlpool/sync_connection_pool.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace mysqlpool {

struct AsyncPoolOptions {
    // 执行 SQL 的工作线程数。每个工作线程同一时刻最多持有一条连接。
    std::size_t worker_threads = 2;

    // 任务队列上限；0 表示不限制。
    // 队列满时 submit() 直接返回“被拒绝”的结果，形成背压，而不是无限堆积。
    std::size_t max_pending = 1024;

    // 工作线程从同步连接池取连接的最长等待时间。
    std::chrono::milliseconds acquire_timeout{3000};
};

// 异步查询结果。
//
// 除了查询本身的结果，还带上了各阶段耗时，便于观察异步池的行为：
//
//   提交 --queue_time--> 工作线程开始执行 --acquire_time--> 拿到连接
//        --query_time--> SQL 执行完成 --total_time--> 总耗时
struct AsyncQueryResult {
    bool ok = false;
    std::string error;
    std::string sql;
    std::uint64_t connection_id = 0;

    std::chrono::microseconds queue_time{0};
    std::chrono::microseconds acquire_time{0};
    std::chrono::microseconds query_time{0};
    std::chrono::microseconds total_time{0};

    bool has_result_set = false;
    unsigned long long affected_rows = 0;
    unsigned long long insert_id = 0;
    ResultSet result;
};

// 异步连接池 = 同步连接池 + 任务队列 + 固定工作线程组。
//
// 重要说明：
//   libmysqlclient 的普通 C API 是阻塞的。这里的“异步”是指调用方线程
//   只负责提交任务并拿到 std::future，真正阻塞在 MySQL 网络 I/O 上的工作
//   由内部工作线程完成，而不是内核级的非阻塞 IO。
//
// 生命周期：
//   AsyncConnectionPool pool(config, sync_options, async_options);
//   pool.init();
//   auto future = pool.submit("SELECT ...");
//   auto result = future.get();
//   pool.shutdown();   // 先拒绝新任务，再排空队列、join 工作线程、关闭同步池
//
// 注意：
//   - 回调版本的回调在“工作线程”上执行，回调里不要做长时间阻塞操作，
//     更不要调用本池的 shutdown()，否则会 join 当前线程造成死锁；
//   - shutdown 会排空已经提交的任务，耗时取决于队列中最慢的 SQL。
class AsyncConnectionPool {
public:
    AsyncConnectionPool(MySQLConfig config,
                        SyncPoolOptions sync_options,
                        AsyncPoolOptions async_options = {});
    ~AsyncConnectionPool();

    AsyncConnectionPool(const AsyncConnectionPool&) = delete;
    AsyncConnectionPool& operator=(const AsyncConnectionPool&) = delete;

    bool init(std::string* error = nullptr);
    void shutdown();

    // 提交 SQL，返回 future。任务被拒绝（池未初始化/已关闭/队列满）时，
    // future 中会得到一个 ok == false 的结果，而不是 broken_promise。
    std::future<AsyncQueryResult> submit(const std::string& sql);

    // 回调版本。返回 false 表示任务没有被接收，此时回调不会被调用。
    // 回调在内部工作线程中执行。
    bool submit(const std::string& sql, std::function<void(AsyncQueryResult)> callback);

    // 当前排队中的任务数（不含正在执行的任务）。
    std::size_t pending() const;

    // 底层同步连接池的统计快照。
    SyncPoolStats poolStats() const;

    const AsyncPoolOptions& options() const noexcept { return options_; }

private:
    struct Task {
        std::string sql;
        std::chrono::steady_clock::time_point submitted_at;
        std::promise<AsyncQueryResult> promise;
        std::function<void(AsyncQueryResult)> callback;
        bool has_callback = false;
    };

    // 成功时把 task 移入队列；失败时 task 仍由调用方持有，便于写入错误结果。
    bool tryPush(std::unique_ptr<Task>& task, std::string* reason);

    void workerLoop();
    void executeTask(Task& task);
    void finishTask(Task& task, AsyncQueryResult result);

    MySQLConfig config_;
    SyncPoolOptions sync_options_;
    AsyncPoolOptions options_;
    std::unique_ptr<SyncConnectionPool> pool_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::unique_ptr<Task>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    bool started_ = false;
};

}  // namespace mysqlpool
