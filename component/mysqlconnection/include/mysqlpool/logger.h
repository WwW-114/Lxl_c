#pragma once

#include <sstream>
#include <string>
#include <utility>

namespace mysqlpool {

  enum class LogLevel {
      Debug = 0,
      Info = 1,
      Warn = 2,
      Error = 3,
  };

  void setLogLevel(LogLevel level);
  LogLevel getLogLevel();
  const char* logLevelName(LogLevel level);

  // 实际输出函数：加时间戳、级别、文件名和行号，并保证多线程下整行输出。
  void logMessage(LogLevel level, const char* file, int line, const std::string& message);

  // 变参日志入口。接受任意可流式输出的参数，例如：
  //   MYSQLPOOL_LOG_INFO("连接 #", id, " 已创建");
  template <typename... Args>
  void logFormat(LogLevel level, const char* file, int line, Args&&... args) {
      std::ostringstream oss;
      (oss << ... << std::forward<Args>(args));
      logMessage(level, file, line, oss.str());
  }

}  // namespace mysqlpool

#define MYSQLPOOL_LOG_DEBUG(...) \
    ::mysqlpool::logFormat(::mysqlpool::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)

#define MYSQLPOOL_LOG_INFO(...) \
    ::mysqlpool::logFormat(::mysqlpool::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)

#define MYSQLPOOL_LOG_WARN(...) \
    ::mysqlpool::logFormat(::mysqlpool::LogLevel::Warn, __FILE__, __LINE__, __VA_ARGS__)

#define MYSQLPOOL_LOG_ERROR(...) \
    ::mysqlpool::logFormat(::mysqlpool::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
