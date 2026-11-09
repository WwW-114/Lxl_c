cmake_minimum_required(VERSION 3.16)

project(threadpool_demo LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(THREADPOOL_TSAN "Build with ThreadSanitizer" OFF)

set(THREADPOOL_COMMON_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -g
    -O0
)

set(THREADPOOL_TSAN_COMPILE_FLAGS "")
set(THREADPOOL_TSAN_LINK_FLAGS "")

if(THREADPOOL_TSAN)
    set(THREADPOOL_TSAN_COMPILE_FLAGS
        -fsanitize=thread
        -fno-omit-frame-pointer
        -O1
    )
    set(THREADPOOL_TSAN_LINK_FLAGS -fsanitize=thread)
endif()

# 独立模块 1：有界阻塞队列
add_library(task_queue STATIC task_queue.c)
target_include_directories(task_queue PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(task_queue PUBLIC Threads::Threads)
target_compile_options(task_queue PRIVATE
    ${THREADPOOL_COMMON_FLAGS}
    ${THREADPOOL_TSAN_COMPILE_FLAGS}
)

# 独立模块 2：线程池，只使用 task_queue 的公开 API
add_library(threadpool STATIC threadpool.c)
target_include_directories(threadpool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(threadpool PUBLIC task_queue)
target_compile_options(threadpool PRIVATE
    ${THREADPOOL_COMMON_FLAGS}
    ${THREADPOOL_TSAN_COMPILE_FLAGS}
)

# 演示程序
add_executable(threadpool_demo main.c)
target_link_libraries(threadpool_demo PRIVATE threadpool)
target_compile_options(threadpool_demo PRIVATE
    ${THREADPOOL_COMMON_FLAGS}
    ${THREADPOOL_TSAN_COMPILE_FLAGS}
)

if(THREADPOOL_TSAN)
    target_link_options(threadpool_demo PRIVATE ${THREADPOOL_TSAN_LINK_FLAGS})
endif()

enable_testing()
add_test(NAME threadpool_demo COMMAND threadpool_demo)
