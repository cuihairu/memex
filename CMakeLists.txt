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

# 第三方依赖经 vcpkg manifest（vcpkg.json + builtin-baseline）统一管理，
# 构建入口见 CMakePresets.json（cmake --preset dev）：preset 注入 vcpkg
# toolchain，toolchain 负责安装依赖并把安装树挂进搜索路径。
# 注意 toolchain 不代管 find_package——各模块在自己的 CMakeLists 里显式声明
# （server 用 asio/OpenSSL/sqlite3，common 用 protobuf/nlohmann-json，client 用 Qt6）。
find_package(nlohmann_json CONFIG REQUIRED)

if(MEMEX_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(common)
add_subdirectory(server)

if(MEMEX_BUILD_CLIENT)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Sql Network)
  add_subdirectory(client)
endif()
