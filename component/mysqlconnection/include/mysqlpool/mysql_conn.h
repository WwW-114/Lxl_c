#pragma once

#include "mysqlpool/mysql_config.h"
#include "mysqlpool/query_result.h"

#include <mysql.h>

#include <cstdint>
#include <string>

namespace mysqlpool {

// 对单条 MYSQL* 连接的 RAII 封装。
//
// 职责边界：
//   - 负责建立/关闭物理连接、执行 SQL、在 ping 失败后按保存的配置重连；
//   - 不负责复用、排队、超时和统计，这些由连接池负责；
//   - 本身不保证线程安全：同一时刻只能由一个线程使用。
class MysqlConn {
public:
    MysqlConn() = default;
    ~MysqlConn();

    MysqlConn(const MysqlConn&) = delete;
    MysqlConn& operator=(const MysqlConn&) = delete;
    MysqlConn(MysqlConn&& other) noexcept;
    MysqlConn& operator=(MysqlConn&& other) noexcept;

    bool connect(const MySQLConfig& config, std::string* error = nullptr);
    void close() noexcept;
    bool connected() const noexcept { return conn_ != nullptr; }

    // 仅发送 ping，不会重连。
    bool ping() noexcept;

    // ping 失败时关闭旧连接，并用 connect() 时保存的配置重连。
    // 注意：重连后的会话状态（临时表、事务、用户变量等）会丢失，
    // 连接池在把连接借给调用者之前做健康检查时，一定要意识到这一点。
    bool ensureAlive(std::string* error = nullptr);

    // 执行一条 SQL。自动区分“有结果集”和“只有影响行数”两种情况。
    QueryResult query(const std::string& sql);

    // 使用当前连接转义字符串。连接未建立时返回空字符串。
    std::string escape(const std::string& input);

    bool beginTransaction(std::string* error = nullptr);
    bool commit(std::string* error = nullptr);
    bool rollback(std::string* error = nullptr);

    // 连接池分配的编号，仅用于日志和教学观察。
    void setId(std::uint64_t id) noexcept { id_ = id; }
    std::uint64_t id() const noexcept { return id_; }

    // MySQL 服务端为该连接分配的线程 ID。
    // 连接复用时这个值保持不变，很适合观察“池里到底复用了哪几条连接”。
    unsigned long serverThreadId() const noexcept;

    std::string serverInfo() const;

    MYSQL* raw() noexcept { return conn_; }
    const MySQLConfig& config() const noexcept { return config_; }

private:
    MYSQL* conn_ = nullptr;
    MySQLConfig config_;
    std::uint64_t id_ = 0;
};

}  // namespace mysqlpool
