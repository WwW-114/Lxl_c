#include "mysqlpool/mysql_config.h"

#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace mysqlpool {
namespace {

std::string envString(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::string(value);
}

unsigned int envUInt(const char* name, unsigned int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > 0xFFFFFFFFUL) {
        return fallback;
    }
    return static_cast<unsigned int>(parsed);
}

}  // namespace

std::string MySQLConfig::describe() const {
    std::ostringstream oss;
    oss << user << '@' << host << ':' << port << '/' << database;
    return oss.str();
}

MySQLConfig configFromEnv() {
    MySQLConfig config;
    config.host = envString("MYSQL_HOST", config.host);
    config.port = envUInt("MYSQL_PORT", config.port);
    config.user = envString("MYSQL_USER", config.user);
    config.password = envString("MYSQL_PASSWORD", config.password);
    config.database = envString("MYSQL_DATABASE", config.database);
    config.charset = envString("MYSQL_CHARSET", config.charset);
    config.connect_timeout_sec = envUInt("MYSQL_CONNECT_TIMEOUT", config.connect_timeout_sec);
    config.read_timeout_sec = envUInt("MYSQL_READ_TIMEOUT", config.read_timeout_sec);
    config.write_timeout_sec = envUInt("MYSQL_WRITE_TIMEOUT", config.write_timeout_sec);
    return config;
}

}  // namespace mysqlpool
