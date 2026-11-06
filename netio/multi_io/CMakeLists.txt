cmake_minimum_required(VERSION 3.16)
project(ccp_internet_study_netio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    # /utf-8 必须加：源码是 UTF-8，MSVC 默认按本地代码页（GBK）解析会乱码
    add_compile_options(/utf-8 /W4 /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -O2)
endif()

# poll / epoll 是 Linux 特有的，非 Linux 平台不参与编译
set(NETIO_SOURCES
    src/EventPoller.cpp
    src/SelectPoller.cpp
    src/TcpServer.cpp
    # HTTP 层：架在 TcpServer 之上，全部是跨平台代码
    src/http/HttpUtil.cpp
    src/http/HttpRequest.cpp
    src/http/HttpResponse.cpp
    src/http/HttpContext.cpp
    src/http/HttpServer.cpp
)
if (UNIX)
    list(APPEND NETIO_SOURCES
        src/PollPoller.cpp
        src/EpollPoller.cpp
    )
endif()

add_library(netio STATIC ${NETIO_SOURCES})
target_include_directories(netio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if (WIN32)
    target_link_libraries(netio PUBLIC ws2_32)
endif()

add_executable(echo_server examples/echo_server.cpp)
target_link_libraries(echo_server PRIVATE netio)

add_executable(echo_client examples/echo_client.cpp)
target_link_libraries(echo_client PRIVATE netio)

add_executable(http_server examples/http_server.cpp)
target_link_libraries(http_server PRIVATE netio)

add_executable(http_client examples/http_client.cpp)
target_link_libraries(http_client PRIVATE netio)
