cmake_minimum_required(VERSION 3.10)

project(tcp_p2p
    VERSION 1.0.0
    DESCRIPTION "A simple TCP P2P implementation using the Reactor pattern"
    LANGUAGES C CXX
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

# ---- 基础版可执行文件 ----
add_executable(${PROJECT_NAME}
    main.c
    reactor.c
    peer.c
)

# 头文件搜索路径（方便 #include "reactor.h" 之类的写法）
target_include_directories(${PROJECT_NAME}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- KV 服务器版本（条件编译）----
option(TCP_P2P_KVSTORE "Build the KV-store server variant" OFF)

if(TCP_P2P_KVSTORE)
    set(KVSTORE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../project/kvstore")

    add_subdirectory(
        "${KVSTORE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/kvstore"
    )

    add_executable(tcp_p2p_kv
        main.c
        reactor.c
        peer.c
    )

    target_include_directories(tcp_p2p_kv
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_definitions(tcp_p2p_kv PRIVATE
        TCP_P2P_KVSTORE=1
    )

    target_link_libraries(tcp_p2p_kv PRIVATE
        kvstore_core
    )

    # 新增：kvstore_core 含 C++ 编译单元，链接时使用 C++ 链接器
    set_target_properties(tcp_p2p_kv PROPERTIES LINKER_LANGUAGE CXX)
endif()

# ---- 安装规则（可选）----
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

if(TCP_P2P_KVSTORE)
    install(TARGETS tcp_p2p_kv
        RUNTIME DESTINATION bin
    )
endif()
