#include <memex/protocol/messages.hpp>

#include <stdexcept>

namespace memex::protocol {

namespace {
constexpr const char* kFieldType = "type";
constexpr const char* kFieldSeq = "seq";
constexpr const char* kFieldFrom = "from";
constexpr const char* kFieldTo = "to";
constexpr const char* kFieldTs = "ts_ms";
constexpr const char* kFieldBody = "body";
} // namespace

nlohmann::json Message::to_json() const {
  nlohmann::json j;
  j[kFieldType] = static_cast<std::uint16_t>(type);
  j[kFieldSeq] = seq;
  j[kFieldFrom] = from;
  j[kFieldTo] = to;
  j[kFieldTs] = ts_ms;
  j[kFieldBody] = body;
  return j;
}

Message Message::from_json(const nlohmann::json& j) {
  if (!j.is_object()) {
    throw ProtocolError("载荷不是 JSON 对象");
  }
  Message m;
  try {
    m.type = MsgType{j.at(kFieldType).get<std::uint16_t>()};
    m.seq = j.at(kFieldSeq).get<std::uint64_t>();
    m.from = j.at(kFieldFrom).get<std::string>();
    m.to = j.at(kFieldTo).get<std::string>();
    m.ts_ms = j.at(kFieldTs).get<std::int64_t>();
    m.body = j.at(kFieldBody);
  } catch (const nlohmann::json::exception& e) {
    throw ProtocolError(std::string("消息字段缺失或类型不符：") + e.what());
  }
  return m;
}

std::string Message::encode() const {
  return encode_frame(to_json().dump());
}

Message Message::decode_payload(std::string_view payload) {
  nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
  if (j.is_discarded()) {
    throw ProtocolError("载荷不是合法 JSON");
  }
  return from_json(j);
}

Message Message::decode_frame(std::string_view frame) {
  if (frame.size() <= kLengthPrefixSize) {
    throw ProtocolError("帧不完整");
  }
  return decode_payload(frame.substr(kLengthPrefixSize));
}

const char* msg_type_name(MsgType t) {
  switch (t) {
  case MsgType::kHello: return "hello";
  case MsgType::kPing: return "ping";
  case MsgType::kPong: return "pong";
  case MsgType::kText: return "text";
  case MsgType::kAck: return "ack";
  case MsgType::kRecall: return "recall";
  case MsgType::kFileMeta: return "file_meta";
  case MsgType::kFileResume: return "file_resume";
  case MsgType::kFileDone: return "file_done";
  }
  return "unknown";
}

} // namespace memex::protocol
