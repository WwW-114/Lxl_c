cmake_minimum_required(VERSION 3.16)

project(mysql_connection_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(WARNING "本项目面向 Linux 验证；其他平台需要自行提供 libmysqlclient 与 pthread")
endif()

option(MYSQLPOOL_ASAN "使用 AddressSanitizer/UBSan 构建（GCC/Clang）" OFF)

find_package(Threads REQUIRED)

# ------------------------------------------------------------------
# 定位 libmysqlclient：
#   优先使用 mysql_config / mariadb_config；
#   找不到时退回 find_path + find_library。
# ------------------------------------------------------------------
find_program(MYSQL_CONFIG_EXECUTABLE NAMES mysql_config mariadb_config)

add_library(mysqlclient_interface INTERFACE)

if(MYSQL_CONFIG_EXECUTABLE)
  execute_process(
    COMMAND ${MYSQL_CONFIG_EXECUTABLE} --include
    OUTPUT_VARIABLE MYSQL_INCLUDE_FLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  execute_process(
    COMMAND ${MYSQL_CONFIG_EXECUTABLE} --libs
    OUTPUT_VARIABLE MYSQL_LINK_FLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  separate_arguments(MYSQL_INCLUDE_FLAGS UNIX_COMMAND "${MYSQL_INCLUDE_FLAGS}")
  separate_arguments(MYSQL_LINK_FLAGS UNIX_COMMAND "${MYSQL_LINK_FLAGS}")

  target_compile_options(mysqlclient_interface INTERFACE ${MYSQL_INCLUDE_FLAGS})
  target_link_libraries(mysqlclient_interface INTERFACE ${MYSQL_LINK_FLAGS})

  message(STATUS "使用 ${MYSQL_CONFIG_EXECUTABLE} 定位 libmysqlclient")
else()
  find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql mariadb)
  find_library(MYSQL_LIBRARY NAMES mysqlclient mariadb)

  if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(FATAL_ERROR
      "找不到 libmysqlclient。\n"
      "Ubuntu/Debian: sudo apt install libmysqlclient-dev  或  libmariadb-dev\n"
      "CentOS/RHEL:   sudo dnf install mysql-devel          或  mariadb-devel\n"
      "Arch:          sudo pacman -S mysql-clients          或  mariadb-libs")
  endif()

  target_include_directories(mysqlclient_interface INTERFACE ${MYSQL_INCLUDE_DIR})
  target_link_libraries(mysqlclient_interface INTERFACE ${MYSQL_LIBRARY})

  message(STATUS "使用 find_path/find_library 定位 libmysqlclient")
endif()

set(MYSQLPOOL_WARNING_FLAGS -Wall -Wextra -Wpedantic)

if(MYSQLPOOL_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ------------------------------------------------------------------
# 连接池静态库
# ------------------------------------------------------------------
add_library(mysqlpool STATIC
  src/mysql_config.cpp
  src/logger.cpp
  src/query_result.cpp
  src/mysql_conn.cpp
  src/sync_connection_pool.cpp
  src/async_connection_pool.cpp)

target_include_directories(mysqlpool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mysqlpool PUBLIC mysqlclient_interface Threads::Threads)
target_compile_options(mysqlpool PRIVATE ${MYSQLPOOL_WARNING_FLAGS})
