# vcpkg toolchain 只负责把安装树的搜索路径注入编译器，不代管 find_package：
# 各模块在自己的 CMakeLists 里显式声明依赖，缺一个就报「target not found」。
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(aws-cpp-sdk-s3 CONFIG REQUIRED)
# storage.cpp 直接消费 aws-cpp-sdk-core 符号（SecureMemClear 等）：部分 vcpkg
# 安装树里 s3 包不传递 core，按「显式声明依赖」规矩单独 find+链接。
find_package(aws-cpp-sdk-core CONFIG REQUIRED)

# 服务端核心库：会话、账号、存储、S3 抽象层（测试直链，不起进程）
add_library(memex_server_core STATIC
  src/server.cpp
  src/session.cpp
  src/store.cpp
  src/authz.cpp
  src/cred.cpp
  src/files_server.cpp
  src/webhook.cpp
  src/model_gateway.cpp
  src/group_assistant.cpp
  src/storage.cpp)
target_include_directories(memex_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(memex_server_core PUBLIC cxx_std_17)
target_link_libraries(memex_server_core PUBLIC
  memex_common
  asio::asio
  OpenSSL::Crypto
  unofficial::sqlite3::sqlite3
  aws-cpp-sdk-s3
  aws-cpp-sdk-core)

add_executable(memex_server src/main.cpp)
target_compile_features(memex_server PRIVATE cxx_std_17)
target_link_libraries(memex_server PRIVATE memex_server_core)
target_compile_definitions(memex_server PRIVATE MEMEX_VERSION="${PROJECT_VERSION}")

# R26-1 memex agent：轻量常驻（Linux 起步）——注册令牌心跳上报本机负载；
# 只读采样不开监听口不落盘（memex 只做「看+连」，操作类归 croupier）
if(UNIX)
  add_executable(memex_agent src/agent_main.cpp)
  target_compile_features(memex_agent PRIVATE cxx_std_17)
  target_link_libraries(memex_agent PRIVATE nlohmann_json::nlohmann_json
                                            asio::asio)
endif()

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

  # 振屏（需求批⑥，服务端面）：NUDGE 空体受理——归档留痕＋在线即投＋
  # 受理回执；不进离线补投；连接级 1s 限频静默拒；群目标拒；未登录拒
  add_executable(test_nudge tests/test_nudge.cpp)
  target_link_libraries(test_nudge PRIVATE memex_server_core)
  add_test(NAME nudge COMMAND test_nudge)
  set_tests_properties(nudge PROPERTIES TIMEOUT 60)

  # BUG-007 msg_id 撞 id 消歧：V1 重演两行齐＋换盐重排、补传幂等、
  # Ack.msg_id 三态回带、群消息撞键同口径、库错受理行为不变
  add_executable(test_msg_dedupe tests/test_msg_dedupe.cpp)
  target_link_libraries(test_msg_dedupe PRIVATE memex_server_core)
  add_test(NAME msg_dedupe COMMAND test_msg_dedupe)
  set_tests_properties(msg_dedupe PROPERTIES TIMEOUT 60)

  # 表情包素材（需求批②，服务端面）：个人素材 BLOB 落库——上传 magic/
  # 1MiB 校验、清单属主隔离、下载字节往返＋Content-Type 嗅探、删除属主裁决
  add_executable(test_emoji tests/test_emoji.cpp)
  target_link_libraries(test_emoji PRIVATE memex_server_core)
  add_test(NAME emoji COMMAND test_emoji)
  set_tests_properties(emoji PROPERTIES TIMEOUT 60)

  # 用户头像（需求批⑫，服务端面）：member_avatars 四档 BLOB——上传 magic/
  # 1MiB/档位校验、他人 403、下载字节往返（组织内互见）、删除归零回落默认
  add_executable(test_avatar tests/test_avatar.cpp)
  target_link_libraries(test_avatar PRIVATE memex_server_core)
  add_test(NAME avatar COMMAND test_avatar)
  set_tests_properties(avatar PROPERTIES TIMEOUT 60)

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
  add_executable(test_bot tests/test_bot.cpp)
  target_link_libraries(test_bot PRIVATE memex_server_core)
  add_test(NAME bot COMMAND test_bot)
  add_executable(test_model_gateway tests/test_model_gateway.cpp)
  target_link_libraries(test_model_gateway PRIVATE memex_server_core)
  add_test(NAME model_gateway COMMAND test_model_gateway)
  add_executable(test_group_assistant tests/test_group_assistant.cpp)
  target_link_libraries(test_group_assistant PRIVATE memex_server_core)
  add_test(NAME group_assistant COMMAND test_group_assistant)
  set_tests_properties(webhook PROPERTIES TIMEOUT 60)

  # R23-1 存储抽象层验收：文件元数据表、秒传哈希去重、群/人两级配额、
  # 外网收件箱留痕、RustFS compose 生成契约（纯库级，不涉网/不涉 docker）
  add_executable(test_files_meta tests/test_files_meta.cpp)
  target_link_libraries(test_files_meta PRIVATE memex_server_core)
  add_test(NAME files_meta COMMAND test_files_meta)
  set_tests_properties(files_meta PROPERTIES TIMEOUT 60)

  # R23-2 判权骨架验收：冲突规则写死（Deny>Allow>Inherited>Default）、
  # 同档确定性、理由留痕
  add_executable(test_authz tests/test_authz.cpp)
  target_link_libraries(test_authz PRIVATE memex_server_core)
  add_test(NAME authz COMMAND test_authz)
  set_tests_properties(authz PROPERTIES TIMEOUT 60)

  # R23-2 文件面走查：真 TCP HTTP 全链路 × 群主/管理员/成员/非成员四视角
  #（会话/上传/秒传/下载/列表/置顶/删除/配额/回滚；对象层内存 Fake）
  add_executable(test_files_api tests/test_files_api.cpp)
  target_link_libraries(test_files_api PRIVATE memex_server_core)
  add_test(NAME files_api COMMAND test_files_api)
  set_tests_properties(files_api PROPERTIES TIMEOUT 60)

  # R26-1 agent 验收：真 agent 进程 × in-process FileServer（真 TCP）——
  # 登记签发令牌→agent --once 一拍心跳 exit 0→列表绿灯＋指标落账＋令牌
  # 不出现；错令牌/连不上一拍即败 exit 1；缺参 exit 2
  if(UNIX)
    add_executable(test_agent tests/test_agent.cpp)
    target_link_libraries(test_agent PRIVATE memex_server_core)
    target_compile_definitions(test_agent PRIVATE
      MEMEX_AGENT_BIN="$<TARGET_FILE:memex_agent>")
    add_test(NAME agent COMMAND test_agent)
    set_tests_properties(agent PROPERTIES TIMEOUT 60)
  endif()

  # R23-4 外网单向 uplink 走查：双实例真 TCP（内网面×uplink 面）——scope
  # 两道闸（内网令牌打 uplink 面 403／uplink 令牌打内网面 403）、唯一落点=
  # 收件箱＋审计流水＋配额、mine 只列本人、delete 只许自己的上传、
  # uplink 口无任何下载/读取内网数据路由（404 兜底）
  add_executable(test_uplink tests/test_uplink.cpp)
  target_link_libraries(test_uplink PRIVATE memex_server_core)
  add_test(NAME uplink COMMAND test_uplink)
  set_tests_properties(uplink PROPERTIES TIMEOUT 60)

  # R23-1 真容器集成（默认 SKIP；MEMEX_S3_E2E=1 且宿主有 docker 才全量跑：
  # RustFS compose up→S3Storage 全接口往返→down）
  add_executable(test_s3_e2e tests/test_s3_e2e.cpp)
  target_link_libraries(test_s3_e2e PRIVATE memex_server_core)
  add_test(NAME s3_e2e COMMAND test_s3_e2e)
  set_tests_properties(s3_e2e PROPERTIES TIMEOUT 300)
endif()
