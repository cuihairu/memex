# 双引擎静态库：各自完整，只共享数据层（后续经 client/core）
qt_standard_project_setup()

add_library(memex_engine_direct STATIC
  engine/direct/direct_engine.cpp)
target_include_directories(memex_engine_direct PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memex_engine_direct PUBLIC Qt6::Core)

add_library(memex_engine_collab STATIC
  engine/collab/collab_engine.cpp)
target_include_directories(memex_engine_collab PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(memex_engine_collab PUBLIC Qt6::Core)

qt_add_executable(memex_client
  app/main.cpp
  app/main_window.cpp
  app/main_window.hpp)
target_include_directories(memex_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(memex_client PRIVATE
  memex_engine_direct
  memex_engine_collab
  Qt6::Widgets
  Qt6::Sql)

if(MEMEX_BUILD_TESTS)
  add_test(NAME client_smoke COMMAND memex_client --smoke)
  set_tests_properties(client_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 30)
endif()
