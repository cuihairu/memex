# 双引擎静态库：各自完整，只共享数据层（经 client/core）
qt_standard_project_setup()

# 共享内核：本地库、会话、联系人（T1.2 起：LocalStore）
add_library(memex_core STATIC
  core/local_store.cpp)
target_include_directories(memex_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memex_core PUBLIC Qt6::Core Qt6::Sql)

add_library(memex_engine_direct STATIC
  engine/direct/direct_engine.cpp
  engine/direct/discovery.cpp
  engine/direct/direct_transport.cpp
  engine/direct/file_transfer.cpp)
target_include_directories(memex_engine_direct PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memex_engine_direct PUBLIC memex_core memex_common Qt6::Core Qt6::Network)

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
  app/theme.cpp
  app/theme.hpp
  app/theme_settings_page.cpp
  app/theme_settings_page.hpp
  app/file_assistant.cpp
  app/file_assistant.hpp)
target_include_directories(memex_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_compile_definitions(memex_client PRIVATE MEMEX_VERSION="${PROJECT_VERSION}")
target_link_libraries(memex_client PRIVATE
  memex_engine_direct
  memex_engine_collab
  Qt6::Widgets
  Qt6::Sql)

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

  # T2.4 模式切换与降级验收：不重启切换、历史合并展示（来源字段）、
  # 服务端不可达回落直连态并提示「消息不进归档」、停服后直连仍可用
  add_executable(test_mode_switch
    tests/test_mode_switch.cpp
    app/main_window.cpp
    app/file_assistant.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_mode_switch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_mode_switch PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql)
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
    app/file_assistant.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_tray PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_tray PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql)
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
    app/file_assistant.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_screenshot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_screenshot PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql)
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
    app/file_assistant.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_icon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_icon PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql)
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
    app/file_assistant.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_theme PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_theme PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql)
  add_test(NAME theme COMMAND test_theme)
  set_tests_properties(theme PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 90)

  # BUG-001/002/003 聊天窗三入口点击验收：截图/发文件无会话守卫入状态栏、
  # 表情全链（面板弹出→内置表情落输入框→面板自关）；链接面同 test_theme
  add_executable(test_chat_actions
    tests/test_chat_actions.cpp
    app/main_window.cpp
    app/file_assistant.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_chat_actions PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_chat_actions PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql)
  add_test(NAME chat_actions COMMAND test_chat_actions)
  set_tests_properties(chat_actions PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)

  # 新消息闪烁提醒验收：真直连对端发消息→主窗 alert_count（非激活才闪/
  # 合并窗 2s 内不叠加/开关关了不闪/开关落盘往返）；链接面同 test_chat_actions
  add_executable(test_flash_alert
    tests/test_flash_alert.cpp
    app/main_window.cpp
    app/file_assistant.cpp
    app/notify_center.cpp
    app/notify_prefs.cpp
    app/screenshot_tool.cpp
    app/icons/icons.qrc
    app/theme.cpp
    app/theme_settings_page.cpp)
  target_include_directories(test_flash_alert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/app)
  target_link_libraries(test_flash_alert PRIVATE
    memex_engine_direct memex_engine_collab Qt6::Widgets Qt6::Sql)
  add_test(NAME flash_alert COMMAND test_flash_alert)
  set_tests_properties(flash_alert PROPERTIES
    ENVIRONMENT "${MEMEX_TEST_ENV}"
    TIMEOUT 60)
endif()
