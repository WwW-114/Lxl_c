# 由 tcp_p2p/CMakeLists.txt 通过 add_subdirectory 引入

add_library(kvstore_core STATIC
    kvstore.c
    kvstore_table.c
)

target_include_directories(kvstore_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(kvstore_core PUBLIC c_std_11)

option(KVSTORE_BUILD_DEMO "Build kvstore standalone demo" ON)

if(KVSTORE_BUILD_DEMO)
    add_executable(kvstore_demo
        kvstore_demo.c
    )

    target_link_libraries(kvstore_demo PRIVATE kvstore_core)
endif()
