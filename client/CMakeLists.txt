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
  engine/collab/collab_engine.cpp)
target_include_directories(memex_engine_collab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memex_engine_collab PUBLIC Qt6::Core)

qt_add_executable(memex_client
  app/main.cpp
  app/main_window.cpp
  app/main_window.hpp)
target_include_directories(memex_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_compile_definitions(memex_client PRIVATE MEMEX_VERSION="${PROJECT_VERSION}")
target_link_libraries(memex_client PRIVATE
  memex_engine_direct
  memex_engine_collab
  Qt6::Widgets
  Qt6::Sql)

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
endif()
