cmake_minimum_required(VERSION 3.13)

project(kcp_chat LANGUAGES C CXX)

if(NOT UNIX)
	message(FATAL_ERROR, "本项目依赖 POSIX socket，只能在 Linux / Unix 上构建。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSION OFF)

set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
				Debug Release RelWithDebInfo MinSizeRel)
endif()

add_library(kcp STATIC ikcp.c)

target_include_directories(kcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(server server.cpp common.h)
target_link_libraries(server PRIVATE kcp)
target_compile_options(server PRIVATE -Wall -Wextra)

add_executable(client client.cpp common.h)
target_link_libraries(client PRIVATE kcp)
target_compile_options(client PRIVATE -Wall -Wextra)

