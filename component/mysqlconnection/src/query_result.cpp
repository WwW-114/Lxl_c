#include "mysqlpool/query_result.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace mysqlpool {

ResultSet::ResultSet(MYSQL_RES* result) : result_(result) {
    if (result_ == nullptr) {
        return;
    }

    field_count_ = mysql_num_fields(result_);
    MYSQL_FIELD* fields = mysql_fetch_fields(result_);
    if (fields != nullptr) {
        column_names_.reserve(field_count_);
        for (unsigned int i = 0; i < field_count_; ++i) {
            column_names_.emplace_back(fields[i].name != nullptr ? fields[i].name : "");
        }
    }
}

ResultSet::~ResultSet() {
    clear();
}

ResultSet::ResultSet(ResultSet&& other) noexcept
    : result_(other.result_),
      row_(other.row_),
      lengths_(other.lengths_),
      field_count_(other.field_count_),
      column_names_(std::move(other.column_names_)) {
    other.result_ = nullptr;
    other.row_ = nullptr;
    other.lengths_ = nullptr;
    other.field_count_ = 0;
}

ResultSet& ResultSet::operator=(ResultSet&& other) noexcept {
    if (this != &other) {
        clear();
        result_ = other.result_;
        row_ = other.row_;
        lengths_ = other.lengths_;
        field_count_ = other.field_count_;
        column_names_ = std::move(other.column_names_);

        other.result_ = nullptr;
        other.row_ = nullptr;
        other.lengths_ = nullptr;
        other.field_count_ = 0;
    }
    return *this;
}

void ResultSet::clear() noexcept {
    if (result_ != nullptr) {
        mysql_free_result(result_);
    }
    result_ = nullptr;
    row_ = nullptr;
    lengths_ = nullptr;
    field_count_ = 0;
    column_names_.clear();
}

unsigned long long ResultSet::rowCount() const {
    if (result_ == nullptr) {
        return 0;
    }
    return static_cast<unsigned long long>(mysql_num_rows(result_));
}

bool ResultSet::next() {
    if (result_ == nullptr) {
        return false;
    }

    row_ = mysql_fetch_row(result_);
    if (row_ == nullptr) {
        lengths_ = nullptr;
        return false;
    }

    lengths_ = mysql_fetch_lengths(result_);
    return true;
}

bool ResultSet::isNull(unsigned int column) const noexcept {
    return row_ != nullptr && column < field_count_ && row_[column] == nullptr;
}

std::string ResultSet::value(unsigned int column) const {
    if (row_ == nullptr || column >= field_count_) {
        return {};
    }

    if (row_[column] == nullptr) {
        return "NULL";
    }

    const unsigned long length =
        (lengths_ != nullptr)
            ? lengths_[column]
            : static_cast<unsigned long>(std::strlen(row_[column]));

    return std::string(row_[column], length);
}

std::vector<std::string> ResultSet::currentRow() const {
    std::vector<std::string> row;
    if (row_ == nullptr) {
        return row;
    }

    row.reserve(field_count_);
    for (unsigned int i = 0; i < field_count_; ++i) {
        row.push_back(value(i));
    }
    return row;
}

bool ResultSet::rewind() {
    if (result_ == nullptr) {
        return false;
    }
    mysql_data_seek(result_, 0);
    row_ = nullptr;
    lengths_ = nullptr;
    return true;
}

std::string ResultSet::toString() const {
    if (result_ == nullptr) {
        return "(无结果集)\n";
    }
    if (field_count_ == 0) {
        return "(空结果集)\n";
    }

    const unsigned int columns = field_count_;

    std::vector<std::string> headers = column_names_;
    if (headers.size() < columns) {
        headers.resize(columns);
    }
    for (unsigned int i = 0; i < columns; ++i) {
        if (headers[i].empty()) {
            headers[i] = "col" + std::to_string(i + 1);
        }
    }

    std::vector<std::vector<std::string>> rows;
    mysql_data_seek(result_, 0);

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result_)) != nullptr) {
        unsigned long* lengths = mysql_fetch_lengths(result_);
        std::vector<std::string> cells;
        cells.reserve(columns);

        for (unsigned int i = 0; i < columns; ++i) {
            if (row[i] == nullptr) {
                cells.emplace_back("NULL");
            } else {
                const unsigned long length =
                    (lengths != nullptr)
                        ? lengths[i]
                        : static_cast<unsigned long>(std::strlen(row[i]));
                cells.emplace_back(row[i], length);
            }
        }

        rows.push_back(std::move(cells));
    }

    mysql_data_seek(result_, 0);

    std::vector<std::size_t> widths;
    widths.reserve(columns);
    for (unsigned int i = 0; i < columns; ++i) {
        std::size_t width = headers[i].size();
        for (const auto& cells : rows) {
            if (i < cells.size()) {
                width = std::max(width, cells[i].size());
            }
        }
        widths.push_back(width);
    }

    std::ostringstream out;

    auto append_separator = [&out, &widths]() {
        out << '+';
        for (std::size_t width : widths) {
            out << std::string(width + 2, '-') << '+';
        }
        out << '\n';
    };

    auto append_row = [&out, &widths, columns](const std::vector<std::string>& cells) {
        out << '|';
        for (unsigned int i = 0; i < columns; ++i) {
            const std::string cell = (i < cells.size()) ? cells[i] : std::string();
            out << ' ' << cell;
            if (cell.size() < widths[i]) {
                out << std::string(widths[i] - cell.size(), ' ');
            }
            out << " |";
        }
        out << '\n';
    };

    append_separator();
    append_row(headers);
    append_separator();
    for (const auto& cells : rows) {
        append_row(cells);
    }
    append_separator();
    out << rows.size() << " row(s)\n";

    return out.str();
}

MYSQL_RES* ResultSet::release() noexcept {
    MYSQL_RES* result = result_;
    result_ = nullptr;
    row_ = nullptr;
    lengths_ = nullptr;
    field_count_ = 0;
    column_names_.clear();
    return result;
}

}  // namespace mysqlpool
