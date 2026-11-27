#include "mysqlpool/sync_connection_pool.h"

#include "mysqlpool/logger.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace mysqlpool {
namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

// ------------------------------------------------------------------
// ConnGuard
// ------------------------------------------------------------------

ConnGuard::ConnGuard(SyncConnectionPool* pool, std::size_t slot, MysqlConn* conn) noexcept
    : pool_(pool), slot_(slot), conn_(conn) {}

ConnGuard::~ConnGuard() {
    release();
}

ConnGuard::ConnGuard(ConnGuard&& other) noexcept
    : pool_(other.pool_), slot_(other.slot_), conn_(other.conn_) {
    other.pool_ = nullptr;
    other.conn_ = nullptr;
}

ConnGuard& ConnGuard::operator=(ConnGuard&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = other.pool_;
        slot_ = other.slot_;
        conn_ = other.conn_;

        other.pool_ = nullptr;
        other.conn_ = nullptr;
    }
    return *this;
}

void ConnGuard::release() noexcept {
    if (pool_ != nullptr && conn_ != nullptr) {
        pool_->releaseSlot(slot_);
    }
    pool_ = nullptr;
    conn_ = nullptr;
}

// ------------------------------------------------------------------
// SyncConnectionPool
// ------------------------------------------------------------------

SyncConnectionPool::SyncConnectionPool(MySQLConfig config, SyncPoolOptions options)
    : config_(std::move(config)), options_(std::move(options)) {
    if (options_.max_size == 0) {
        options_.max_size = 1;
    }
    if (options_.min_size > options_.max_size) {
        options_.min_size = options_.max_size;
    }
    if (options_.maintenance_interval.count() <= 0) {
        options_.maintenance_interval = std::chrono::milliseconds(1000);
    }
}

SyncConnectionPool::~SyncConnectionPool() {
    shutdown();
}

std::unique_ptr<MysqlConn> SyncConnectionPool::createConnection(std::string* error) {
    auto conn = std::make_unique<MysqlConn>();
    std::string local_error;
    if (!conn->connect(config_, &local_error)) {
        if (error != nullptr) {
            *error = local_error;
        }
        return nullptr;
    }
    return conn;
}

bool SyncConnectionPool::init(std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (started_) {
        return true;
    }
    if (stopping_) {
        if (error != nullptr) {
            *error = "连接池已关闭，不能再次初始化";
        }
        return false;
    }

    for (std::size_t i = 0; i < options_.min_size; ++i) {
        std::string create_error;
        std::unique_ptr<MysqlConn> conn = createConnection(&create_error);
        if (!conn) {
            if (error != nullptr) {
                *error = "预创建连接失败: " + create_error;
            }
            closeAllLocked();
            return false;
        }

        conn->setId(++next_conn_id_);

        auto slot = std::make_unique<Slot>();
        slot->conn = std::move(conn);
        slot->created_at = Clock::now();
        slot->last_returned = slot->created_at;

        idle_slots_.push_back(slots_.size());
        slots_.push_back(std::move(slot));
        ++total_;
        ++created_;
    }

    started_ = true;
    stopping_ = false;

    try {
        maintenance_thread_ = std::thread(&SyncConnectionPool::maintenanceLoop, this);
    } catch (const std::exception& e) {
        if (error != nullptr) {
            *error = std::string("启动维护线程失败: ") + e.what();
        }
        closeAllLocked();
        started_ = false;
        return false;
    }

    MYSQLPOOL_LOG_INFO("同步连接池初始化完成: ", config_.describe(),
                       " min=", options_.min_size,
                       " max=", options_.max_size,
                       " 预创建=", idle_slots_.size());
    return true;
}

void SyncConnectionPool::releaseSlot(std::size_t slot) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    if (slot >= slots_.size() || !slots_[slot]) {
        return;
    }

    Slot& entry = *slots_[slot];
    if (!entry.in_use) {
        return;
    }

    entry.in_use = false;
    entry.last_returned = Clock::now();

    if (busy_ > 0) {
        --busy_;
    }

    idle_slots_.push_back(slot);
    available_cv_.notify_all();
}

