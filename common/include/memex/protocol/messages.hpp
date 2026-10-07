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

// —— 通知子系统（T4.10）——
// 通知消息的服务端发送方标识：归档／常用联系人／界面显示均以此为键，
// 客户端分级推送的入口判断也以此为准（普通通知不走常规新消息提示）。
inline constexpr const char* kNoticeSender = "\xe9\x80\x9a\xe7\x9f\xa5"; // "通知"（UTF-8）

// 通知正文的归档形态（服务端归档与客户端本地缓存同源生成，前后对账一致）：
// 「标题：正文[ 跳转]」——气泡渲染按单行处理，跳转随文留痕。
inline std::string compose_notice_text(std::string_view title,
                                       std::string_view content,
                                       std::string_view jump_url) {
  std::string s(title);
  s += "\xef\xbc\x9a"; // "："
  s += content;
  if (!jump_url.empty()) {
    s += ' ';
    s += jump_url;
  }
  return s;
}

// 编码为完整帧（长度前缀 + 序列化 Envelope）。
std::string encode(const Message& msg);

// 序列化为纯载荷（不含长度前缀）：直连安全通道的封套输入面——
// 密文帧自行成帧（长度前缀 + 计数 + 密文），载荷语义与 decode_payload 对偶。
std::string encode_payload(const Message& msg);

// 从纯载荷（FrameDecoder 解出的帧体）解析；空载荷或解析失败抛 ProtocolError。
Message decode_payload(std::string_view payload);

// 从完整帧（长度前缀 + 载荷）解码。
Message decode_frame(std::string_view frame);

const char* msg_type_name(MsgType t);

} // namespace memex::protocol
