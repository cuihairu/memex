// 协议编解码单测（proto 载荷）：往返、粘包／半包、畸形输入不崩溃、
// 未知字段容忍（前向兼容：旧端收到新端加字段的消息不拒收）。
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <memex/protocol/messages.hpp>

using namespace memex::protocol;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond    \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

#define CHECK_THROWS(expr)                                                   \
  do {                                                                       \
    bool thrown = false;                                                     \
    try {                                                                    \
      expr;                                                                  \
    } catch (const std::exception&) {                                        \
      thrown = true;                                                         \
    }                                                                        \
    CHECK(thrown);                                                           \
  } while (false)

Message sample_message() {
  Message m;
  m.set_type(v1::TEXT);
  m.set_seq(42);
  m.set_from("dev-2077");
  m.set_to("dev-0042");
  m.set_ts_ms(1727848800123);
  m.mutable_text()->set_text("对账单已归档，可检索");
  return m;
}

void test_roundtrip() {
  const Message m = sample_message();
  const Message back = decode_frame(encode(m));
  CHECK(back.type() == m.type());
  CHECK(back.seq() == m.seq());
  CHECK(back.from() == m.from());
  CHECK(back.to() == m.to());
  CHECK(back.ts_ms() == m.ts_ms());
  CHECK(back.has_text());
  CHECK(back.text().text() == "对账单已归档，可检索");

  // 纯载荷对偶（平台-8 安全通道输入面）：encode＝长度前缀＋encode_payload
  const std::string payload = encode_payload(m);
  const std::string full = encode(m);
  CHECK(full.size() == kLengthPrefixSize + payload.size());
  CHECK(full.compare(kLengthPrefixSize, payload.size(), payload) == 0);
  CHECK(decode_payload(payload).text().text() == m.text().text());

  // 平台-8 握手字段（Hello bytes 扩展）往返
  Message h;
  h.set_type(v1::HELLO);
  h.set_from("dev-A");
  h.set_to("dev-B");
  h.mutable_hello()->set_proto_version(2);
  h.mutable_hello()->set_identity_pub(std::string(32, '\x01'));
  h.mutable_hello()->set_eph_pub(std::string(32, '\x02'));
  h.mutable_hello()->set_nonce(std::string(16, '\x03'));
  h.mutable_hello()->set_sig(std::string(64, '\x04'));
  const Message hb = decode_frame(encode(h));
  CHECK(hb.hello().proto_version() == 2);
  CHECK(hb.hello().identity_pub().size() == 32);
  CHECK(hb.hello().eph_pub().size() == 32);
  CHECK(hb.hello().nonce().size() == 16);
  CHECK(hb.hello().sig().size() == 64);

  // 各类型字段逐一往返
  Message f;
  f.set_type(v1::FILE_META);
  f.mutable_file_meta()->set_transfer_id("t-1");
  f.mutable_file_meta()->set_rel_path("docs/报告.pdf");
  f.mutable_file_meta()->set_size(120u * 1024 * 1024);
  f.mutable_file_meta()->set_sha256("ab12");
  const Message fb = decode_frame(encode(f));
  CHECK(fb.file_meta().transfer_id() == "t-1");
  CHECK(fb.file_meta().rel_path() == "docs/报告.pdf");
  CHECK(fb.file_meta().size() == 120u * 1024 * 1024);
  CHECK(fb.file_meta().sha256() == "ab12");

  Message d;
  d.set_type(v1::FILE_DONE);
  d.mutable_file_done()->set_ok(false);
  d.mutable_file_done()->set_error("哈希不一致");
  const Message db = decode_frame(encode(d));
  CHECK(!db.file_done().ok());
  CHECK(db.file_done().error() == "哈希不一致");
}

void test_stream_split() {
  const std::string frame = encode(sample_message());
  FrameDecoder decoder;
  std::vector<std::string> out;
  DecodeStatus st = DecodeStatus::kNeedMoreData;
  for (char c : frame) { // 逐字节喂入：半包
    st = decoder.feed(std::string_view(&c, 1), out);
    if (st == DecodeStatus::kZeroLength || st == DecodeStatus::kTooLarge) break;
  }
  CHECK(st == DecodeStatus::kOk);
  CHECK(out.size() == 1);
}

