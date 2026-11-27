#include "mysqlpool/async_connection_pool.h"

#include "mysqlpool/logger.h"

#include <exception>
#include <utility>

namespace mysqlpool {
namespace {

AsyncQueryResult makeErrorResult(const std::string& sql, const std::string& error) {
    AsyncQueryResult result;
    result.sql = sql;
    result.error = error;
    return result;
}

}  // namespace

AsyncConnectionPool::AsyncConnectionPool(MySQLConfig config,
                                         SyncPoolOptions sync_options,
                                         AsyncPoolOptions async_options)
    : config_(std::move(config)),
      sync_options_(std::move(sync_options)),
      options_(std::move(async_options)) {
    if (options_.worker_threads == 0) {
        options_.worker_threads = 1;
    }
}

AsyncConnectionPool::~AsyncConnectionPool() {
    shutdown();
}

bool AsyncConnectionPool::init(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (started_) {
            return true;
        }
        if (stopping_) {
            if (error != nullptr) {
                *error = "连接池已关闭，不能再次初始化";
            }
            return false;
        }
    }

    if (options_.worker_threads == 0) {
        options_.worker_threads = 1;
    }

    auto pool = std::make_unique<SyncConnectionPool>(config_, sync_options_);
    if (!pool->init(error)) {
        return false;
    }

    std::vector<std::thread> workers;
    workers.reserve(options_.worker_threads);

    try {
        for (std::size_t i = 0; i < options_.worker_threads; ++i) {
            workers.emplace_back(&AsyncConnectionPool::workerLoop, this);
        }
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stopping_ = true;
        }
        queue_cv_.notify_all();

        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        pool->shutdown();

        if (error != nullptr) {
            *error = std::string("启动工作线程失败: ") + e.what();
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stopping_ = false;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pool_ = std::move(pool);
        workers_ = std::move(workers);
        started_ = true;
        stopping_ = false;
    }

    MYSQLPOOL_LOG_INFO("异步连接池初始化完成: workers=", options_.worker_threads,
                       " max_pending=", options_.max_pending,
                       " db=", config_.describe());
    return true;
}

void AsyncConnectionPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (!started_ && workers_.empty() && !pool_) {
            stopping_ = true;
            return;
        }

        stopping_ = true;
    }
    queue_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    std::unique_ptr<SyncConnectionPool> pool;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pool = std::move(pool_);
    }
    if (pool) {
        pool->shutdown();
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        started_ = false;
    }
}

bool AsyncConnectionPool::tryPush(std::unique_ptr<Task>& task, std::string* reason) {
    if (reason != nullptr) {
        reason->clear();
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (stopping_) {
            if (reason != nullptr) {
                *reason = "连接池已关闭或正在关闭";
            }
            return false;
        }

        if (!started_) {
            if (reason != nullptr) {
                *reason = "连接池尚未初始化";
            }
            return false;
        }

        if (options_.max_pending > 0 && tasks_.size() >= options_.max_pending) {
            if (reason != nullptr) {
                *reason = "任务队列已满（背压）";
            }
            return false;
        }

        tasks_.push(std::move(task));
    }

    queue_cv_.notify_one();
    return true;
}

std::future<AsyncQueryResult> AsyncConnectionPool::submit(const std::string& sql) {
    auto task = std::make_unique<Task>();
    task->sql = sql;
    task->submitted_at = std::chrono::steady_clock::now();

    std::future<AsyncQueryResult> future = task->promise.get_future();

    std::string reason;
    if (!tryPush(task, &reason)) {
        task->promise.set_value(makeErrorResult(sql, reason));
    }

    return future;
}

bool AsyncConnectionPool::submit(const std::string& sql,
                                 std::function<void(AsyncQueryResult)> callback) {
    auto task = std::make_unique<Task>();
    task->sql = sql;
    task->submitted_at = std::chrono::steady_clock::now();
    task->callback = std::move(callback);
    task->has_callback = true;

    std::string reason;
    if (!tryPush(task, &reason)) {
        MYSQLPOOL_LOG_WARN("异步任务被拒绝: ", reason);
        return false;
    }
    return true;
}

std::size_t AsyncConnectionPool::pending() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

SyncPoolStats AsyncConnectionPool::poolStats() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return pool_ ? pool_->stats() : SyncPoolStats{};
}

void AsyncConnectionPool::workerLoop() {
    for (;;) {
        std::unique_ptr<Task> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (tasks_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            executeTask(*task);
        } catch (const std::exception& e) {
            AsyncQueryResult failed =
                makeErrorResult(task->sql,
                                std::string("任务执行异常: ") + e.what());
            failed.total_time = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - task->submitted_at);
            finishTask(*task, std::move(failed));
        } catch (...) {
            AsyncQueryResult failed =
                makeErrorResult(task->sql, "任务执行异常（未知类型）");
            finishTask(*task, std::move(failed));
        }
    }
}

void AsyncConnectionPool::executeTask(Task& task) {
    AsyncQueryResult result;
    result.sql = task.sql;

    const auto submitted = task.submitted_at;
    const auto start = std::chrono::steady_clock::now();
    result.queue_time =
        std::chrono::duration_cast<std::chrono::microseconds>(start - submitted);

    SyncConnectionPool* pool = nullptr;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pool = pool_.get();
    }

    if (pool == nullptr) {
        result.error = "连接池已关闭";
        result.total_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - submitted);
        finishTask(task, std::move(result));
        return;
    }

    {
        ConnGuard guard = pool->acquireFor(options_.acquire_timeout);
        const auto acquired = std::chrono::steady_clock::now();
        result.acquire_time =
            std::chrono::duration_cast<std::chrono::microseconds>(acquired - start);

        if (!guard) {
            result.ok = false;
            result.error = "从连接池获取连接失败（超时或池已关闭）";
        } else {
            result.connection_id = guard->id();

            QueryResult query_result = guard->query(task.sql);
            const auto finished = std::chrono::steady_clock::now();

            result.query_time = std::chrono::duration_cast<std::chrono::microseconds>(
                finished - acquired);
            result.ok = query_result.ok;
            result.error = std::move(query_result.error);
            result.has_result_set = query_result.has_result_set;
            result.affected_rows = query_result.affected_rows;
            result.insert_id = query_result.insert_id;
            result.result = std::move(query_result.result);
        }
    }
    // 连接已经在上面的大括号结束时归还，避免回调/等待 future 的线程
    // 阻塞期间一直占着物理连接。

    result.total_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - submitted);

    finishTask(task, std::move(result));
}

void AsyncConnectionPool::finishTask(Task& task, AsyncQueryResult result) {
    if (task.has_callback) {
        try {
            if (task.callback) {
                task.callback(std::move(result));
            }
        } catch (const std::exception& e) {
            MYSQLPOOL_LOG_ERROR("异步回调抛出异常: ", e.what());
        } catch (...) {
            MYSQLPOOL_LOG_ERROR("异步回调抛出未知异常");
        }
        return;
    }

    try {
        task.promise.set_value(std::move(result));
    } catch (const std::future_error& e) {
        MYSQLPOOL_LOG_WARN("写入 future 失败: ", e.what());
    }
}

}  // namespace mysqlpool
