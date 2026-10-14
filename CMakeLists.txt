cmake_minimum_required(VERSION 3.21)
project(function_game CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# 源码注释为中文（UTF-8），MSVC 需要显式指定；clang/gcc 默认即 UTF-8
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_subdirectory(src)
