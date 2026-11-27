#include "mysqlpool/logger.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace {

std::atomic<int> g_level{static_cast<int>(mysqlpool::LogLevel::Info)};
std::mutex g_output_mutex;

}  // namespace

namespace mysqlpool {

void setLogLevel(LogLevel level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel getLogLevel() {
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

void logMessage(LogLevel level, const char* file, int line, const std::string& message) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) {
        return;
    }

    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t time = system_clock::to_time_t(now);

    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    ::localtime_r(&time, &local);
#endif

    char timestamp[32] = {0};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);

    const char* base = (file != nullptr) ? file : "";
    const char* slash = std::strrchr(base, '/');
    const char* backslash = std::strrchr(base, '\\');
    if (backslash != nullptr && (slash == nullptr || backslash > slash)) {
        slash = backslash;
    }
    const char* name = (slash != nullptr) ? slash + 1 : base;

    std::lock_guard<std::mutex> lock(g_output_mutex);
    std::fprintf(stderr, "[%s.%03lld] [%-5s] %s (%s:%d)\n",
                 timestamp,
                 static_cast<long long>(millis.count()),
                 logLevelName(level),
                 message.c_str(),
                 name,
                 line);
    std::fflush(stderr);
}

}  // namespace mysqlpool
