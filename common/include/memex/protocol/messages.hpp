// 协议消息：类型枚举与 JSON 映射。
// 载荷统一为 UTF-8 JSON 对象；本头文件定义公共字段与编解码。
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include <memex/protocol/frame.hpp>

namespace memex::protocol {

// 消息类型。v0.1 骨架集合，后续按需追加，不改动已分配编号。
enum class MsgType : std::uint16_t {
  kHello = 1,     // 握手：终端标识与能力
  kPing = 2,      // 心跳请求
  kPong = 3,      // 心跳应答
  kText = 10,     // 文本消息
  kAck = 11,      // 消息回执
  kRecall = 12,   // 撤回（仅显示层；服务端保留原文并记录撤回事件）
  kFileMeta = 20,   // 文件元数据（文件字节流旁路，不经服务端）
  kFileResume = 21, // 文件续传偏移协商（接收方 → 发送方）
  kFileDone = 22,   // 文件终态：落盘完成与否 + 整文件 SHA-256 校验结果
};

// 编解码错误类型 ProtocolError 定义于 frame.hpp。

struct Message {
  MsgType type{MsgType::kText};
  std::uint64_t seq{0};   // 发送序号（会话内单调）
  std::string from;       // 发送方标识
  std::string to;         // 接收方标识（直连态为设备标识，协作态为账号）
  std::int64_t ts_ms{0};  // 发送时间戳（毫秒）
  nlohmann::json body;    // 载荷体（各类型自定义）

  nlohmann::json to_json() const;
  std::string encode() const; // 编码为完整帧（长度前缀 + JSON）

  // 从 JSON 对象解析；字段缺失或类型不符抛 ProtocolError。
  static Message from_json(const nlohmann::json& j);
  // 从纯 JSON 载荷解析（FrameDecoder 解出的帧体）。
  static Message decode_payload(std::string_view payload);
  // 从完整帧（长度前缀 + JSON）解码。
  static Message decode_frame(std::string_view frame);
};

const char* msg_type_name(MsgType t);

} // namespace memex::protocol
