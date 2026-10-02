# vcpkg toolchain 只负责把安装树的搜索路径注入编译器，不代管 find_package：
# 各模块在自己的 CMakeLists 里显式声明依赖，缺一个就报「target not found」。
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# 服务端核心库：会话、账号、存储（测试直链，不起进程）
add_library(memex_server_core STATIC
  src/server.cpp
  src/session.cpp
  src/store.cpp
  src/cred.cpp)
target_include_directories(memex_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(memex_server_core PUBLIC cxx_std_20)
target_link_libraries(memex_server_core PUBLIC
  memex_common
  asio::asio
  OpenSSL::Crypto
  unofficial::sqlite3::sqlite3)

add_executable(memex_server src/main.cpp)
target_compile_features(memex_server PRIVATE cxx_std_20)
target_link_libraries(memex_server PRIVATE memex_server_core)
target_compile_definitions(memex_server PRIVATE MEMEX_VERSION="${PROJECT_VERSION}")

if(MEMEX_BUILD_TESTS)
  add_test(NAME server_self_test COMMAND memex_server --self-test)

  # T2.1 账号与设备验收：登录校验、单点在线互踢、登录记录全量可查
  add_executable(test_accounts tests/test_accounts.cpp)
  target_link_libraries(test_accounts PRIVATE memex_server_core)
  add_test(NAME accounts COMMAND test_accounts)
  set_tests_properties(accounts PROPERTIES TIMEOUT 60)

  # T2.2 消息通道验收：在线路由、离线排队补投、ACK 清队重投、同类型互踢跨类型并存
  add_executable(test_gateway tests/test_gateway.cpp)
  target_link_libraries(test_gateway PRIVATE memex_server_core)
  add_test(NAME gateway COMMAND test_gateway)
  set_tests_properties(gateway PROPERTIES TIMEOUT 60)
endif()
