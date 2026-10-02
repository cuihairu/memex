cmake_minimum_required(VERSION 3.24)

project(memex
  VERSION 0.1.0
  DESCRIPTION "Memex 内网办公即时通讯系统"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MEMEX_BUILD_CLIENT "构建 Qt 桌面客户端（需要 Qt 6）" OFF)
option(MEMEX_BUILD_TESTS "构建测试" ON)

# asio 为 header-only 系统库（libasio-dev），以头文件存在性探测
include(CheckIncludeFileCXX)
check_include_file_cxx("asio.hpp" MEMEX_HAS_ASIO)
if(NOT MEMEX_HAS_ASIO)
  message(FATAL_ERROR "未找到 asio.hpp：请安装 libasio-dev（或等价 asio 开发包）")
endif()

if(MEMEX_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(common)
add_subdirectory(server)

if(MEMEX_BUILD_CLIENT)
  find_package(Qt6 6.6 REQUIRED COMPONENTS Widgets Sql)
  add_subdirectory(client)
endif()
