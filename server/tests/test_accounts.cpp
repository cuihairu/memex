// T2.1 验收：账号表＋密码摘要登录、设备指纹留档、登录记录全量可查、
// 桌面端单点在线互踢（第二台登录后第一台收到 KICK 下线提示）。
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

  void send(const memex::protocol::Message& msg) {
    const std::string frame = memex::protocol::encode(msg);
    asio::write(*socket_, asio::buffer(frame));
  }

  memex::protocol::Message read() {
    std::array<char, 4> head{};
    asio::read(*socket_, asio::buffer(head));
    const std::uint32_t len = (std::uint8_t(head[0]) << 24) |
                              (std::uint8_t(head[1]) << 16) |
                              (std::uint8_t(head[2]) << 8) |
                              std::uint8_t(head[3]);
    CHECK(len > 0 && len < memex::protocol::kMaxFrameSize);
    std::string payload(len, '\0');
    asio::read(*socket_, asio::buffer(payload));
    return memex::protocol::decode_payload(payload);
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

void test_store_basics(ServerStore& store) {
  CHECK(store.create_account("alice", "pass-123", "Alice"));
  CHECK(!store.create_account("alice", "another", "Alice2")); // 幂等拒绝
  const auto row = store.find_account("alice");
  CHECK(row.has_value());
  CHECK(row->display_name == "Alice");
  CHECK(row->salt_hex.size() == 32);           // 16 字节盐 hex
  CHECK(row->digest_hex != "pass-123");        // 不落明文
  CHECK(row->digest_hex.size() == 64);         // SHA-256 hex
  // 摘要可复算（登录校验路径的存储半边）
  CHECK(memex::server::pbkdf2_sha256_hex("pass-123", row->salt_hex, 60000) ==
        row->digest_hex);
  CHECK(!store.find_account("nobody").has_value());
}

void test_login_and_kick(ServerStore& store) {
  store.create_account("bob", "secret-9", "Bob");

  asio::io_context io;
  CollabServer server(io, store, 0); // 系统分配端口
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  // 第一台桌面登录成功
  TestClient a(io, port);
  a.send(make_login("bob", "secret-9", "workstation-A"));
  const auto r1 = a.read();
  CHECK(r1.type() == memex::protocol::v1::LOGIN_RESULT);
  CHECK(r1.has_login_result());
  CHECK(r1.login_result().ok());
  CHECK(r1.login_result().display_name() == "Bob");

  // 心跳通路仍可用（登录态下）
  memex::protocol::Message ping;
  ping.set_type(memex::protocol::v1::PING);
  ping.set_from("bob");
  ping.set_seq(7);
  a.send(ping);
  const auto pong = a.read();
  CHECK(pong.type() == memex::protocol::v1::PONG);
  CHECK(pong.seq() == 7);

  // 第二台同账号桌面登录：第一台被原子化踢出并收到 KICK 提示
  TestClient b(io, port);
  b.send(make_login("bob", "secret-9", "workstation-B"));
  const auto r2 = b.read();
  CHECK(r2.login_result().ok());
  const auto kick = a.read();
  CHECK(kick.type() == memex::protocol::v1::KICK);
  CHECK(kick.has_kick());
  CHECK(kick.kick().reason().find("单点在线") != std::string::npos);
  CHECK(kick.kick().replaced_by() == "workstation-B");
  CHECK(a.closed_after_kick());

  // 被踢后第二台仍在线（心跳可达），旧连接不再顶替
  b.send(ping);
  CHECK(b.read().type() == memex::protocol::v1::PONG);

  // 口令不符：拒绝且留痕
  TestClient c(io, port);
  c.send(make_login("bob", "wrong", "workstation-C"));
  const auto r3 = c.read();
  CHECK(!r3.login_result().ok());
  CHECK(r3.login_result().reason() == "口令不符");

  // 不存在的账号：拒绝且留痕
  TestClient d(io, port);
  d.send(make_login("ghost", "x", "workstation-D"));
  const auto r4 = d.read();
  CHECK(!r4.login_result().ok());
  CHECK(r4.login_result().reason() == "账号不存在");

  // 登录记录全量可查：bob 共 4 条（成功×2、口令不符、账号不存在属于 ghost）
  const auto rows = store.login_records("bob");
  CHECK(rows.size() == 3);
  int ok_count = 0, bad_count = 0;
  for (const auto& r : rows) {
    CHECK(r.account == "bob");
    CHECK(r.source_ip == "127.0.0.1");
    CHECK(r.version == "0.1.0-test");
    CHECK(!r.fingerprint.empty());
    if (r.result == "ok") {
      ++ok_count;
      CHECK(r.kind == "desktop");
      CHECK((r.name == "workstation-A" || r.name == "workstation-B"));
    } else {
      ++bad_count;
      CHECK(r.result == "bad_password");
    }
  }
  CHECK(ok_count == 2);
  CHECK(bad_count == 1);
  const auto ghost_rows = store.login_records("ghost");
  CHECK(ghost_rows.size() == 1);
  CHECK(ghost_rows[0].result == "no_account");

  io.stop();
  io_thread.join();
}

} // namespace

int main() {
  memex::server::ServerStore store;
  CHECK(store.open(":memory:"));
  test_store_basics(store);

  memex::server::ServerStore store2;
  CHECK(store2.open(":memory:"));
  test_login_and_kick(store2);

  if (g_failures == 0) {
    std::cout << "accounts tests: all passed\n";
    return 0;
  }
  std::cerr << "accounts tests: " << g_failures << " failure(s)\n";
  return 1;
}
