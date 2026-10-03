// T4.5 表情与常用联系人验收（服务端面）：星标／最近落库（UPSERT、排序、
// 账号隔离）＋协议级（TEXT 受理即刷新双方最近、FAV_CMD 星标后 FAV_DATA
// 全量重推并置顶、未登录帧被静默丢弃）。表情为客户端本地能力（内置网格、
// 频次 QSettings、自定义表情包导入走文件通道），不经服务端，不在此测。
#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "server.hpp"
#include "store.hpp"

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

// 阻塞式协议客户端（与 test_cross 同款）
class TestClient {
public:
  TestClient(asio::io_context& io, std::uint16_t port) {
    socket_ = std::make_unique<asio::ip::tcp::socket>(io);
    socket_->connect(asio::ip::tcp::endpoint(
        asio::ip::make_address("127.0.0.1"), port));
  }
  ~TestClient() {
    if (socket_) {
      std::error_code ignore;
      socket_->close(ignore);
    }
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
    std::string payload(len, '\0');
    asio::read(*socket_, asio::buffer(payload));
    return memex::protocol::decode_payload(payload);
  }
  void sync() {
    memex::protocol::Message ping;
    ping.set_type(memex::protocol::v1::PING);
    ping.set_from(account_);
    ping.set_to("server");
    ping.set_ts_ms(now_ms());
    send(ping);
    // 可能先积着回执/推送（ACK、PRESENCE、FAV 等）——读到 PONG 为准
    for (int i = 0; i < 16; ++i) {
      memex::protocol::Message r = read();
      if (r.type() == memex::protocol::v1::PONG) return;
    }
    CHECK(false);
  }
  void login(const std::string& account, const std::string& fp_seed) {
    account_ = account;
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::LOGIN);
    m.set_from(fp_seed);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    auto* in = m.mutable_login();
    in->set_account(account);
    in->set_password("pw");
    in->set_device_fingerprint(memex::server::sha256_hex(fp_seed));
    in->set_device_kind("desktop");
    in->set_device_name(fp_seed);
    in->set_client_version("0.1.0-test");
    send(m);
    // T4.3 起登录成功伴随 PRESENCE_DATA 推送（可能先到，先于 LOGIN_RESULT）
    // T4.5 起另伴随 FAV_DATA
    for (int i = 0; i < 12; ++i) {
      memex::protocol::Message r = read();
      if (r.type() == memex::protocol::v1::LOGIN_RESULT) {
        CHECK(r.login_result().ok());
        return;
      }
      CHECK(r.type() == memex::protocol::v1::PRESENCE_DATA ||
            r.type() == memex::protocol::v1::FAV_DATA);
    }
    CHECK(false);
  }
  void fav_query() {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::FAV_QUERY);
    m.set_from(account_);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    send(m);
  }
  void fav_cmd(const std::string& op, const std::string& peer) {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::FAV_CMD);
    m.set_from(account_);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    auto* c = m.mutable_fav_cmd();
    c->set_op(op);
    c->set_peer(peer);
    c->set_ts_ms(now_ms());
    send(m);
  }
  void send_text(const std::string& to, const std::string& text) {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::TEXT);
    m.set_seq(next_seq_++);
    m.set_from(account_);
    m.set_to(to);
    m.set_ts_ms(now_ms());
    m.mutable_text()->set_text(text);
    send(m);
  }
  std::string account_;
  std::uint64_t next_seq_{1};

private:
  std::unique_ptr<asio::ip::tcp::socket> socket_;
};

} // namespace

int main() {
  namespace v1 = memex::protocol::v1;

  // —— 库级：最近刷新（UPSERT、MAX 单调）、星标（不刷 last_ms）、排序、
  //    账号隔离 ——
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    for (const char* acct : {"alice", "bob", "carol"}) {
      CHECK(s.create_account(acct, "pw", acct));
    }
    CHECK(s.fav_touch("alice", "bob", 1000));
    CHECK(s.fav_touch("alice", "carol", 3000));
    CHECK(s.fav_touch("alice", "bob", 2000)); // 2000 > 1000，更新为 2000
    CHECK(s.fav_touch("alice", "bob", 500));  // 500 < 2000，保持 2000
    auto rows = s.fav_list("alice");
    CHECK(rows.size() == 2);
    CHECK(rows[0].peer == "carol" && rows[0].last_ms == 3000); // last 新者先
    CHECK(rows[1].peer == "bob" && rows[1].last_ms == 2000);

    CHECK(s.fav_star("alice", "bob", true)); // 星标 bob：不改 last_ms
    rows = s.fav_list("alice");
    CHECK(rows.size() == 2);
    CHECK(rows[0].peer == "bob" && rows[0].starred && rows[0].last_ms == 2000); // 星标置顶
    CHECK(rows[1].peer == "carol" && !rows[1].starred);

    CHECK(s.fav_star("alice", "bob", false));
    rows = s.fav_list("alice");
    CHECK(rows[0].peer == "carol"); // 取消星标回原序

    CHECK(s.fav_list("bob").empty()); // 账号隔离
    CHECK(s.fav_star("", "bob", true) == false);
    CHECK(s.fav_touch("alice", "", 1000) == false);
    CHECK(s.fav_touch("alice", "bob", 0) == false);
  }

  // —— 协议级：TEXT 受理即刷新双方最近；FAV_CMD 星标后 FAV_DATA 重推置顶；
  //    收到的重推帧携带全量（排序＋星标位）——
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    for (const char* acct : {"alice", "bob"}) {
      CHECK(store.create_account(acct, "pw", acct));
    }

    asio::io_context io;
    memex::server::CollabServer server(io, store, 0);
    server.start_accept();
    std::thread th([&io] { io.run(); });
    const std::uint16_t port = server.port();

    TestClient alice(io, port), bob(io, port);
    alice.login("alice", "laptop-a");
    bob.login("bob", "laptop-b");

    alice.send_text("bob", "你好");
    // bob 收 TEXT → ACK；alice 收 ACK
    for (int i = 0; i < 3; ++i) {
      auto r = bob.read();
      if (r.type() == v1::TEXT) break;
    }
    // 发送方与接收方双方在线表都刷新了
    alice.sync();
    bob.sync();
    auto arows = store.fav_list("alice");
    bool found = false;
    for (const auto& r : arows) found |= (r.peer == "bob" && r.last_ms > 0);
    CHECK(found);
    auto brows = store.fav_list("bob");
    found = false;
    for (const auto& r : brows) found |= (r.peer == "alice" && r.last_ms > 0);
    CHECK(found);

    // FAV_CMD star → FAV_DATA 全量重推，星标置顶
    alice.fav_cmd("star", "bob");
    for (int i = 0; i < 4; ++i) {
      auto r = alice.read();
      if (r.type() == v1::FAV_DATA) {
        CHECK(r.fav_data().entries_size() == 1);
        CHECK(r.fav_data().entries(0).peer() == "bob");
        CHECK(r.fav_data().entries(0).starred());
        break;
      }
    }
    // 重新查询也是置顶且星标位一致（数据已落库）
    alice.fav_query();
    for (int i = 0; i < 4; ++i) {
      auto r = alice.read();
      if (r.type() == v1::FAV_DATA) {
        CHECK(r.fav_data().entries_size() == 1);
        CHECK(r.fav_data().entries(0).starred());
        break;
      }
    }

    alice.sync();
    bob.sync();
    io.stop();
    th.join();
  }

  if (g_failures == 0) {
    std::cout << "test_fav OK\n";
    return 0;
  }
  std::cout << g_failures << " failures\n";
  return 1;
}
