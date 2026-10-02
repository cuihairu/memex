# protobuf 代码生成：proto/memex.proto → 构建树内 memex.pb.{h,cc}。
# memex.proto 是协议唯一事实源，改动它即改协议（已分配编号只增不改）。
find_package(protobuf CONFIG REQUIRED)

set(MEMEX_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/proto/memex.proto)
set(MEMEX_PROTO_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
set(MEMEX_PB_HDR ${MEMEX_PROTO_OUT_DIR}/memex.pb.h)
set(MEMEX_PB_SRC ${MEMEX_PROTO_OUT_DIR}/memex.pb.cc)

add_custom_command(
  OUTPUT ${MEMEX_PB_HDR} ${MEMEX_PB_SRC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MEMEX_PROTO_OUT_DIR}
  COMMAND protobuf::protoc "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto"
          "--cpp_out=${MEMEX_PROTO_OUT_DIR}" ${MEMEX_PROTO}
  DEPENDS ${MEMEX_PROTO}
  VERBATIM
  COMMENT "protoc memex.proto")

add_library(memex_common STATIC
  src/frame.cpp
  src/messages.cpp
  ${MEMEX_PB_SRC})

target_include_directories(memex_common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MEMEX_PROTO_OUT_DIR})

target_link_libraries(memex_common PUBLIC
  nlohmann_json::nlohmann_json
  protobuf::libprotobuf)

target_compile_features(memex_common PUBLIC cxx_std_20)

if(MEMEX_BUILD_TESTS)
  add_executable(test_protocol tests/test_protocol.cpp)
  target_link_libraries(test_protocol PRIVATE memex_common)
  add_test(NAME protocol COMMAND test_protocol)
endif()
