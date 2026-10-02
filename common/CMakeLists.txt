add_library(memex_common STATIC
  src/frame.cpp
  src/messages.cpp)

target_include_directories(memex_common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(memex_common PUBLIC nlohmann_json::nlohmann_json)

target_compile_features(memex_common PUBLIC cxx_std_20)

if(MEMEX_BUILD_TESTS)
  add_executable(test_protocol tests/test_protocol.cpp)
  target_link_libraries(test_protocol PRIVATE memex_common)
  add_test(NAME protocol COMMAND test_protocol)
endif()
