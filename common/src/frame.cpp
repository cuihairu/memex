#include <memex/protocol/frame.hpp>

#include <cstring>

namespace memex::protocol {

namespace {

void append_u32_be(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>(v & 0xFF));
}

std::uint32_t read_u32_be(const char* p) {
  std::uint32_t v = 0;
  std::memcpy(&v, p, 4);
  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

} // namespace

std::string encode_frame(std::string_view payload) {
  if (payload.empty()) {
    throw ProtocolError("空载荷：帧长度必须大于 0");
  }
  if (payload.size() > kMaxFrameSize) {
    throw ProtocolError("载荷超限：超过 kMaxFrameSize");
  }
  std::string frame;
  frame.reserve(kLengthPrefixSize + payload.size());
  append_u32_be(frame, static_cast<std::uint32_t>(payload.size()));
  frame.append(payload);
  return frame;
}

DecodeStatus FrameDecoder::feed(std::string_view bytes, std::vector<std::string>& out) {
  buffer_.append(bytes.data(), bytes.size());

  std::size_t offset = 0;
  bool got_frame = false;
  while (true) {
    if (buffer_.size() - offset < kLengthPrefixSize) {
      break; // 长度前缀不完整
    }
    const std::uint32_t len = read_u32_be(buffer_.data() + offset);
    if (len == 0) {
      return DecodeStatus::kZeroLength;
    }
    if (len > kMaxFrameSize) {
      return DecodeStatus::kTooLarge;
    }
    if (buffer_.size() - offset - kLengthPrefixSize < len) {
      break; // 载荷不完整
    }
    out.emplace_back(buffer_, offset + kLengthPrefixSize, len);
    offset += kLengthPrefixSize + len;
    got_frame = true;
  }

  buffer_.erase(0, offset);
  return got_frame ? DecodeStatus::kOk : DecodeStatus::kNeedMoreData;
}

void FrameDecoder::reset() { buffer_.clear(); }

const char* decode_status_name(DecodeStatus s) {
  switch (s) {
  case DecodeStatus::kOk: return "ok";
  case DecodeStatus::kNeedMoreData: return "need_more_data";
  case DecodeStatus::kZeroLength: return "zero_length";
  case DecodeStatus::kTooLarge: return "too_large";
  }
  return "unknown";
}

} // namespace memex::protocol
