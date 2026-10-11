// 平台-1 授权盘点补漏测试（todo「服务端授权专项审查」输出腿）：伪造发送
// 者（TEXT/NUDGE 归档与投递绑定会话身份）、伪造回执（delivered/read 事件
// 仅收件人可记）、设备停用语义（存量会话钉现状＋新登录闸）。越权撤回
// 已有 test_archive 覆盖。服务端核心库直链运行（io 线程驱动，不起进程），
// 骨架同 test_gateway。
#include <asio.hpp>

#include <chrono>
#include <iostream>
#include <thread>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "server.hpp"
#include "store.hpp"

using asio::ip::tcp;
using memex::server::CollabServer;
using memex::server::ServerStore;

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

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 阻塞式测试客户端：帧收发（长度前缀 + Envelope）
class TestClient {
public:
  TestClient(asio::io_context& io, std::uint16_t port) {
    socket_ = std::make_unique<tcp::socket>(io);
    socket_->connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  }

  ~TestClient() {
    if (socket_) {
      std::error_code ignore;
      socket_->close(ignore);
    }
  }

  TestClient(const TestClient&) = delete;
  TestClient& operator=(const TestClient&) = delete;

  void send(const memex::protocol::Message& msg) {
    const std::string frame = memex::protocol::encode(msg);
    asio::write(*socket_, asio::buffer(frame));
  }

  memex::protocol::Message read() {
    for (;;) {
      std::array<char, 4> head{};
      asio::read(*socket_, asio::buffer(head));
      const std::uint32_t len = (std::uint8_t(head[0]) << 24) |
                                (std::uint8_t(head[1]) << 16) |
                                (std::uint8_t(head[2]) << 8) |
                                std::uint8_t(head[3]);
      std::string payload(len, '\0');
      asio::read(*socket_, asio::buffer(payload));
      auto msg = memex::protocol::decode_payload(payload);
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA &&
          msg.type() != memex::protocol::v1::FAV_DATA)
        return msg;
    }
  }

private:
  std::unique_ptr<tcp::socket> socket_;
};

memex::protocol::Message make_login(const std::string& account,
                                    const std::string& password,
                                    const std::string& device_name,
                                    const std::string& kind = "desktop") {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::LOGIN);
  m.set_from(device_name);
  m.set_to("server");
  m.set_ts_ms(now_ms());
  auto* in = m.mutable_login();
  in->set_account(account);
  in->set_password(password);
  in->set_device_fingerprint(memex::server::sha256_hex(device_name));
  in->set_device_kind(kind);
  in->set_device_name(device_name);
  in->set_client_version("0.1.0-test");
  return m;
}

memex::protocol::Message make_text(const std::string& from, const std::string& to,
                                   std::uint64_t seq, const std::string& body) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::TEXT);
  m.set_seq(seq);
  m.set_from(from);
  m.set_to(to);
  m.set_ts_ms(now_ms());
  m.mutable_text()->set_text(body);
  return m;
}

memex::protocol::Message make_nudge(const std::string& from,
                                    const std::string& to, std::uint64_t seq) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::NUDGE);
  m.set_seq(seq);
  m.set_from(from);
  m.set_to(to);
  m.set_ts_ms(now_ms());
  m.mutable_nudge();
  return m;
}

memex::protocol::Message make_ack(const std::string& from,
                                  const std::string& msg_id) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::ACK);
  m.set_from(from);
  m.set_to("server");
  m.mutable_ack()->set_msg_id(msg_id);
  return m;
}

memex::protocol::Message make_read(const std::string& from,
                                   const std::string& msg_id) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::READ);
  m.set_from(from);
  m.set_to("server");
  m.mutable_read()->set_msg_id(msg_id);
  return m;
}

void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(80)); }

// 伪造发送者（TEXT）：报称他人被拒收不留痕，本人发送照常受理
void test_forged_from_text(ServerStore& store) {
  store.create_account("alice", "pa-1", "Alice");
  store.create_account("bob", "pb-1", "Bob");
  store.create_account("eve", "pe-1", "Eve");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient a(io, port), b(io, port);
  a.send(make_login("alice", "pa-1", "pc-a1"));
  CHECK(a.read().login_result().ok());
  b.send(make_login("bob", "pb-1", "pc-b1"));
  CHECK(b.read().login_result().ok());

  // 伪造：alice 会话报称 eve——拒收（无受理回执、不归档、不投递）
  a.send(make_text("eve", "bob", 1, "冒名消息"));
  // 控制腿：本人发送照常（下一帧回执必须属本次发送，seq 对号）
  a.send(make_text("alice", "bob", 2, "本人消息"));
  const auto receipt = a.read();
  CHECK(receipt.type() == memex::protocol::v1::ACK);
  CHECK(receipt.seq() == 2); // 伪造帧若漏闸会先回 seq=1 的回执

  settle();
  CHECK(!store.message_by_id(
      memex::server::sha256_hex("eve:1"))); // 冒名不归档
  CHECK(store.offline_count("bob") == 1);   // 入队的只有真消息（未 ACK 前保留）
  const auto legit = store.message_by_id(memex::server::sha256_hex("alice:2"));
  CHECK(legit && legit->from_account == "alice" &&
        legit->text == "本人消息");

  io.stop();
  io_thread.join();
}

