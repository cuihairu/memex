// T2.2 服务端半边验收：消息路由（在线即投＋发送方受理回执）、
// 离线排队与登录补投、ACK 清队与未 ACK 重投、msg_id 确定性、
// 同类型设备互踢＋跨类型并存（桌面与手机各留一台）、多端同投单回执即清。
// 服务端核心库直链运行（io 线程驱动，不起进程）。
#include <asio.hpp>

#include <cassert>
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

  // T4.3 在线推送（PRESENCE_DATA）与本文件验收特性无关，自动跳过——
  // 在线表语义由 test_read_presence 显式验收。
  memex::protocol::Message read() {
    for (;;) {
      std::array<char, 4> head{};
      asio::read(*socket_, asio::buffer(head));
      const std::uint32_t len = (std::uint8_t(head[0]) << 24) |
                                (std::uint8_t(head[1]) << 16) |
                                (std::uint8_t(head[2]) << 8) |
                                std::uint8_t(head[3]);
      CHECK(len > 0 && len < memex::protocol::kMaxFrameSize);
      std::string payload(len, '\0');
      asio::read(*socket_, asio::buffer(payload));
      auto msg = memex::protocol::decode_payload(payload);
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA && msg.type() != memex::protocol::v1::FAV_DATA) return msg;
    }
  }

  // 对端关闭后读到 EOF（asio::read 抛 system_error）
  bool closed_after_kick() {
    try {
      std::array<char, 4> head{};
      asio::read(*socket_, asio::buffer(head));
      return false; // 还有数据，不算关闭
    } catch (const std::system_error&) {
      return true;
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

memex::protocol::Message make_ping(const std::string& from, std::uint64_t seq) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::PING);
  m.set_seq(seq);
  m.set_from(from);
  return m;
}

memex::protocol::Message make_ack(const std::string& msg_id) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::ACK);
  m.set_from("receiver");
  m.set_to("server");
  m.mutable_ack()->set_msg_id(msg_id);
  return m;
}

// 在线路由：在线即投（带 msg_id）、发送方受理回执（原 seq）、ACK 清队
void test_online_routing(ServerStore& store) {
  store.create_account("alice", "pa-1", "Alice");
  store.create_account("bob", "pb-1", "Bob");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient a(io, port), b(io, port);
  a.send(make_login("alice", "pa-1", "pc-alice"));
  CHECK(a.read().login_result().ok());
  b.send(make_login("bob", "pb-1", "pc-bob"));
  CHECK(b.read().login_result().ok());

  a.send(make_text("alice", "bob", 1, "在线直投"));
  const auto got = b.read();
  CHECK(got.type() == memex::protocol::v1::TEXT);
  CHECK(got.has_text());
  CHECK(got.text().text() == "在线直投");
  CHECK(got.from() == "alice");
  CHECK(got.msg_id() == memex::server::sha256_hex("alice:1")); // 去重键确定性

  const auto receipt = a.read();
  CHECK(receipt.type() == memex::protocol::v1::ACK);
  CHECK(receipt.seq() == 1); // 受理回执携发送方原 seq

  b.send(make_ack(got.msg_id()));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(store.offline_count("bob") == 0); // 回执清队

  io.stop();
  io_thread.join();
}

// 离线排队：接收方不在线先入队，登录即补投；未 ACK 的下次登录重投（同 msg_id）
void test_offline_queue_and_redelivery(ServerStore& store) {
  store.create_account("alice", "pa-2", "Alice");
  store.create_account("bob", "pb-2", "Bob");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient a(io, port);
  a.send(make_login("alice", "pa-2", "pc-alice"));
  CHECK(a.read().login_result().ok());

  // 接收方离线：入队＋受理回执
  a.send(make_text("alice", "bob", 5, "离线消息"));
  CHECK(a.read().type() == memex::protocol::v1::ACK);
  CHECK(store.offline_count("bob") == 1);

  // 登录补投：收到后不回 ACK 即断开
  {
    TestClient b(io, port);
    b.send(make_login("bob", "pb-2", "pc-bob"));
    CHECK(b.read().login_result().ok());
    const auto got = b.read();
    CHECK(got.type() == memex::protocol::v1::TEXT);
    CHECK(got.text().text() == "离线消息");
    CHECK(got.msg_id() == memex::server::sha256_hex("alice:5"));
  } // 析构断开，未 ACK
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(store.offline_count("bob") == 1); // 仍在队列

  // 再登录：重投同一条（同 msg_id，接收端可去重），这次 ACK 清队
  TestClient b2(io, port);
  b2.send(make_login("bob", "pb-2", "pc-bob"));
  CHECK(b2.read().login_result().ok());
  const auto again = b2.read();
  CHECK(again.type() == memex::protocol::v1::TEXT);
  CHECK(again.msg_id() == memex::server::sha256_hex("alice:5"));
  b2.send(make_ack(again.msg_id()));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(store.offline_count("bob") == 0);

  io.stop();
  io_thread.join();
}

