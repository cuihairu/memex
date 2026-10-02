// 协议编解码单测：往返、粘包／半包、畸形输入不崩溃。
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
  m.type = MsgType::kText;
  m.seq = 42;
  m.from = "dev-2077";
  m.to = "dev-0042";
  m.ts_ms = 1727848800123;
  m.body = nlohmann::json{{"text", "对账单已归档，可检索"}, {"lang", "zh-CN"}};
  return m;
}

void test_roundtrip() {
  const Message m = sample_message();
  const Message back = Message::decode_frame(m.encode());
  CHECK(back.type == m.type);
  CHECK(back.seq == m.seq);
  CHECK(back.from == m.from);
  CHECK(back.to == m.to);
  CHECK(back.ts_ms == m.ts_ms);
  CHECK(back.body == m.body);
}

void test_stream_split() {
  const std::string frame = sample_message().encode();
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
  const std::string f1 = sample_message().encode();
  const std::string f2 = sample_message().encode();
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
  std::string frame;
  // 非法 JSON
  {
    const std::string payload = "{not json";
    frame = encode_frame(payload);
  }
  CHECK_THROWS(static_cast<void>(Message::decode_frame(frame)));
  // 合法 JSON 但缺字段
  {
    const std::string payload = nlohmann::json{{"type", 10}}.dump();
    frame = encode_frame(payload);
  }
  CHECK_THROWS(static_cast<void>(Message::decode_frame(frame)));
  // 帧过短
  CHECK_THROWS(static_cast<void>(Message::decode_frame("\x00\x00\x00\x02{}")));
}

} // namespace

int main() {
  test_roundtrip();
  test_stream_split();
  test_batch_and_pipelining();
  test_malformed();
  test_bad_payloads();
  if (g_failures == 0) {
    std::cout << "protocol tests: all passed\n";
    return 0;
  }
  std::cerr << "protocol tests: " << g_failures << " failure(s)\n";
  return 1;
}
