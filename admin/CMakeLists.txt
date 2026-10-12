# admin 管理后台（2026-10-11 用户令）：Drogon 独立进程（默认 8089）。
# 依赖：drogon（vcpkg `admin` feature）+ memex_server_core（直连库面——
# ServerStore/authz/cred 与 CLI 同径复用，判权引擎同一套）。
# 本目录随 MEMEX_BUILD_SERVER 一起构建（直连库面依赖服务端库）。
find_package(Drogon CONFIG REQUIRED) # vcpkg 装的是 DrogonConfig.cmake（大小写敏感）

# 业务核心库：setup()（配置＋路由注册）单列，供 memex_admin 主程序与
# API 验收测试（进程内 setup + 另线程 run）共用同一份真实路由面。
add_library(memex_admin_core STATIC src/admin_app.cpp)
target_compile_features(memex_admin_core PRIVATE cxx_std_17)
target_include_directories(memex_admin_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(memex_admin_core PUBLIC
  Drogon::Drogon
  nlohmann_json::nlohmann_json
  memex_server_core)

add_executable(memex_admin src/main.cpp)
target_compile_features(memex_admin PRIVATE cxx_std_17)
target_link_libraries(memex_admin PRIVATE memex_admin_core)

if(MEMEX_BUILD_TESTS)
  # admin 后台 API 验收：真 admin 进程（setup+run 进程内）× 真库 ＋
  # 登录门/角色门/账号组织/策略/留存/审计红线（auditor 证）/设备台账/
  # 健康面逐腿打穿；org-admin 越权 403 与 CLI 同口径。
  add_executable(test_admin_api tests/test_admin_api.cpp)
  target_include_directories(test_admin_api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_admin_api PRIVATE memex_admin_core)
  # CI 假 abort 排障：terminate 钩子的 backtrace 要能对回符号（dladdr
  # 只解析动态符号表），测试二进制导出自身符号（Linux/glibc）
  if(UNIX AND NOT APPLE)
    target_link_options(test_admin_api PRIVATE -rdynamic)
  endif()
  target_compile_definitions(test_admin_api PRIVATE
    MEMEX_ADMIN_WWW_DIR="${CMAKE_CURRENT_SOURCE_DIR}/www")
  add_test(NAME admin_api COMMAND test_admin_api)
  set_tests_properties(admin_api PROPERTIES TIMEOUT 60)
endif()