void test_batch_and_pipelining() {
  const std::string f1 = encode(sample_message());
  const std::string f2 = encode(sample_message());
  FrameDecoder decoder;
  std::vector<std::string> out;
  const DecodeStatus st = decoder.feed(f1 + f2, out); // 粘包：两帧一次到达
  CHECK(st == DecodeStatus::kOk);
  CHECK(out.size() == 2);
}

void test_malformed() {
  std::vector<std::string> out;
  FrameDecoder decoder;
  // 长度 0
  const char zero[4] = {0, 0, 0, 0};
  CHECK(decoder.feed(std::string_view(zero, 4), out) == DecodeStatus::kZeroLength);
  // 长度超限
  const char huge[4] = {static_cast<char>(0x7F), static_cast<char>(0xFF),
                        static_cast<char>(0xFF), static_cast<char>(0xFF)};
  decoder.reset();
  CHECK(decoder.feed(std::string_view(huge, 4), out) == DecodeStatus::kTooLarge);
  // 前缀不完整
  decoder.reset();
  CHECK(decoder.feed(std::string_view(zero, 2), out) == DecodeStatus::kNeedMoreData);
  // 空载荷编码拒绝
  CHECK_THROWS(static_cast<void>(encode_frame("")));
  // 超限载荷编码拒绝
  std::string big(static_cast<std::size_t>(kMaxFrameSize) + 1, 'a');
  CHECK_THROWS(static_cast<void>(encode_frame(big)));
}

void test_bad_payloads() {
  // 随机垃圾字节：解析必须报错而非崩溃
  const char garbage[] = "\x0a\xff\xfe\x00not-proto-at-all\x12";
  CHECK_THROWS(static_cast<void>(
      decode_payload(std::string_view(garbage, sizeof(garbage) - 1))));
  // 空载荷
  CHECK_THROWS(static_cast<void>(decode_payload("")));
  // 帧过短
  CHECK_THROWS(static_cast<void>(decode_frame(std::string_view("\x00\x00\x00\x02x", 5))));
}

// 前向兼容：对端升协议加字段后，本端解析不失败、已知字段不丢。
// 手工在序列化尾部追加未知字段（编号 9999，varint），模拟新版本消息。
void test_unknown_field_tolerance() {
  std::string payload;
  sample_message().SerializeToString(&payload);
  // field 9999, wire type 0 → tag varint = (9999 << 3) | 0 = 79992 → 0xB8 0xF0 0x04，值 1
  payload.append("\xB8\xF0\x04\x01", 4);

  const Message back = decode_frame(encode_frame(payload));
  CHECK(back.type() == v1::TEXT);
  CHECK(back.has_text());
  CHECK(back.text().text() == "对账单已归档，可检索");
}

void test_type_names() {
  CHECK(std::string(msg_type_name(v1::HELLO)) == "hello");
  CHECK(std::string(msg_type_name(v1::TEXT)) == "text");
  CHECK(std::string(msg_type_name(v1::FILE_DONE)) == "file_done");
  CHECK(std::string(msg_type_name(v1::LOGIN)) == "login");
  CHECK(std::string(msg_type_name(v1::KICK)) == "kick");
}

// 振屏（需求批⑥）：空体 NUDGE——类型即信号；oneof 字段号 153 对账。
// 空 message 进 oneof 须显式 mutable_nudge() 置位（proto3 语义：未置位
// 即 has_nudge()==false 且不序列化）
void test_nudge() {
  Message m;
  m.set_type(v1::NUDGE);
  m.mutable_nudge();
  m.set_seq(7);
  m.set_from("dev-A");
  m.set_to("dev-B");
  m.set_ts_ms(1727848800456);
  const Message back = decode_frame(encode(m));
  CHECK(back.type() == v1::NUDGE);
  CHECK(back.has_nudge());
  CHECK(back.nudge().ByteSizeLong() == 0); // 空体：无载荷字节
  CHECK(back.seq() == 7);
  CHECK(back.from() == "dev-A");
  CHECK(back.to() == "dev-B");
  CHECK(back.ts_ms() == m.ts_ms());
  CHECK(std::string(msg_type_name(v1::NUDGE)) == "nudge");
}

} // namespace

int main() {
  test_roundtrip();
  test_stream_split();
  test_batch_and_pipelining();
  test_malformed();
  test_bad_payloads();
  test_unknown_field_tolerance();
  test_type_names();
  test_nudge();
  if (g_failures == 0) {
    std::cout << "protocol tests: all passed\n";
    return 0;
  }
  std::cerr << "protocol tests: " << g_failures << " failure(s)\n";
  return 1;
}
