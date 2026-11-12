cmake_minimum_required(VERSION 3.16)

project(mempool_demo LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MEMPOOL_ASAN "Build with AddressSanitizer/UBSan (GCC/Clang) or ASan (MSVC)" OFF)
option(MEMPOOL_DEBUG_CHECKS "Enable O(chunks) pointer validation in mempool_free" OFF)

set(MEMPOOL_COMMON_FLAGS "")

if (MSVC)
  set(MEMPOOL_COMMON_FLAGS /W4 /utf-8)
  if (MEMPOOL_ASAN)
    message(STATUS "MEMPOOL_ASAN: 请在 Visual Studio 项目属性中开启 AddressSanitizer")
  endif()
else()
  set(MEMPOOL_COMMON_FLAGS -Wall -Wextra -Wpedantic -g -O0)
  if (MEMPOOL_ASAN)
    list(APPEND MEMPOOL_COMMON_FLAGS
         -fsanitize=address,undefined
         -fno-omit-frame-pointer
         -O1)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# 独立模块 1：侵入式空闲链表（不依赖 mempool）
add_library(free_list STATIC free_list.c)
target_include_directories(free_list PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(free_list PRIVATE ${MEMPOOL_COMMON_FLAGS})

# 独立模块 2：内存池，只使用 free_list 的公开 API
add_library(mempool STATIC mempool.c)
target_include_directories(mempool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mempool PUBLIC free_list)
target_compile_options(mempool PRIVATE ${MEMPOOL_COMMON_FLAGS})

if (MEMPOOL_DEBUG_CHECKS)
  target_compile_definitions(mempool PRIVATE MEMPOOL_DEBUG)
endif()

# 演示程序；mempool-v1.c 保留作对照，不加入构建
add_executable(mempool_demo main.c)
target_link_libraries(mempool_demo PRIVATE mempool)
target_compile_options(mempool_demo PRIVATE ${MEMPOOL_COMMON_FLAGS})

enable_testing()
add_test(NAME mempool_demo COMMAND mempool_demo)
