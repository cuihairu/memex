# vcpkg toolchain 只负责把安装树的搜索路径注入编译器，不代管 find_package：
# 各模块在自己的 CMakeLists 里显式声明依赖，缺一个就报「target not found」。
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(aws-cpp-sdk-s3 CONFIG REQUIRED)

# 服务端核心库：会话、账号、存储、S3 抽象层（测试直链，不起进程）
add_library(memex_server_core STATIC
  src/server.cpp
  src/session.cpp
  src/store.cpp
  src/cred.cpp
  src/webhook.cpp
  src/storage.cpp)
target_include_directories(memex_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(memex_server_core PUBLIC cxx_std_20)
target_link_libraries(memex_server_core PUBLIC
  memex_common
  asio::asio
  OpenSSL::Crypto
  unofficial::sqlite3::sqlite3
  aws-cpp-sdk-s3)

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

  # T4.2 跨态互通验收：跨态会话日志闭环（时间/双方/时长，无内容）、
  # 归档起点＝首条归档消息前最近一次成功登录时刻（A8）
  add_executable(test_cross tests/test_cross.cpp)
  target_link_libraries(test_cross PRIVATE memex_server_core)
  add_test(NAME cross COMMAND test_cross)
  set_tests_properties(cross PROPERTIES TIMEOUT 60)

  # T4.5 表情与常用联系人验收（服务端面）：星标／最近落库（UPSERT、
  # MAX 单调、排序、账号隔离）＋TEXT 受理双方最近刷新＋FAV_CMD 置顶重推
  add_executable(test_fav tests/test_fav.cpp)
  target_link_libraries(test_fav PRIVATE memex_server_core)
  add_test(NAME fav COMMAND test_fav)
  set_tests_properties(fav PROPERTIES TIMEOUT 60)

  # T4.3 消息状态与多端验收（服务端面）：已读上报留痕（伪造拒绝／幂等）、
  # READ_NOTICE 在线路由、在线表查询与登录/登出/断开变更推送
  add_executable(test_read_presence tests/test_read_presence.cpp)
  target_link_libraries(test_read_presence PRIVATE memex_server_core)
  add_test(NAME read_presence COMMAND test_read_presence)
  set_tests_properties(read_presence PROPERTIES TIMEOUT 60)

  # T4.6 通讯录可见性验收（A20）：库级过滤／脱敏／上级引用抹除／落盘重开＋
  # CLI 配置面（org hide|fields|allow|visibility list）＋协议级 ORG_QUERY
  # 按查询者过滤（被隐藏者不可见不可搜——数据不出服务端）
  add_executable(test_visibility tests/test_visibility.cpp)
  target_link_libraries(test_visibility PRIVATE memex_server_core)
  target_compile_definitions(test_visibility PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME visibility COMMAND test_visibility)
  set_tests_properties(visibility PROPERTIES TIMEOUT 60)

  # T4.10 webhook 通知接入验收：HTTP 面鉴权／校验／状态码 ＋ 投递管线
  # （在线即投、离线补投＋ACK、群扇出、归档 type=48 前后对账）
  add_executable(test_webhook tests/test_webhook.cpp)
  target_link_libraries(test_webhook PRIVATE memex_server_core)
  add_test(NAME webhook COMMAND test_webhook)
  set_tests_properties(webhook PROPERTIES TIMEOUT 60)
endif()
