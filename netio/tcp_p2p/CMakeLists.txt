cmake_minimum_required(VERSION 3.10)

project(tcp_p2p
    VERSION 1.0.0
    DESCRIPTION "A simple TCP P2P implementation using the Reactor pattern"
    LANGUAGES C
)

# ---- C 标准与编译选项 ----
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)          # 使用 -std=c11 而非 -std=gnu11

# ---- 默认构建类型 ----
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---- 编译警告与优化 ----
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

# 不同构建类型的编译选项
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g3 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")

# ---- 可执行文件 ----
add_executable(${PROJECT_NAME}
    main.c
    reactor.c
    peer.c
)

# 头文件搜索路径（方便 #include "reactor.h" 之类的写法）
target_include_directories(${PROJECT_NAME}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- 安装规则（可选）----
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)



     
