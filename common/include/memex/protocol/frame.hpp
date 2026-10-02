// Memex 协议帧：4 字节大端长度前缀 + JSON 载荷（UTF-8）。
// 长度前缀只描述载荷长度，不含自身 4 字节。
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace memex::protocol {

// 协议层错误。畸形输入必须报错返回，不得崩溃。
class ProtocolError : public std::runtime_error {
public:
  explicit ProtocolError(const std::string& what) : std::runtime_error(what) {}
};

// 单帧载荷上限：归档消息与文件元数据远小于此；超大载荷应走文件旁路。
inline constexpr std::uint32_t kMaxFrameSize = 4u * 1024u * 1024u;
inline constexpr std::size_t kLengthPrefixSize = 4;

// 编码一帧。载荷为空或超限时抛 ProtocolError。
std::string encode_frame(std::string_view payload);

enum class DecodeStatus {
  kOk,           // 解出至少一帧
  kNeedMoreData, // 数据不足，等待后续 feed
  kZeroLength,   // 长度为 0 的非法帧
  kTooLarge,     // 长度超限的非法帧
};

// 流式解码器：容忍字节流任意切分（TCP 粘包／半包）。
class FrameDecoder {
public:
  // 追加字节。返回解码状态；解出的载荷追加到 out。
  DecodeStatus feed(std::string_view bytes, std::vector<std::string>& out);

  void reset();

private:
  std::string buffer_;
};

const char* decode_status_name(DecodeStatus s);

} // namespace memex::protocol
