// 协议消息门面：载荷为 memex.proto 生成的 Envelope（强类型，单一事实源）。
// 帧 = 4 字节大端长度前缀 + Envelope 序列化字节（长度前缀语义见 frame.hpp）。
#pragma once

#include <string>
#include <string_view>

#include <memex/protocol/frame.hpp>
// 生成代码（构建树 common/proto/ 下平铺生成，目录已入 include 路径）
#include <memex.pb.h>

namespace memex::protocol {

// 生成代码命名空间（memex.protocol.v1）：pb.h 已在本作用域声明 namespace v1，
// 直接引用即可——再写同名命名空间别名（namespace v1 = …::v1）在 clang 下是
// redefinition 错误（gcc 宽容曾掩盖；linux/gcc 与 mac/clang 双工具链对齐）。

// 对外统一别名：信封即消息。
using Message = v1::Envelope;
using MsgType = v1::MsgType;

// 编码为完整帧（长度前缀 + 序列化 Envelope）。
std::string encode(const Message& msg);

// 从纯载荷（FrameDecoder 解出的帧体）解析；空载荷或解析失败抛 ProtocolError。
Message decode_payload(std::string_view payload);

// 从完整帧（长度前缀 + 载荷）解码。
Message decode_frame(std::string_view frame);

const char* msg_type_name(MsgType t);

} // namespace memex::protocol