// 同类型互踢＋跨类型并存：桌面与手机各留一台；多端在线时都投递，任一端 ACK 即清队
void test_multidevice(ServerStore& store) {
  store.create_account("alice", "pa-3", "Alice");
  store.create_account("bob", "pb-3", "Bob");

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  TestClient bob_d1(io, port), bob_m(io, port);
  bob_d1.send(make_login("bob", "pb-3", "pc-bob-1"));
  CHECK(bob_d1.read().login_result().ok());
  bob_m.send(make_login("bob", "pb-3", "phone-bob", "mobile"));
  CHECK(bob_m.read().login_result().ok()); // 跨类型并存：桌面不受影响
  bob_d1.send(make_ping("bob", 1));
  CHECK(bob_d1.read().type() == memex::protocol::v1::PONG);

  TestClient a(io, port);
  a.send(make_login("alice", "pa-3", "pc-alice"));
  CHECK(a.read().login_result().ok());

  // 多端同投：桌面与手机各收一份（同 msg_id），手机回执即清队
  a.send(make_text("alice", "bob", 1, "多端投递"));
  CHECK(a.read().type() == memex::protocol::v1::ACK);
  const auto to_desktop = bob_d1.read();
  const auto to_mobile = bob_m.read();
  CHECK(to_desktop.msg_id() == to_mobile.msg_id());
  CHECK(to_desktop.text().text() == "多端投递");
  bob_m.send(make_ack(to_mobile.msg_id()));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(store.offline_count("bob") == 0);
  // 需求批⑦：手机端 ACK 触发送达通知推给发送方 alice——先读掉再续帧序断言
  {
    const auto n = a.read();
    CHECK(n.type() == memex::protocol::v1::DELIVER_NOTICE);
    CHECK(n.deliver_notice().msg_id() == to_mobile.msg_id());
    CHECK(n.deliver_notice().delivered_to() == "bob");
  }

  // 第二台桌面登录：只踢第一台桌面，手机不动
  TestClient bob_d2(io, port);
  bob_d2.send(make_login("bob", "pb-3", "pc-bob-2"));
  CHECK(bob_d2.read().login_result().ok());
  const auto kick = bob_d1.read();
  CHECK(kick.type() == memex::protocol::v1::KICK);
  CHECK(kick.kick().reason().find("桌面端") != std::string::npos);
  CHECK(kick.kick().replaced_by() == "pc-bob-2");
  CHECK(bob_d1.closed_after_kick());

  bob_m.send(make_ping("bob", 2)); // 手机不受互踢影响
  CHECK(bob_m.read().type() == memex::protocol::v1::PONG);
  bob_d2.send(make_ping("bob", 3));
  CHECK(bob_d2.read().type() == memex::protocol::v1::PONG);

  // 顶替后投递只达新桌面与手机
  a.send(make_text("alice", "bob", 2, "顶替后"));
  CHECK(a.read().type() == memex::protocol::v1::ACK);
  CHECK(bob_d2.read().text().text() == "顶替后");
  CHECK(bob_m.read().text().text() == "顶替后");

  io.stop();
  io_thread.join();
}

} // namespace

int main() {
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_online_routing(store);
  }
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_offline_queue_and_redelivery(store);
  }
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    test_multidevice(store);
  }

  if (g_failures == 0) {
    std::cout << "gateway tests: all passed\n";
    return 0;
  }
  std::cerr << "gateway tests: " << g_failures << " failure(s)\n";
  return 1;
}
