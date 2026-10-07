#include <memex/protocol/messages.hpp>

namespace memex::protocol {

std::string encode(const Message& msg) { return encode_frame(encode_payload(msg)); }

std::string encode_payload(const Message& msg) {
  std::string payload;
  msg.SerializeToString(&payload);
  return payload;
}

Message decode_payload(std::string_view payload) {
  if (payload.empty()) {
    throw ProtocolError("空载荷");
  }
  Message msg;
  if (!msg.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    throw ProtocolError("载荷不是合法的 Envelope 编码");
  }
  return msg;
}

Message decode_frame(std::string_view frame) {
  if (frame.size() <= kLengthPrefixSize) {
    throw ProtocolError("帧不完整");
  }
  return decode_payload(frame.substr(kLengthPrefixSize));
}

const char* msg_type_name(MsgType t) {
  switch (t) {
  case v1::HELLO: return "hello";
  case v1::PING: return "ping";
  case v1::PONG: return "pong";
  case v1::TEXT: return "text";
  case v1::ACK: return "ack";
  case v1::RECALL: return "recall";
  case v1::FILE_META: return "file_meta";
  case v1::FILE_RESUME: return "file_resume";
  case v1::FILE_DONE: return "file_done";
  case v1::LOGIN: return "login";
  case v1::LOGIN_RESULT: return "login_result";
  case v1::KICK: return "kick";
  case v1::LOGOUT: return "logout";
  case v1::ORG_QUERY: return "org_query";
  case v1::ORG_DATA: return "org_data";
  case v1::GROUP_CMD: return "group_cmd";
  case v1::GROUP_RESULT: return "group_result";
  case v1::GROUP_QUERY: return "group_query";
  case v1::GROUP_DATA: return "group_data";
  case v1::CROSS_LOG: return "cross_log";
  case v1::READ: return "read";
  case v1::READ_NOTICE: return "read_notice";
  case v1::PRESENCE_QUERY: return "presence_query";
  case v1::PRESENCE_DATA: return "presence_data";
  case v1::FAV_QUERY: return "fav_query";
  case v1::FAV_DATA: return "fav_data";
  case v1::FAV_CMD: return "fav_cmd";
  case v1::NOTICE: return "notice";
  case v1::FILE_AUTHZ: return "file_authz";
  case v1::FILE_AUTHZ_RESULT: return "file_authz_result";
  case v1::MSG_TYPE_UNSPECIFIED: break;
  }
  return "unknown";
}

} // namespace memex::protocol
