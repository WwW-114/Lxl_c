# 由 tcp_p2p/CMakeLists.txt 通过 add_subdirectory 引入

# ---- 存储后端选择（条件编译）----
# array  : 可变长数组（沿用原 kvstore_table.c 的 kv_entry_t / kv_table_t）
# rbtree : 红黑树（MyRedBlackTree.hpp）

set (KVSTORE_TABLE_BACKEND "array" CACHE STRING
    "kvstore table backend: array = dynamic array, rbtree = red-black tree, hash = unordered_map")
set_property (CACHE KVSTORE_TABLE_BACKEND PROPERTY STRINGS array rbtree)

if (KVSTORE_TABLE_BACKEND STREQUAL "array")
    set (KVSTORE_TABLE_DEF KVSTORE_TABLE_USE_ARRAY=1)
elseif (KVSTORE_TABLE_BACKEND STREQUAL "rbtree")
    set (KVSTORE_TABLE_DEF KVSTORE_TABLE_USE_RBTREE=1)
elseif (KVSTORE_TABLE_BACKEND STREQUAL "hash")
    set (KVSTORE_TABLE_DEF KVSTORE_TABLE_USE_HASH=1)
else()
    message(FATAL_ERROR
        "KVSTORE_TABLE_BACKEND must be 'array', 'rbtree' or 'hash', got: '${KVSTORE_TABLE_BACKEND}'")
endif()

message(STATUS "kvstore table backend: ${KVSTORE_TABLE_BACKEND}")

# kvstore_table.cpp 无论选择哪个后端都由 C++ 编译器编译
enable_language(CXX)

add_library(kvstore_core STATIC
            kvstore.c
            kvstore_table.cpp
)

target_include_directories (kvstore_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features (kvstore_core PUBLIC c_std_11)
target_compile_features (kvstore_core PRIVATE cxx_std_17)
target_compile_definitions (kvstore_core PRIVATE ${KVSTORE_TABLE_DEF})

option(KVSTORE_BUILD_DEMO "Build kvstore standalone demo" ON)

if(KVSTORE_BUILD_DEMO)
    add_executable(kvstore_demo
        kvstore_demo.c
    )

    target_link_libraries(kvstore_demo PRIVATE kvstore_core)

    # demo 是 C 源文件，但 kvstore_core 含 C++ 编译单元，使用 C++ 链接器
    set_target_properties(kvstore_demo PROPERTIES LINKER_LANGUAGE CXX)
endif()
