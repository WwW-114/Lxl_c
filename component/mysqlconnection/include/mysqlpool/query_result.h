#pragma once

#include <mysql.h>

#include <chrono>
#include <string>
#include <vector>

namespace mysqlpool {

// 对 MYSQL_RES* 的 RAII 封装。
//
// 说明：
//   - 只封装“缓冲结果集”（mysql_store_result），数据已经全部拉到客户端；
//   - 行数据在 ResultSet 析构时统一 mysql_free_result；
//   - 同一个 ResultSet 不是线程安全的，同一时刻只能被一个线程使用。
class ResultSet {
public:
    ResultSet() = default;
    explicit ResultSet(MYSQL_RES* result);
    ~ResultSet();

    ResultSet(ResultSet&& other) noexcept;
    ResultSet& operator=(ResultSet&& other) noexcept;
    ResultSet(const ResultSet&) = delete;
    ResultSet& operator=(const ResultSet&) = delete;

    bool valid() const noexcept { return result_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    unsigned int columnCount() const noexcept { return field_count_; }
    const std::vector<std::string>& columnNames() const noexcept { return column_names_; }
    unsigned long long rowCount() const;

    // 读取下一行。调用成功后可用 value()/currentRow()/isNull() 读取当前行。
    bool next();

    bool isNull(unsigned int column) const noexcept;

    // NULL 返回字符串 "NULL"；若需要区分 NULL 与普通的 "NULL" 文本，
    // 请先调用 isNull()。越界或未调用 next() 时返回空字符串。
    std::string value(unsigned int column) const;

    std::vector<std::string> currentRow() const;

    // 把游标移回第一行之前。
    bool rewind();

    // 以简单文本表格输出全部行。会从第一行开始读取到末尾，并在结束后
    // 把游标重置到开头。只用于学习演示，不适合超大结果集。
    std::string toString() const;

    // 放弃所有权，返回底层 MYSQL_RES*。调用者负责 mysql_free_result()。
    MYSQL_RES* release() noexcept;

private:
    void clear() noexcept;

    MYSQL_RES* result_ = nullptr;
    MYSQL_ROW row_ = nullptr;
    unsigned long* lengths_ = nullptr;
    unsigned int field_count_ = 0;
    std::vector<std::string> column_names_;
};

// 一次查询的完整结果。
// 对 SELECT 来说 has_result_set == true；对 INSERT/UPDATE/DDL 则只有影响行数。
struct QueryResult {
    bool ok = false;
    bool has_result_set = false;
    ResultSet result;
    unsigned long long affected_rows = 0;
    unsigned long long insert_id = 0;
    std::string error;
    std::chrono::microseconds elapsed{0};

    explicit operator bool() const noexcept { return ok; }
};

}  // namespace mysqlpool