// 伪造发送者（NUDGE）：振屏归档含 from，同样绑定会话身份
void test_forged_from_nudge(ServerStore& store) {
  store.create_account("alice", "pa-2", "Alice");
  store.create_account("bob", "pb-2", "Bob");
  store.create_account("eve", "pe-2", "Eve");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient a(io, port);
  a.send(make_login("alice", "pa-2", "pc-a2"));
  CHECK(a.read().login_result().ok());

  a.send(make_nudge("eve", "bob", 3));
  a.send(make_text("alice", "bob", 4, "对照消息"));
  const auto receipt = a.read();
  CHECK(receipt.type() == memex::protocol::v1::ACK);
  CHECK(receipt.seq() == 4);

  settle();
  CHECK(!store.message_by_id(memex::server::sha256_hex("eve:3")));
  CHECK(store.message_by_id(memex::server::sha256_hex("alice:4")));

  io.stop();
  io_thread.join();
}

// 伪造回执（ACK）：非收件人回执不清队、不留 delivered 事件；
// 真收件人回执照常留痕（对照）
void test_forged_ack(ServerStore& store) {
  store.create_account("alice", "pa-3", "Alice");
  store.create_account("bob", "pb-3", "Bob");
  store.create_account("carol", "pc-3", "Carol");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient a(io, port), c(io, port);
  a.send(make_login("alice", "pa-3", "pc-a3"));
  CHECK(a.read().login_result().ok());
  c.send(make_login("carol", "pc-3", "pc-c3"));
  CHECK(c.read().login_result().ok());

  // bob 离线：alice 发送入队
  a.send(make_text("alice", "bob", 5, "离线消息"));
  CHECK(a.read().type() == memex::protocol::v1::ACK);
  const std::string msg_id = memex::server::sha256_hex("alice:5");
  CHECK(store.offline_count("bob") == 1);

  // 伪造：carol（非收件人）对他人消息 ACK——不留痕
  c.send(make_ack("carol", msg_id));
  settle();
  CHECK(store.offline_count("bob") == 1); // 队列不受伪造回执影响
  CHECK(!store.delivered_recorded(msg_id, "carol"));

  // 对照：真收件人 bob 登录补投后 ACK——照常留痕
  TestClient b(io, port);
  b.send(make_login("bob", "pb-3", "pc-b3"));
  CHECK(b.read().login_result().ok());
  const auto redelivered = b.read();
  CHECK(redelivered.type() == memex::protocol::v1::TEXT);
  CHECK(redelivered.msg_id() == msg_id);
  b.send(make_ack("bob", msg_id));
  settle();
  CHECK(store.offline_count("bob") == 0);
  CHECK(store.delivered_recorded(msg_id, "bob"));

  io.stop();
  io_thread.join();
}

// 伪造回执（READ）：非收件人已读上报不留痕，真收件人照常
void test_forged_read(ServerStore& store) {
  store.create_account("alice", "pa-4", "Alice");
  store.create_account("bob", "pb-4", "Bob");
  store.create_account("carol", "pc-4", "Carol");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient a(io, port), c(io, port);
  a.send(make_login("alice", "pa-4", "pc-a4"));
  CHECK(a.read().login_result().ok());
  c.send(make_login("carol", "pc-4", "pc-c4"));
  CHECK(c.read().login_result().ok());

  a.send(make_text("alice", "bob", 6, "待读消息"));
  CHECK(a.read().type() == memex::protocol::v1::ACK);
  const std::string msg_id = memex::server::sha256_hex("alice:6");

  c.send(make_read("carol", msg_id));
  settle();
  bool carol_read = false;
  for (const auto& r : store.readers_for(msg_id))
    if (r.reader == "carol") carol_read = true;
  CHECK(!carol_read); // 非收件人已读不入名单

  TestClient b(io, port);
  b.send(make_login("bob", "pb-4", "pc-b4"));
  CHECK(b.read().login_result().ok());
  CHECK(b.read().type() == memex::protocol::v1::TEXT);
  b.send(make_read("bob", msg_id));
  settle();
  bool bob_read = false;
  for (const auto& r : store.readers_for(msg_id))
    if (r.reader == "bob") bob_read = true;
  CHECK(bob_read); // 真收件人照常

  io.stop();
  io_thread.join();
}

// 设备停用语义（盘点钉现状）：存量会话不受影响继续可用（停用闸＝新
// 登录拒绝，踢存量会话是另一决策面，见授权盘点文档）；同指纹新登录拒
void test_device_disable_semantics(ServerStore& store) {
  store.create_account("alice", "pa-6", "Alice");
  store.create_account("bob", "pb-6", "Bob");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient a(io, port);
  a.send(make_login("alice", "pa-6", "pc-a6"));
  CHECK(a.read().login_result().ok());

  CHECK(store.set_device_enabled(memex::server::sha256_hex("pc-a6"), false));

  // 存量会话钉现状：停用后仍可收发（旧会话生命周期独立于设备闸）
  a.send(make_text("alice", "bob", 8, "停用后存量会话"));
  const auto receipt = a.read();
  CHECK(receipt.type() == memex::protocol::v1::ACK);
  CHECK(store.message_by_id(memex::server::sha256_hex("alice:8")));

  // 同指纹新登录：闸住（设备停用的现行语义＝断新会话）
  TestClient a2(io, port);
  a2.send(make_login("alice", "pa-6", "pc-a6"));
  const auto denied = a2.read().login_result();
  CHECK(!denied.ok());

  io.stop();
  io_thread.join();
}

} // namespace

int main() {
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_forged_from_text(store);
  }
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_forged_from_nudge(store);
  }
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_forged_ack(store);
  }
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_forged_read(store);
  }
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_device_disable_semantics(store);
  }

  if (g_failures == 0) {
    std::cout << "authz audit tests: all passed\n";
    return 0;
  }
  std::cerr << "authz audit tests: " << g_failures << " failure(s)\n";
  return 1;
}
