// T4.3 消息状态与多端验收（服务端面）：已读上报留痕（伪造 msg_id 拒绝、
// 重复上报幂等）＋ READ_NOTICE 路由（发送方在线即收、离线仅留痕）＋
// 在线表查询与变更推送（登录/登出/断开即广播，含自己）。
// 库级（ServerStore 直测）＋协议级（真实连接＋PING/PONG 同步点）。
#include <asio.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// 阻塞式协议客户端（与 test_group/test_cross 同款）
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
      // T4.5：TEXT 受理后的 FAV 全量回推不串扰帧序断言
      if (msg.type() != memex::protocol::v1::FAV_DATA) return msg;
    }
  }
  // 同步点：PING/PONG 往返后，服务端已处理完此前发来的全部帧
  void sync() {
    memex::protocol::Message ping;
    ping.set_type(memex::protocol::v1::PING);
    ping.set_from(account_);
    ping.set_to("server");
    ping.set_ts_ms(now_ms());
    send(ping);
    // T4.5：FAV 全量回推可能插队——读到 PONG 为止
    bool saw_pong = false;
    for (int i = 0; i < 16 && !saw_pong; ++i) {
      const auto r = read();
      if (r.type() == memex::protocol::v1::PONG) saw_pong = true;
    }
    CHECK(saw_pong);
  }
  // 登录：T4.3 起登录成功伴随 PRESENCE_DATA 推送（先于 LOGIN_RESULT），
  // 逐帧读到 LOGIN_RESULT 为止（推送帧顺手断言在线表内容）。
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
    for (int i = 0; i < 8; ++i) {
      memex::protocol::Message r = read();
      if (r.type() == memex::protocol::v1::LOGIN_RESULT) {
        CHECK(r.login_result().ok());
        return;
      }
      // 登录推送：在线表必含自己
      CHECK(r.type() == memex::protocol::v1::PRESENCE_DATA);
      if (r.type() == memex::protocol::v1::PRESENCE_DATA) {
        CHECK(has_account(r, account));
      }
    }
    CHECK(false); // 8 帧内未见 LOGIN_RESULT
  }
  void read_msg(const std::string& msg_id) {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::READ);
    m.set_from(account_);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    m.mutable_read()->set_msg_id(msg_id);
    send(m);
  }
  void query_presence() {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::PRESENCE_QUERY);
    m.set_from(account_);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    send(m);
  }
  static bool has_account(const memex::protocol::Message& m,
                          const std::string& account) {
    for (const auto& a : m.presence_data().accounts()) {
      if (a == account) return true;
    }
    return false;
  }
  static std::vector<std::string> accounts_of(
      const memex::protocol::Message& m) {
    return {m.presence_data().accounts().begin(),
            m.presence_data().accounts().end()};
  }
  std::string account_;

private:
  std::unique_ptr<asio::ip::tcp::socket> socket_;
};

} // namespace