void SyncConnectionPool::destroySlotLocked(std::size_t slot) {
    if (slot >= slots_.size() || !slots_[slot]) {
        return;
    }

    if (slots_[slot]->in_use) {
        if (busy_ > 0) {
            --busy_;
        }
    }

    slots_[slot]->conn.reset();
    slots_[slot].reset();

    idle_slots_.erase(
        std::remove(idle_slots_.begin(), idle_slots_.end(), slot),
        idle_slots_.end());

    if (total_ > 0) {
        --total_;
    }
    ++destroyed_;
}

void SyncConnectionPool::closeAllLocked() {
    for (auto& slot : slots_) {
        if (slot) {
            slot->conn.reset();
        }
    }

    slots_.clear();
    idle_slots_.clear();
    total_ = 0;
    busy_ = 0;

    // creating_ 不在这里清零：它由正在锁外创建连接的线程负责递减。
    // 正常运行路径下 shutdown() 会先等待 creating_ 归零。
}

ConnGuard SyncConnectionPool::acquire() {
    return acquireFor(options_.acquire_timeout);
}

ConnGuard SyncConnectionPool::acquireFor(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);

    if (!started_ || stopping_) {
        return ConnGuard();
    }

    bool waited = false;
    bool counted_wait = false;

    for (;;) {
        // 1. 优先复用空闲连接
        if (!idle_slots_.empty()) {
            const std::size_t index = idle_slots_.front();
            idle_slots_.pop_front();

            if (index >= slots_.size() || !slots_[index]) {
                continue;
            }

            Slot& slot = *slots_[index];
            slot.in_use = true;
            ++busy_;

            MysqlConn* conn = slot.conn.get();
            lock.unlock();
            
            bool alive = true;
            std::string check_error;
            if (options_.validate_on_acquire) {
                alive = conn->ensureAlive(&check_error);
            }

            if (alive) {
                ++reused_;
                return ConnGuard(this, index, conn);
            }

            const std::uint64_t failed_id = conn->id();
            MYSQLPOOL_LOG_WARN("连接 #", failed_id,
                               " 健康检查失败（", check_error,
                               "），销毁后重新获取");
            ++validation_failures_;

            lock.lock();
            destroySlotLocked(index);
            available_cv_.notify_all();

            if (Clock::now() >= deadline) {
                ++timeouts_;
                return ConnGuard();
            }
            continue;
        }

        // 2. 没有空闲连接，但还有扩容空间
        if (total_ < options_.max_size && (!waited || Clock::now() < deadline)) {
            // 先在锁内占住名额，再在锁外建立连接，避免并发扩容超过 max_size。
            ++total_;
            ++creating_;
            lock.unlock();

            std::string create_error;
            std::unique_ptr<MysqlConn> conn = createConnection(&create_error);

            lock.lock();
            if (creating_ > 0) {
                --creating_;
            }
            available_cv_.notify_all();

            if (!conn) {
                if (total_ > 0) {
                    --total_;
                }
                MYSQLPOOL_LOG_ERROR("创建 MySQL 连接失败: ", create_error);

                if (!started_ || stopping_) {
                    return ConnGuard();
                }
                if (Clock::now() >= deadline) {
                    ++timeouts_;
                    return ConnGuard();
                }

                waited = true;
                available_cv_.wait_for(lock, std::chrono::milliseconds(200));

                if (!started_ || stopping_) {
                    return ConnGuard();
                }
                continue;
            }

            if (!started_ || stopping_) {
                if (total_ > 0) {
                    --total_;
                }
                lock.unlock();
                return ConnGuard();
            }

            conn->setId(++next_conn_id_);

            const std::size_t index = slots_.size();
            auto slot = std::make_unique<Slot>();
            slot->conn = std::move(conn);
            slot->in_use = true;
            slot->created_at = Clock::now();
            slot->last_returned = slot->created_at;

            MysqlConn* raw = slot->conn.get();
            slots_.push_back(std::move(slot));
            ++busy_;
            ++created_;

            return ConnGuard(this, index, raw);
        }

        // 3. 池已满且全部借出：等待别人归还
        if (!counted_wait) {
            counted_wait = true;
            ++waits_;
        }

        if (Clock::now() >= deadline) {
            ++timeouts_;
            return ConnGuard();
        }

        waited = true;
        available_cv_.wait_until(lock, deadline);

        if (!started_ || stopping_) {
            return ConnGuard();
        }
    }
}

