#pragma once

#include "mysqlpool/mysql_config.h"
#include "mysqlpool/mysql_conn.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mysqlpool {

// 同步连接池的参数。时间单位统一为毫秒。
struct SyncPoolOptions {
    // 初始化时预创建、维护时尽量保持的连接数。
    std::size_t min_size = 2;

    // 池中连接总数上限（含正在创建中的连接）。
    std::size_t max_size = 8;

    // acquire() 默认等待空闲连接的最长时间。
    std::chrono::milliseconds acquire_timeout{3000};

    // 空闲超过该时间的连接会被维护线程回收；
    // 0 表示不按空闲时间回收。
    std::chrono::milliseconds idle_timeout{60000};

    // 连接从创建开始的最大存活时间；
    // 0 表示不限制。用于躲避服务端 wait_timeout 或负载均衡后的旧连接。
    std::chrono::milliseconds max_lifetime{0};

    // 维护线程的巡检周期。
    std::chrono::milliseconds maintenance_interval{10000};

    // 每次借出连接前是否 ping 一次。
    // 更安全，但每次借出多一次网络往返。
    bool validate_on_acquire = true;
};

// 快照式统计。数值在调用 stats() 时读取，之后池继续变化不会影响该副本。
struct SyncPoolStats {
    std::size_t total = 0;   // 池内连接总数（含使用中、创建中）
    std::size_t idle = 0;    // 空闲连接数
    std::size_t busy = 0;    // 已借出连接数
    std::size_t min_size = 0;
    std::size_t max_size = 0;

    std::uint64_t created = 0;
    std::uint64_t destroyed = 0;
    std::uint64_t reused = 0;
    std::uint64_t waits = 0;               // 进入“等待归还”状态的次数
    std::uint64_t timeouts = 0;            // 获取连接超时次数
    std::uint64_t validation_failures = 0; // 借出前健康检查失败的次数
};

class SyncConnectionPool;

// 借出连接的 RAII 句柄。
//
// 作用域结束时自动把连接还给池，即使中途抛异常也不会漏还。
// 要求：ConnGuard 的生命周期不能超过所属的 SyncConnectionPool。
class ConnGuard {
public:
    ConnGuard() noexcept = default;
    ~ConnGuard();

    ConnGuard(ConnGuard&& other) noexcept;
    ConnGuard& operator=(ConnGuard&& other) noexcept;
    ConnGuard(const ConnGuard&) = delete;
    ConnGuard& operator=(const ConnGuard&) = delete;

    explicit operator bool() const noexcept { return conn_ != nullptr; }

    MysqlConn& operator*() const { return *conn_; }
    MysqlConn* operator->() const noexcept { return conn_; }
    MysqlConn* get() const noexcept { return conn_; }

    // 提前归还。析构时也会自动调用。
    void release() noexcept;

    std::uint64_t connectionId() const noexcept { return conn_ != nullptr ? conn_->id() : 0; }

private:
    friend class SyncConnectionPool;

    ConnGuard(SyncConnectionPool* pool, std::size_t slot, MysqlConn* conn) noexcept;

    SyncConnectionPool* pool_ = nullptr;
    std::size_t slot_ = 0;
    MysqlConn* conn_ = nullptr;
};

// 同步 MySQL 连接池。
//
// 多个业务线程共享一个池：线程调用 acquire() 借出连接，ConnGuard 析构时归还。
// 当池中有空闲连接时立即返回；池未满时可以新建；池满且全部借出时阻塞等待，
// 直到有人归还或超时。
//
// 生命周期：
//   SyncConnectionPool pool(config, options);
//   pool.init();            // 预创建 min_size 条连接并启动维护线程
//   { auto guard = pool.acquire(); ... }   // 作用域结束自动归还
//   pool.shutdown();        // 可选，析构函数也会调用
//
// 注意：shutdown 会等待已借出的连接归还（最多等待一段时间），因此必须保证
// 业务线程先结束或先释放 ConnGuard，再销毁连接池。
class SyncConnectionPool {
public:
    SyncConnectionPool(MySQLConfig config, SyncPoolOptions options = {});
    ~SyncConnectionPool();

    SyncConnectionPool(const SyncConnectionPool&) = delete;
    SyncConnectionPool& operator=(const SyncConnectionPool&) = delete;

    // 显式初始化：创建 min_size 条物理连接并启动维护线程。
    // 可重复调用，只有第一次真正生效。
    bool init(std::string* error = nullptr);

    // 停止维护线程并关闭所有连接。可重复调用。
    void shutdown();

    bool initialized() const;

    // 使用 options().acquire_timeout 作为超时时间。
    ConnGuard acquire();
    ConnGuard acquireFor(std::chrono::milliseconds timeout);

    SyncPoolStats stats() const;

    const MySQLConfig& config() const noexcept { return config_; }
    const SyncPoolOptions& options() const noexcept { return options_; }

private:
    friend class ConnGuard;

    struct Slot {
        std::unique_ptr<MysqlConn> conn;
        bool in_use = false;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_returned;
    };

    // 注意：createConnection() 只负责建立物理连接，不修改 next_conn_id_。
    // 连接编号在持有 mutex_ 时统一分配，避免并发创建时产生重复编号。
    std::unique_ptr<MysqlConn> createConnection(std::string* error);

    void releaseSlot(std::size_t slot) noexcept;
    void destroySlotLocked(std::size_t slot);
    void closeAllLocked();
    void maintenanceLoop();

    MySQLConfig config_;
    SyncPoolOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable available_cv_;    // 等空闲连接 / 等池关闭
    std::condition_variable maintenance_cv_;  // 维护线程的定时等待
    std::vector<std::unique_ptr<Slot>> slots_;
    std::deque<std::size_t> idle_slots_;      // FIFO：越早归还的越早复用
    std::size_t total_ = 0;                   // 已有连接 + 正在创建的名额
    std::size_t busy_ = 0;                    // 已借出连接数
    std::size_t creating_ = 0;                // 正在锁外创建的连接数
    std::uint64_t next_conn_id_ = 0;
    bool stopping_ = false;
    bool started_ = false;
    std::thread maintenance_thread_;

    std::atomic<std::uint64_t> created_{0};
    std::atomic<std::uint64_t> destroyed_{0};
    std::atomic<std::uint64_t> reused_{0};
    std::atomic<std::uint64_t> waits_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> validation_failures_{0};
};

}  // namespace mysqlpool