int main() {
  namespace v1 = memex::protocol::v1;

  // —— 库级：已读留痕（拒绝条件／幂等／查询）——
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    CHECK(s.create_account("alice", "pw", "alice"));
    CHECK(s.create_account("bob", "pw", "bob"));

    CHECK(s.store_message("m1", "alice", "bob", 10, "hi", 5000));
    CHECK(s.record_read("m1", "bob", 6000));
    CHECK(s.record_read("m1", "bob", 6500)); // 重复上报幂等（首条为准）
    CHECK(s.record_read("", "bob", 6000) == false);
    CHECK(s.record_read("m1", "", 6000) == false);
    CHECK(s.record_read("m1", "bob", 0) == false);
    CHECK(s.record_read("no-such-msg", "bob", 6000) == false); // 伪造拒绝
    auto rows = s.readers_for("m1");
    CHECK(rows.size() == 1);
    CHECK(rows[0].reader == "bob" && rows[0].read_ms == 6000);
    CHECK(s.readers_for("m1").size() == 1);
    CHECK(s.readers_for("no-such-msg").empty());
    s.close();
  }

  // —— 协议级：上报→留痕→发送方在线即收 NOTICE；在线表查询与推送 ——
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    for (const char* acct : {"alice", "bob"}) {
      CHECK(s.create_account(acct, "pw", acct));
    }
    asio::io_context io;
    memex::server::CollabServer server(io, s, 0);
    server.start_accept();
    std::thread io_thread([&] { io.run(); });

    TestClient a(io, server.port());
    {
      TestClient b(io, server.port());
      a.login("alice", "pc-a"); // 推送帧已在 login 内断言（含自己）
      b.login("bob", "pc-b");

    // bob 上线 → alice 收到变更推送（含双方）
    {
      const auto r = a.read();
      CHECK(r.type() == v1::PRESENCE_DATA);
      CHECK(TestClient::has_account(r, "alice"));
      CHECK(TestClient::has_account(r, "bob"));
    }
    // 在线表查询回执（与推送同结构）
    b.query_presence();
    {
      const auto r = b.read();
      CHECK(r.type() == v1::PRESENCE_DATA);
      CHECK(TestClient::accounts_of(r) ==
            std::vector<std::string>({"alice", "bob"})); // 服务端排序后
    }

    // alice→bob 发一条归档消息（bob 未读）
    memex::protocol::Message t;
    t.set_type(v1::TEXT);
    t.set_seq(3);
    t.set_from("alice");
    t.set_to("bob");
    t.set_ts_ms(now_ms());
    t.mutable_text()->set_text("已读验收消息");
    a.send(t);
    CHECK(a.read().type() == v1::ACK);
    const auto incoming = b.read();
    CHECK(incoming.type() == v1::TEXT);
    const std::string mid = incoming.msg_id();
    CHECK(!mid.empty());

    // bob 上报已读 → alice 在线，即收 READ_NOTICE（msg_id/已读方正确）
    b.read_msg(mid);
    b.sync();
    {
      // T4.5：FAV 全量回推可能插在 ACK 与 READ_NOTICE 之间——读到 NOTICE 为止
      memex::protocol::Message n;
      bool saw = false;
      for (int i = 0; i < 8 && !saw; ++i) {
        n = a.read();
        if (n.type() == v1::READ_NOTICE) saw = true;
      }
      CHECK(saw);
      CHECK(n.read_notice().msg_id() == mid);
      CHECK(n.read_notice().reader() == "bob");
      CHECK(n.read_notice().read_ms() > 0);
    }
    {
      const auto rows = s.readers_for(mid);
      CHECK(rows.size() == 1 && rows[0].reader == "bob");
    }

    // 重复上报幂等：无第二条 NOTICE（sync 回 PONG 即证明队列干净）
    b.read_msg(mid);
    b.sync();
    CHECK(s.readers_for(mid).size() == 1);

    // 伪造 msg_id 上报：不留痕、无 NOTICE（sync 直接回 PONG）
    b.read_msg("forged-msg-id");
    b.sync();
    } // b 析构断开 → 服务端除名并广播（alice 侧断言推送）
    // bob 断开 → alice 收到变更推送（只剩自己）
    {
      const auto r = a.read();
      CHECK(r.type() == v1::PRESENCE_DATA);
      CHECK(TestClient::accounts_of(r) ==
            std::vector<std::string>({"alice"}));
    }
    // 断开后查询回执同样只剩自己
    a.query_presence();
    {
      const auto r = a.read();
      CHECK(r.type() == v1::PRESENCE_DATA);
      CHECK(TestClient::accounts_of(r) ==
            std::vector<std::string>({"alice"}));
    }

    io.stop();
    io_thread.join();
    s.close();
  }

  if (g_failures == 0) {
    std::cout << "read_presence tests: all passed\n";
    return 0;
  }
  std::cerr << "read_presence tests: " << g_failures << " failure(s)\n";
  return 1;
}