SyncPoolStats SyncConnectionPool::stats() const {
    SyncPoolStats result;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        result.total = total_;
        result.idle = idle_slots_.size();
        result.busy = busy_;
        result.min_size = options_.min_size;
        result.max_size = options_.max_size;
    }

    result.created = created_.load(std::memory_order_relaxed);
    result.destroyed = destroyed_.load(std::memory_order_relaxed);
    result.reused = reused_.load(std::memory_order_relaxed);
    result.waits = waits_.load(std::memory_order_relaxed);
    result.timeouts = timeouts_.load(std::memory_order_relaxed);
    result.validation_failures = validation_failures_.load(std::memory_order_relaxed);

    return result;
}

bool SyncConnectionPool::initialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && !stopping_;
}

void SyncConnectionPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!started_ && !maintenance_thread_.joinable()) {
            stopping_ = true;
            closeAllLocked();
            return;
        }

        stopping_ = true;
        available_cv_.notify_all();
        maintenance_cv_.notify_all();
    }

    if (maintenance_thread_.joinable()) {
        maintenance_thread_.join();
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);

        const auto deadline = Clock::now() + std::chrono::seconds(5);
        available_cv_.wait_until(lock, deadline, [this] {
            return busy_ == 0 && creating_ == 0;
        });

        if (busy_ > 0 || creating_ > 0) {
            MYSQLPOOL_LOG_WARN(
                "关闭同步连接池时仍有连接在使用（busy=", busy_,
                ", creating=", creating_,
                "），将强制关闭。请确保 ConnGuard 的生命周期短于连接池对象。");
        }

        closeAllLocked();
        started_ = false;
    }
}

void SyncConnectionPool::maintenanceLoop() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stopping_) {
        maintenance_cv_.wait_for(lock, options_.maintenance_interval);

        if (stopping_) {
            break;
        }

        const auto now = Clock::now();

        std::vector<std::size_t> expired;
        for (std::size_t index : idle_slots_) {
            if (index >= slots_.size() || !slots_[index]) {
                continue;
            }

            const Slot& slot = *slots_[index];
            const bool idle_expired =
                options_.idle_timeout.count() > 0 &&
                (now - slot.last_returned) >= options_.idle_timeout;

            const bool lifetime_expired =
                options_.max_lifetime.count() > 0 &&
                (now - slot.created_at) >= options_.max_lifetime;

            if (idle_expired || lifetime_expired) {
                expired.push_back(index);
            }
        }

        for (std::size_t index : expired) {
            if (total_ <= options_.min_size) {
                break;
            }
            if (index >= slots_.size() || !slots_[index]) {
                continue;
            }

            const std::uint64_t conn_id =
                slots_[index]->conn ? slots_[index]->conn->id() : 0;

            destroySlotLocked(index);
            MYSQLPOOL_LOG_DEBUG("维护线程回收空闲连接 #", conn_id,
                                "，当前总数 ", total_);
        }

        while (total_ < options_.min_size) {
            std::string error;
            std::unique_ptr<MysqlConn> conn = createConnection(&error);
            if (!conn) {
                MYSQLPOOL_LOG_WARN("维护线程补充连接失败: ", error);
                break;
            }

            conn->setId(++next_conn_id_);

            auto slot = std::make_unique<Slot>();
            slot->conn = std::move(conn);
            slot->created_at = Clock::now();
            slot->last_returned = slot->created_at;

            idle_slots_.push_back(slots_.size());
            slots_.push_back(std::move(slot));
            ++total_;
            ++created_;

            MYSQLPOOL_LOG_DEBUG("维护线程补充连接 #",
                                slots_.back()->conn->id());
        }
    }
}

}  // namespace mysqlpool
