#include "mysqlpool/mysql_conn.h"

#include "mysqlpool/logger.h"

#include <mysql.h>

#include <chrono>
#include <mutex>
#include <utility>

namespace mysqlpool {
namespace {

// libmysqlclient 的全局初始化只需执行一次。
// mysql_library_init() 不是线程安全的，用 call_once 串行化。
bool ensureLibraryInitialized(std::string* error) {
    static std::once_flag once;
    static bool initialized = false;
    static std::string init_error;

    std::call_once(once, [] {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            init_error = "mysql_library_init() 失败";
        } else {
            initialized = true;
        }
    });

    if (!initialized && error != nullptr) {
        *error = init_error;
    }
    return initialized;
}

bool setError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

}  // namespace

MysqlConn::~MysqlConn() {
    close();
}

MysqlConn::MysqlConn(MysqlConn&& other) noexcept
    : conn_(other.conn_),
      config_(std::move(other.config_)),
      id_(other.id_) {
    other.conn_ = nullptr;
    other.id_ = 0;
}

MysqlConn& MysqlConn::operator=(MysqlConn&& other) noexcept {
    if (this != &other) {
        close();
        conn_ = other.conn_;
        config_ = std::move(other.config_);
        id_ = other.id_;

        other.conn_ = nullptr;
        other.id_ = 0;
    }
    return *this;
}

bool MysqlConn::connect(const MySQLConfig& config, std::string* error) {
    close();

    std::string library_error;
    if (!ensureLibraryInitialized(&library_error)) {
        return setError(error, library_error);
    }

    MYSQL* conn = mysql_init(nullptr);
    if (conn == nullptr) {
        return setError(error, "mysql_init() 返回空指针（内存不足）");
    }

    unsigned int connect_timeout = config.connect_timeout_sec;
    unsigned int read_timeout = config.read_timeout_sec;
    unsigned int write_timeout = config.write_timeout_sec;

    if (connect_timeout > 0) {
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    }
    if (read_timeout > 0) {
        mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &read_timeout);
    }
    if (write_timeout > 0) {
        mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout);
    }
    if (!config.charset.empty()) {
        mysql_options(conn, MYSQL_SET_CHARSET_NAME, config.charset.c_str());
    }

    MYSQL* connected = mysql_real_connect(conn,
                                          config.host.c_str(),
                                          config.user.c_str(),
                                          config.password.c_str(),
                                          config.database.c_str(),
                                          config.port,
                                          nullptr,
                                          0);
    if (connected == nullptr) {
        const std::string message = mysql_error(conn);
        mysql_close(conn);
        return setError(error,
                        message.empty() ? "mysql_real_connect() 失败" : message);
    }

    conn_ = conn;
    config_ = config;
    return true;
}

void MysqlConn::close() noexcept {
    if (conn_ != nullptr) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

bool MysqlConn::ping() noexcept {
    return conn_ != nullptr && mysql_ping(conn_) == 0;
}

bool MysqlConn::ensureAlive(std::string* error) {
    if (conn_ == nullptr) {
        return setError(error, "连接未建立");
    }

    if (mysql_ping(conn_) == 0) {
        return true;
    }

    const std::string old_error = mysql_error(conn_);
    MYSQLPOOL_LOG_DEBUG("连接 #", id_, " ping 失败: ", old_error, "，尝试重连");

    close();
    return connect(config_, error);
}

QueryResult MysqlConn::query(const std::string& sql) {
    QueryResult result;
    const auto started = std::chrono::steady_clock::now();

    if (conn_ == nullptr) {
        result.error = "连接未建立";
        return result;
    }

    if (mysql_real_query(conn_, sql.c_str(),
                         static_cast<unsigned long>(sql.size())) != 0) {
        result.error = mysql_error(conn_);
        result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        return result;
    }

    result.affected_rows =
        static_cast<unsigned long long>(mysql_affected_rows(conn_));
    result.insert_id =
        static_cast<unsigned long long>(mysql_insert_id(conn_));

    MYSQL_RES* raw_result = mysql_store_result(conn_);
    if (raw_result != nullptr) {
        result.has_result_set = true;
        result.result = ResultSet(raw_result);
        result.ok = true;
    } else if (mysql_field_count(conn_) == 0) {
        // 没有结果集是正常情况，例如 INSERT / UPDATE / DDL。
        result.ok = true;
    } else {
        // 有字段却没拿到结果集，说明 mysql_store_result 失败。
        result.error = mysql_error(conn_);
    }

    result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

std::string MysqlConn::escape(const std::string& input) {
    if (conn_ == nullptr) {
        return {};
    }

    std::string output(input.size() * 2 + 1, '\0');
    const unsigned long length = mysql_real_escape_string(
        conn_,
        &output[0],
        input.data(),
        static_cast<unsigned long>(input.size()));
    output.resize(length);
    return output;
}

bool MysqlConn::beginTransaction(std::string* error) {
    QueryResult result = query("START TRANSACTION");
    if (!result.ok && error != nullptr) {
        *error = result.error;
    }
    return result.ok;
}

bool MysqlConn::commit(std::string* error) {
    QueryResult result = query("COMMIT");
    if (!result.ok && error != nullptr) {
        *error = result.error;
    }
    return result.ok;
}

bool MysqlConn::rollback(std::string* error) {
    QueryResult result = query("ROLLBACK");
    if (!result.ok && error != nullptr) {
        *error = result.error;
    }
    return result.ok;
}

unsigned long MysqlConn::serverThreadId() const noexcept {
    return (conn_ != nullptr) ? mysql_thread_id(conn_) : 0UL;
}

std::string MysqlConn::serverInfo() const {
    if (conn_ == nullptr) {
        return {};
    }
    const char* info = mysql_get_server_info(conn_);
    return (info != nullptr) ? info : "";
}

}  // namespace mysqlpool
