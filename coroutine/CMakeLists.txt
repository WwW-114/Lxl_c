cmake_minimum_required(VERSION 3.13)
project(coroutine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "coroutine 依赖 ucontext 与 epoll，仅支持 Linux")
endif()

add_library(co STATIC
            src/Fiber.cpp
            src/Scheduler.cpp
            src/Net.cpp)

target_include_directories(co PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(co PRIVATE -Wall -Wextra)

add_executable(test_coroutine test/test_coroutine.cpp)
target_link_libraries(test_coroutine PRIVATE co)
target_compile_options(test_coroutine PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME coroutine_test COMMAND test_coroutine)
