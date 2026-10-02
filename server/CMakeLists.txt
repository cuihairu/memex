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
  target_compile_definitions(test_accounts PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME accounts COMMAND test_accounts)
  set_tests_properties(accounts PROPERTIES TIMEOUT 60)

  # T2.2 消息通道验收：在线路由、离线排队补投、ACK 清队重投、同类型互踢跨类型并存
  add_executable(test_gateway tests/test_gateway.cpp)
  target_link_libraries(test_gateway PRIVATE memex_server_core)
  add_test(NAME gateway COMMAND test_gateway)
  set_tests_properties(gateway PROPERTIES TIMEOUT 60)

  # T2.3 消息归档验收：全量落库、撤回仅置标记不清正文、撤回事件独立留痕、越权拒绝
  add_executable(test_archive tests/test_archive.cpp)
  target_link_libraries(test_archive PRIVATE memex_server_core)
  target_compile_definitions(test_archive PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME archive COMMAND test_archive)

  # T2.6 组织架构验收：部门树、成员资料（直属上级独立字段）、
  # 上级链路逐级上溯、CSV 批量导入错误行校验拒绝（库级＋CLI 进程级）
  add_executable(test_org tests/test_org.cpp)
  target_link_libraries(test_org PRIVATE memex_server_core)
  target_compile_definitions(test_org PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME org COMMAND test_org)
  set_tests_properties(org PROPERTIES TIMEOUT 60)

  # T4.1 群聊验收：建群／拉人／公告／退群（群主退群＝解散）、成员扇出、
  # 全量归档一次、按成员检索联入、离线补投、非成员被拒不归档
  add_executable(test_group tests/test_group.cpp)
  target_link_libraries(test_group PRIVATE memex_server_core)
  add_test(NAME group COMMAND test_group)
  set_tests_properties(group PROPERTIES TIMEOUT 60)
endif()
