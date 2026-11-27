#pragma once

#include <string>

namespace mysqlpool {

  // 建立一条物理 MySQL 连接所需的参数。
  // 连接池只负责“复用连接”，这些参数统一下发给 MysqlConn。
  struct MySQLConfig {
      std::string host = "127.0.0.1";
      unsigned int port = 3306;
      std::string user = "root";
      std::string password;
      std::string database = "pool_learning";
      std::string charset = "utf8mb4";

      // libmysqlclient 的超时单位都是“秒”。
      // 0 表示不显式设置，交给客户端库的默认值。
      unsigned int connect_timeout_sec = 3;
      unsigned int read_timeout_sec = 5;
      unsigned int write_timeout_sec = 5;

      // 用于日志输出，不包含密码。
      std::string describe() const;
  };

  // 从环境变量读取配置，便于在 Linux 虚拟机和脚本中直接运行：
  //   MYSQL_HOST / MYSQL_PORT / MYSQL_USER / MYSQL_PASSWORD /
  //   MYSQL_DATABASE / MYSQL_CHARSET /
  //   MYSQL_CONNECT_TIMEOUT / MYSQL_READ_TIMEOUT / MYSQL_WRITE_TIMEOUT
  MySQLConfig configFromEnv();

}  // namespace mysqlpool
