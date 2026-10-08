# 双引擎静态库：各自完整，只共享数据层（经 client/core）
qt_standard_project_setup()

# R24-3 密码箱客户端面：PBKDF2/AES-256-GCM 本地解锁派生（与服务端同 libcrypto）
find_package(OpenSSL CONFIG REQUIRED)

# 共享内核：本地库、会话、联系人（T1.2 起：LocalStore）
add_library(memex_core STATIC
  core/local_store.cpp)
target_include_directories(memex_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memex_core PUBLIC Qt6::Core Qt6::Sql)

add_library(memex_engine_direct STATIC
  engine/direct/direct_engine.cpp
  engine/direct/discovery.cpp
  engine/direct/direct_transport.cpp
  engine/direct/file_transfer.cpp
  engine/direct/secure_channel.cpp)
target_include_directories(memex_engine_direct PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# 平台-8 直连安全：Ed25519 身份 + X25519 握手 + HKDF/AES-GCM（libcrypto）
target_link_libraries(memex_engine_direct PUBLIC memex_core memex_common
  Qt6::Core Qt6::Network OpenSSL::Crypto)

add_library(memex_engine_collab STATIC
  engine/collab/collab_engine.cpp
  engine/collab/files_client.cpp)
target_include_directories(memex_engine_collab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memex_engine_collab PUBLIC memex_core memex_common Qt6::Core Qt6::Network)
target_compile_definitions(memex_engine_collab PRIVATE MEMEX_VERSION="${PROJECT_VERSION}")

# qrc 资源自动处理（A23 图标面：窗口/托盘/关于页共用的 logo 多尺寸资产）
set(CMAKE_AUTORCC ON)

qt_add_executable(memex_client
  app/main.cpp
  app/main_window.cpp
  app/main_window.hpp
  app/icons/icons.qrc
  app/notify_center.cpp
  app/notify_center.hpp
  app/notify_prefs.cpp
  app/notify_prefs.hpp
  app/screenshot_tool.cpp
  app/screenshot_tool.hpp
  app/brand_kit.cpp
  app/brand_kit.hpp
  app/brand_settings_page.cpp
  app/shortcut_settings.cpp
  app/theme.cpp
  app/theme.hpp
  app/theme_settings_page.cpp
  app/theme_settings_page.hpp
  app/file_assistant.cpp
  app/file_assistant.hpp
  app/task_dialog.cpp
  app/approval_dialog.cpp
  app/report_dialog.cpp
  app/group_tools_dialog.cpp
  app/audit_dialog.cpp
  app/office_map_dialog.cpp
  app/assist_dialog.cpp
  engine/task/task_provider.cpp
  engine/task/task_http.cpp
  engine/task/github_provider.cpp
  engine/task/dingtalk_provider.cpp
  engine/task/feishu_provider.cpp
  app/task_dialog.hpp
  app/approval_dialog.hpp
  app/office_map_dialog.hpp
  app/assist_dialog.hpp
  app/group_memo_dialog.cpp
  app/group_memo_dialog.hpp
  app/group_ci_dialog.cpp
  app/group_ci_dialog.hpp
  app/group_pack_dialog.cpp
  app/group_pack_dialog.hpp
  app/group_server_dialog.cpp
  app/group_server_dialog.hpp
  app/group_vault_crypto.cpp
  app/group_vault_crypto.hpp
  app/group_vault_dialog.cpp
  app/group_vault_dialog.hpp)
target_include_directories(memex_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_compile_definitions(memex_client PRIVATE MEMEX_VERSION="${PROJECT_VERSION}")
target_link_libraries(memex_client PRIVATE
  memex_engine_direct
  memex_engine_collab
  Qt6::Widgets
  Qt6::Sql
  OpenSSL::Crypto)

# 分发体属性：Windows GUI 子系统（双击启动不弹控制台；--version 等控制台面经
# 句柄重定向仍可用）；macOS .app bundle（dmg 分发体，Info.plist 由 CMake 生成）。
# A23 图标面：icns 进 .app bundle Resources（Info.plist CFBundleIconFile 指名）
if(APPLE)
  target_sources(memex_client PRIVATE app/icons/memex.icns)
  set_source_files_properties(app/icons/memex.icns PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
endif()

set_target_properties(memex_client PROPERTIES
  WIN32_EXECUTABLE TRUE
  MACOSX_BUNDLE TRUE)
if(APPLE)
  # .app 与内含可执行名统一 Memex（dmg 面向用户展示；Linux/Windows 产物名不变）
  set_target_properties(memex_client PROPERTIES
    OUTPUT_NAME "Memex"
    MACOSX_BUNDLE_BUNDLE_NAME "Memex"
    MACOSX_BUNDLE_ICON_FILE memex.icns
    MACOSX_BUNDLE_GUI_IDENTIFIER "site.cuihairu.memex"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()

# A23 图标面：Windows 可执行内嵌 ico（Explorer/任务栏取此，与 Qt 资源同源）
if(WIN32)
  target_sources(memex_client PRIVATE app/memex.rc)
endif()

if(MEMEX_BUILD_TESTS)
  # 测试环境隔离：offscreen 无头渲染 + XDG_CACHE_HOME 指向构建树。
  # fontconfig 缓存必须隔离——用户机既有缓存与随链 Qt 携带的 fontconfig
  # 版本可能错位，会在文字排版路径（FcCharSetHasChar）崩掉，与被测代码无关。
  set(MEMEX_TEST_ENV "QT_QPA_PLATFORM=offscreen;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/.qt-cache")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.qt-cache")

  add_test(NAME client_smoke COMMAND memex_client --smoke)
  set_tests_properties(client_smoke PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 30)

  # T1.1 UDP 发现验收：同机双实例互现 + 超时离线
  add_executable(test_discovery engine/direct/tests/test_discovery.cpp)
  target_link_libraries(test_discovery PRIVATE memex_engine_direct)
  add_test(NAME discovery COMMAND test_discovery)
  set_tests_properties(discovery PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 40)

  # T1.2 直连文本验收：双实例互发、送达确认、本地落库、重开历史仍在
  add_executable(test_direct_chat engine/direct/tests/test_direct_chat.cpp)
  target_link_libraries(test_direct_chat PRIVATE memex_engine_direct)
  add_test(NAME direct_chat COMMAND test_direct_chat)
  set_tests_properties(direct_chat PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # T1.3 直连文件验收：≥100MB 文件与多层目录、进度、中断续传、哈希一致
  add_executable(test_direct_file engine/direct/tests/test_direct_file.cpp)
  target_link_libraries(test_direct_file PRIVATE memex_engine_direct)
  add_test(NAME direct_file COMMAND test_direct_file)
  set_tests_properties(direct_file PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 240)

  # 平台-8 直连安全验收：信道对拨（密文不可见/篡改拒/重放拒）、真引擎
  # raw-socket 协议腿（握手+密文+ACK+定针）、明文直插拒、TOFU 定针
  # （错针拒+清针恢复）、重启同库同公钥
  add_executable(test_direct_secure engine/direct/tests/test_direct_secure.cpp)
  target_link_libraries(test_direct_secure PRIVATE memex_engine_direct)
  add_test(NAME direct_secure COMMAND test_direct_secure)
  set_tests_properties(direct_secure PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # T2.1 协作登录验收：真实服务端进程 + 双引擎互踢 + 登录记录查询
  add_executable(test_collab_login engine/collab/tests/test_collab_login.cpp)
  target_link_libraries(test_collab_login PRIVATE memex_engine_collab)
  target_compile_definitions(test_collab_login PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME collab_login COMMAND test_collab_login)
  set_tests_properties(collab_login PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # T2.2 协作消息通道验收：互发回执、离线补投、msg_id 去重落库、断线自动重连
  add_executable(test_collab_chat engine/collab/tests/test_collab_chat.cpp)
  target_link_libraries(test_collab_chat PRIVATE memex_engine_collab)
  target_compile_definitions(test_collab_chat PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME collab_chat COMMAND test_collab_chat)
  set_tests_properties(collab_chat PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # T2.5 断线补传归档验收：中断期暂存、恢复补传、归档无重复无缺失（CLI 检索核对）
  add_executable(test_collab_resend engine/collab/tests/test_collab_resend.cpp)
  target_link_libraries(test_collab_resend PRIVATE memex_engine_collab)
  target_compile_definitions(test_collab_resend PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME collab_resend COMMAND test_collab_resend)
  set_tests_properties(collab_resend PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 平台-10 文件旁路授权：统一 AuthorizationService 四问（同部门/跨部门/
  # 策略放行/再转发禁/收方未知/未登录 fail-closed）
  add_executable(test_file_authz engine/collab/tests/test_file_authz.cpp)
  target_link_libraries(test_file_authz PRIVATE memex_engine_collab)
  target_compile_definitions(test_file_authz PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME file_authz COMMAND test_file_authz)
  set_tests_properties(file_authz PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # T4.3 客户端半边验收：已读上报→发送方 message_read、在线表推送刷新、
  # 同账号第二台桌面登录互踢（桌面单点在线，T2.1 合并验收）
  add_executable(test_client_read_presence engine/collab/tests/test_read_presence.cpp)
  target_link_libraries(test_client_read_presence PRIVATE memex_engine_collab)
  target_compile_definitions(test_client_read_presence PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME client_read_presence COMMAND test_client_read_presence)
  set_tests_properties(client_read_presence PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # R23-3 文件助手客户端面验收：真实服务端进程（--files-port，无 S3＝
  # 元数据面全链路）——备忘录建/改/查/删、inbox 列表混排条目、跨账号
  # 403、错口令 401、未登录拒发、存储未配置上传 503 边界
  add_executable(test_files_client engine/collab/tests/test_files_client.cpp)
  target_link_libraries(test_files_client PRIVATE memex_engine_collab)
  target_compile_definitions(test_files_client PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME files_client COMMAND test_files_client)
  set_tests_properties(files_client PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # R23-3 文件助手窗口冒烟：真服务端进程 × 离屏 QDialog——连接→存备忘录
  # →流列表落地→编辑→删除；上传错误通道（无 S3=503 明示）
  add_executable(test_files_assistant
    app/tests/test_files_assistant.cpp
    app/file_assistant.cpp)
  target_link_libraries(test_files_assistant PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_files_assistant PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_files_assistant PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME files_assistant COMMAND test_files_assistant)
  set_tests_properties(files_assistant PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # R24-2 群备忘录窗口冒烟：真服务端进程 × 离屏 QDialog——连接→列表
  # →新建→编辑→搜索→历史窗填充；权限面（成员 403/删恒归管理员）与
  # 回滚全链走 test_files_client 引擎级腿，不在此重复
  add_executable(test_group_memo_dialog
    app/tests/test_group_memo_dialog.cpp
    app/group_memo_dialog.cpp)
  target_link_libraries(test_group_memo_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_group_memo_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_group_memo_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME group_memo_dialog COMMAND test_group_memo_dialog)
  set_tests_properties(group_memo_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # R24-3 群密码箱窗口冒烟：真服务端进程 × 离屏 QDialog——本地派生建箱
  # →解锁→条目加密存取→查看解密→复制留痕→改箱密码/重置箱重包裹；权限面
  # （成员 403/名单收窄/审计管理面）走 test_files_client 引擎级腿，不在此重复
  add_executable(test_group_vault_dialog
    app/tests/test_group_vault_dialog.cpp
    app/group_vault_dialog.cpp
    app/group_vault_crypto.cpp)
  target_link_libraries(test_group_vault_dialog PRIVATE
    memex_engine_collab Qt6::Widgets OpenSSL::Crypto)
  target_include_directories(test_group_vault_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_group_vault_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME group_vault_dialog COMMAND test_group_vault_dialog)
  set_tests_properties(group_vault_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 120)

  # R25-2 群 CI/CD 窗口冒烟：真服务端进程 × 离屏 QDialog——管理员建流水线
  # →开工具白名单→触发（stub 即时终态）→红绿灯翻转→run 历史谁触发可回溯
  # →结果卡片回群（服务端面留痕走 test_files_api，不在此重复）
  add_executable(test_group_ci_dialog
    app/tests/test_group_ci_dialog.cpp
    app/group_ci_dialog.cpp)
  target_link_libraries(test_group_ci_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_group_ci_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_group_ci_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME group_ci_dialog COMMAND test_group_ci_dialog)
  set_tests_properties(group_ci_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # R25-3 打包/导出窗口冒烟：真服务端进程 × 离屏 QDialog——开白名单→
  # 一键出产物（台账落地谁出可回溯）→同款重打覆盖→导出群配置快照
  # （密文面不进快照）；权限矩阵（非成员 403/删恒归管理员/卡片回群）
  # 走 test_files_api 服务端腿，不在此重复
  add_executable(test_group_pack_dialog
    app/tests/test_group_pack_dialog.cpp
    app/group_pack_dialog.cpp)
  target_link_libraries(test_group_pack_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_group_pack_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_group_pack_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME group_pack_dialog COMMAND test_group_pack_dialog)
  set_tests_properties(group_pack_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # R26-2 群服务器面板冒烟：真服务端进程 × 离屏 QDialog——登记（令牌一次
  # 性显示）→agent --once 心跳→列表绿/红两态；成员只读、越权登记 403
  # 状态行（服务端判权矩阵走 test_files_api，不在此重复）
  add_executable(test_group_server_dialog
    app/tests/test_group_server_dialog.cpp
    app/group_server_dialog.cpp)
  target_link_libraries(test_group_server_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_group_server_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_group_server_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>"
    MEMEX_AGENT_BIN="$<TARGET_FILE:memex_agent>")
  add_test(NAME group_server_dialog COMMAND test_group_server_dialog)
  set_tests_properties(group_server_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # R27-1 任务清单窗口冒烟：真服务端进程 × 离屏 QDialog——连接→自建→
  # 同群分配（权限模型「谁能分配」现查）→对端清单见来源→勾完成→撤回→
  # 到期提醒（通知中心触发＋服务端回执只提醒一次）。判权矩阵走
  # test_files_api 协议腿，不在此重复
  add_executable(test_task_dialog
    app/tests/test_task_dialog.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp)
  target_link_libraries(test_task_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_task_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_task_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME task_dialog COMMAND test_task_dialog)
  set_tests_properties(task_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·审批窗口冒烟：真服务端进程 × 离屏 QDialog——连接→member1 发起
  # 请假→直属上级 owner1 待决可见并批准→member1 见终态→新申请撤回。
  # 判权矩阵（自审 403/无权 403/已决 409/白名单外 400）走 test_files_api
  # 协议腿，不在此重复
  add_executable(test_approval_dialog
    app/tests/test_approval_dialog.cpp
    app/approval_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp)
  target_link_libraries(test_approval_dialog PRIVATE
    memex_engine_direct
    memex_engine_collab
    Qt6::Widgets
    Qt6::Sql
    OpenSSL::Crypto)
  target_include_directories(test_approval_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_approval_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME approval_dialog COMMAND test_approval_dialog)
  set_tests_properties(approval_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·日报周报窗口冒烟：真服务端进程 × 离屏 QDialog——连接→member1
  # 写日报→当日重复提交=更新（同日期一行）→直属上级 owner1 团队聚合可见
  #→「仅本周」过滤→无关者 admin1 看下属=空（判权服务端裁，无下属即空
  # 列表）。判权矩阵（org-admin 不兜底/转岗断权/幽灵 403）走 test_files_api
  # 协议腿，不在此重复
  add_executable(test_report_dialog
    app/tests/test_report_dialog.cpp
    app/report_dialog.cpp)
  target_link_libraries(test_report_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_report_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_report_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME report_dialog COMMAND test_report_dialog)
  set_tests_properties(report_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·群工具三件窗口冒烟：真服务端进程 × 离屏 QDialog——连接→建投票
  # （选项越界 400 状态行明示）→投票改票覆盖 counts 变化→无关者关票 403→
  # 关后投票 409→接龙加入（upsert 自己条目）→关后加入 409→建任务幽灵
  # 负责人 404→认领重复 409→完成留痕 done_by。判权矩阵细分走 test_files_api
  # 协议腿，不在此重复
  add_executable(test_group_tools_dialog
    app/tests/test_group_tools_dialog.cpp
    app/group_tools_dialog.cpp)
  target_link_libraries(test_group_tools_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_group_tools_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_group_tools_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME group_tools_dialog COMMAND test_group_tools_dialog)
  set_tests_properties(group_tools_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·品牌物料客户端面冒烟：真服务端进程 × 离屏——默认标兜底（没配就
  # 不变：Memex 标题/默认图标/logo 空）→CLI 配牌 fetch 换牌（标题/公司名/
  # accent/slogan/version）→配 logo（QPixmap 产真 PNG 走 CLI）位图落地+
  # 缓存文件→新实例 load_cache 离线复原→坏端口 fetch 静默不改现状→CLI
  # 清牌 fetch 回默认标。判权/PNG 校验协议腿走 test_files_api，不在此重复
  add_executable(test_brand_kit
    app/tests/test_brand_kit.cpp
    app/brand_kit.cpp)
  target_link_libraries(test_brand_kit PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_brand_kit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_brand_kit PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME brand_kit COMMAND test_brand_kit)
  set_tests_properties(brand_kit PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·品牌物料设置页冒烟：真服务端进程 × 离屏——未连接本地拒→错口令
  # 状态行→member 保存 403 归属文案→org-admin 保存换牌（本机 BrandKit
  # 即时生效）→accent 坏形态本地拒→logo 选图预览＋上传/非 PNG 本地拒→
  # 清除回落→截图验收（配置前后主窗 grab 落 docs/src/public/branding/，
  # 另一客户端＝独立 MainWindow 实例）。判权/PNG 校验协议腿走 test_files_api
  add_executable(test_brand_settings
    app/tests/test_brand_settings.cpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/brand_kit.cpp
    app/main_window.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_brand_settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_brand_settings PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  target_compile_definitions(test_brand_settings PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>"
    MEMEX_DOCS_BRAND_DIR="${CMAKE_SOURCE_DIR}/docs/src/public/branding")
  add_test(NAME brand_settings COMMAND test_brand_settings)
  set_tests_properties(brand_settings PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·会话审计窗口冒烟：真服务端进程 × 离屏 QDialog——CollabEngine
  # 造归档消息→无角色检索 403 状态行明示→授 auditor 后持证检索命中→
  # 查阅日志台账见检索与被拒留痕（可对账）。判权分立腿（admin≠auditor）
  # 走 test_files_api 协议段，不在此重复
  add_executable(test_audit_dialog
    app/tests/test_audit_dialog.cpp
    app/audit_dialog.cpp)
  target_link_libraries(test_audit_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_audit_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_audit_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME audit_dialog COMMAND test_audit_dialog)
  set_tests_properties(audit_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·办公室位置图窗口冒烟：真服务端进程 × 离屏 QDialog——member1
  # 无权编辑 403 状态行明示→admin1（org-admin）建座＋拖拽落位保存回查→
  # 同层互见（工位即楼层归属）→搜索过滤→绑定/解绑。判权协议腿走
  # test_files_api 位置图段，不在此重复
  add_executable(test_office_map_dialog
    app/tests/test_office_map_dialog.cpp
    app/office_map_dialog.cpp)
  target_link_libraries(test_office_map_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_office_map_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_office_map_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME office_map_dialog COMMAND test_office_map_dialog)
  set_tests_properties(office_map_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # 二期·远程协助窗口冒烟：真服务端进程 × 离屏 QDialog 双实例（发起方
  # ×受控方）——CLI 开部门放行→发起→受控方批准缩权→启动→合成帧推送
  # 回环渲染→点画面发输入→受控方收到→结束撤权→台账终态＋审计链。判权
  # 协议腿（默认禁/consent/权限位门）走 test_files_api 协议段
  add_executable(test_assist_dialog
    app/tests/test_assist_dialog.cpp
    app/assist_dialog.cpp)
  target_link_libraries(test_assist_dialog PRIVATE
    memex_engine_collab Qt6::Widgets)
  target_include_directories(test_assist_dialog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(test_assist_dialog PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME assist_dialog COMMAND test_assist_dialog)
  set_tests_properties(assist_dialog PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # T2.4 模式切换与降级验收：不重启切换、历史合并展示（来源字段）、
  # 服务端不可达回落直连态并提示「消息不进归档」、停服后直连仍可用
  add_executable(test_mode_switch
    tests/test_mode_switch.cpp
    app/main_window.cpp
    app/brand_kit.cpp
    app/brand_kit.hpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_mode_switch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_mode_switch PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  target_compile_definitions(test_mode_switch PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME mode_switch COMMAND test_mode_switch)
  set_tests_properties(mode_switch PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # BUG-004 托盘交互验收：单击/双击激活主界面（隐藏弹出、最小化还原保留最大化），
  # 右键留给菜单；裸 QSystemTrayIcon 发真实 activated 信号驱动生产接线
  # wire_tray_activation（offscreen 可跑，无 DE 依赖）。
  add_executable(test_tray
    tests/test_tray.cpp
    app/main_window.cpp
    app/brand_kit.cpp
    app/brand_kit.hpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_tray PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_tray PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  add_test(NAME tray COMMAND test_tray)
  set_tests_properties(tray PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # T4.4 截图与标注验收：标注画布（涂鸦/箭头/马赛克/文字/撤销/保存）、
  # 多屏区域数学（跨屏/负坐标/高 DPI）、端到端（注入截屏→标注→确认→
  # 主窗发送→对端引擎收到文件）
  add_executable(test_screenshot
    tests/test_screenshot.cpp
    app/main_window.cpp
    app/brand_kit.cpp
    app/brand_kit.hpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_screenshot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_screenshot PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  # 平台-10 授权门接线后，截图 e2e 须起真服务端过四问（MEMEX_SERVER_BIN）
  target_compile_definitions(test_screenshot PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME screenshot COMMAND test_screenshot)
  set_tests_properties(screenshot PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)


  # T4.10 通知子系统验收：分级裁决矩阵／偏好落盘／通知中心执行面
  # （重要托盘强提醒、紧急置顶确认弹窗抓图、全屏递延补弹）／设置页
  # 交互；全链路＝真实服务端 --webhook-port → CLI 建 webhook → HTTP
  # POST 三级 → 引擎 NOTICE →信号＋弹窗，本地库 compose 同源对账
  add_executable(test_notify
    tests/test_notify.cpp
    app/notify_prefs.cpp
    app/notify_center.cpp)
  target_include_directories(test_notify PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_notify PRIVATE
    memex_engine_collab Qt6::Widgets Qt6::Sql)
  target_compile_definitions(test_notify PRIVATE
    MEMEX_SERVER_BIN="$<TARGET_FILE:memex_server>")
  add_test(NAME notify COMMAND test_notify)
  set_tests_properties(notify PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 240) # CI 共享机全链路（真实服务端+webhook+弹窗）超 90s，与 direct_file 同档

  # A23 图标面验收：qrc logo 资产（多尺寸可载＋品牌橙恒定）、主窗窗口
  # 图标非空、帮助→关于页真弹（logo 标签抓图入库）、开机启动 .desktop Icon
  add_executable(test_icon
    tests/test_icon.cpp
    app/main_window.cpp
    app/brand_kit.cpp
    app/brand_kit.hpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_icon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_icon PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  add_test(NAME icon COMMAND test_icon)
  set_tests_properties(icon PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # T4.9 主题切换验收（R19）：令牌完备性与品牌橙恒定、跟随系统解析、
  # 手动选择落盘往返、令牌化 QSS、多主题扩展位、应用级即时生效、设置页交互、
  # 主窗接线（控件样式走令牌、切换后富文本重渲、菜单入口可达）
  # 链接面与 test_screenshot 一致（含 main_window.cpp/引擎）——否则接线无覆盖
  add_executable(test_theme
    tests/test_theme.cpp
    app/main_window.cpp
    app/brand_kit.cpp
    app/brand_kit.hpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_theme PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_theme PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  add_test(NAME theme COMMAND test_theme)
  set_tests_properties(theme PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # BUG-001/002/003 聊天窗三入口点击验收：截图/发文件无会话守卫入状态栏、
  # 表情全链（面板弹出→内置表情落输入框→面板自关）；链接面同 test_theme
  add_executable(test_chat_actions
    tests/test_chat_actions.cpp
    app/main_window.cpp
    app/brand_kit.cpp
    app/brand_kit.hpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_chat_actions PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_chat_actions PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  add_test(NAME chat_actions COMMAND test_chat_actions)
  set_tests_properties(chat_actions PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # 新消息闪烁提醒验收：真直连对端发消息→主窗 alert_count（非激活才闪/
  # 合并窗 2s 内不叠加/开关关了不闪/开关落盘往返）；链接面同 test_chat_actions
  add_executable(test_flash_alert
    tests/test_flash_alert.cpp
    app/main_window.cpp
    app/brand_kit.cpp
    app/brand_kit.hpp
    app/brand_settings_page.cpp
    app/shortcut_settings.cpp
    app/file_assistant.cpp
    app/task_dialog.cpp
    app/approval_dialog.cpp
    app/report_dialog.cpp
    app/group_tools_dialog.cpp
    app/audit_dialog.cpp
    app/office_map_dialog.cpp
    app/assist_dialog.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp
    engine/task/dingtalk_provider.cpp
    engine/task/feishu_provider.cpp
    app/group_memo_dialog.cpp
    app/group_vault_crypto.cpp
    app/group_vault_dialog.cpp
    app/group_ci_dialog.cpp
    app/group_pack_dialog.cpp
    app/group_server_dialog.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_flash_alert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_flash_alert PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql
    OpenSSL::Crypto)
  add_test(NAME flash_alert COMMAND test_flash_alert)
  set_tests_properties(flash_alert PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # R27-2 外部任务 Provider SPI 验收：能力声明位（L1/L2/L3）、URL 模板
  # 槽替换（{project}/{key}，缺槽拒）、直通链接 http(s) 门、注册表
  # 预设解析与未知 id、L2/L3 默认不支持（异步 SPI 基类同步回不可用）
  add_executable(test_task_provider
    engine/task/tests/test_task_provider.cpp
    engine/task/task_provider.cpp)
  target_include_directories(test_task_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_task_provider PRIVATE Qt6::Core)
  add_test(NAME task_provider COMMAND test_task_provider)
  set_tests_properties(task_provider PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 30)

  # R27-3 GitHub Issues provider 验收（假传输回放不打外网）：L1 键校验、
  # L2 拉列表解析滤 PR、L3 建单/关单、认证头与 API 形态断言、错误腿
  add_executable(test_github_provider
    engine/task/tests/test_github_provider.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/github_provider.cpp)
  target_include_directories(test_github_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_github_provider PRIVATE Qt6::Core Qt6::Network)
  add_test(NAME github_provider COMMAND test_github_provider)
  set_tests_properties(github_provider PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 30)

  # R27-3 钉钉待办 provider 验收（假传输回放不打外网）：令牌流缓存与
  # 到期重取、L2 拉列表（detailUrl 可缺行仍在）、L3 建单/完成、鉴权头
  add_executable(test_dingtalk_provider
    engine/task/tests/test_dingtalk_provider.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/dingtalk_provider.cpp)
  target_include_directories(test_dingtalk_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_dingtalk_provider PRIVATE Qt6::Core Qt6::Network)
  add_test(NAME dingtalk_provider COMMAND test_dingtalk_provider)
  set_tests_properties(dingtalk_provider PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 30)

  # R27-3 飞书任务 provider 验收（假传输回放不打外网）：tenant 令牌流
  #（缓存/信封 code 拒/到期重取）、L3 建任务与完成回写、Bearer 头与端点
  add_executable(test_feishu_provider
    engine/task/tests/test_feishu_provider.cpp
    engine/task/task_provider.cpp
    engine/task/task_http.cpp
    engine/task/feishu_provider.cpp)
  target_include_directories(test_feishu_provider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_feishu_provider PRIVATE Qt6::Core Qt6::Network)
  add_test(NAME feishu_provider COMMAND test_feishu_provider)
  set_tests_properties(feishu_provider PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 30)
endif()
