add_executable(memex_server src/main.cpp)

target_compile_features(memex_server PRIVATE cxx_std_20)
target_link_libraries(memex_server PRIVATE memex_common)

if(MEMEX_BUILD_TESTS)
  add_test(NAME server_self_test COMMAND memex_server --self-test)
endif()
