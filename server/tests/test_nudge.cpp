// 振屏（需求批⑥，服务端面）：NUDGE 空体帧受理——归档留痕（"[振屏]" 标记
// 文本，type=NUDGE）＋在线即投＋受理回执；不进离线补投（补投的抖动失去
// 时效且成骚扰）；连接级 1s 限频静默拒；群目标拒。
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

// 阻塞式协议客户端（与 test_read_presence/test_group 同款）
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
      // 振屏不触最近刷新，但登录推送 PRESENCE_DATA 等仍会插队——只滤 FAV
      if (msg.type() != memex::protocol::v1::FAV_DATA) return msg;
    }
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
    for (int i = 0; i < 8; ++i) {
      memex::protocol::Message r = read();
      if (r.type() == memex::protocol::v1::LOGIN_RESULT) {
        CHECK(r.login_result().ok());
        return;
      }
    }
    CHECK(false); // 未收到 LOGIN_RESULT
  }
  // 同步点：PING 后**只**应回一帧 PONG——任何先到的其他帧（受理回执/
  // 补投/拒单证据）都算断言失败。用于「此后不应有帧」的否证。
  void expect_pong_only() {
    memex::protocol::Message ping;
    ping.set_type(memex::protocol::v1::PING);
    ping.set_from(account_);
    ping.set_to("server");
    ping.set_ts_ms(now_ms());
    send(ping);
    const auto r = read();
    if (r.type() != memex::protocol::v1::PONG) {
      std::cerr << "FAIL expect_pong_only：先到帧 type="
                << static_cast<int>(r.type()) << '\n';
      ++g_failures;
    }
  }
  void nudge(std::uint64_t seq, const std::string& to) {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::NUDGE);
    m.mutable_nudge(); // 空 message 进 oneof 须显式置位（否则 has_nudge 假）
    m.set_seq(seq);
    m.set_from(account_);
    m.set_to(to);
    m.set_ts_ms(now_ms());
    send(m);
  }
  // 常规同步点：读到 PONG 为止（登录推送等中间帧顺手丢弃）
  void sync() {
    memex::protocol::Message ping;
    ping.set_type(memex::protocol::v1::PING);
    ping.set_from(account_);
    ping.set_to("server");
    ping.set_ts_ms(now_ms());
    send(ping);
    bool saw_pong = false;
    for (int i = 0; i < 16 && !saw_pong; ++i) {
      if (read().type() == memex::protocol::v1::PONG) saw_pong = true;
    }
    CHECK(saw_pong);
  }

private:
  std::unique_ptr<asio::ip::tcp::socket> socket_;
  std::string account_;
};

} // namespace

int main() {
  namespace v1 = memex::protocol::v1;

  // —— 协议级：受理→归档留痕＋在线即投＋受理回执；限频／群目标／
  //    未登录／离线不补投 ——
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
    a.login("alice", "pc-a");

    {
      TestClient b(io, server.port());
      b.login("bob", "pc-b");
      // 登录推送（PRESENCE_DATA 等）清空到静止
      a.sync();
      b.sync();

      // alice→bob 振屏：bob 收到空体 NUDGE（msg_id 服务端分配），alice 收受理回执
      a.nudge(3, "bob");
      {
        const auto r = a.read();
        CHECK(r.type() == v1::ACK);
        CHECK(r.seq() == 3);
      }
      const std::string mid = [&] {
        const auto r = b.read();
        CHECK(r.type() == v1::NUDGE);
        CHECK(r.has_nudge());
        CHECK(r.from() == "alice");
        CHECK(r.to() == "bob");
        return r.msg_id();
      }();
      CHECK(!mid.empty());
      // 归档留痕：type=NUDGE（53）、标记文本、双方正确
      {
        const auto rows = s.search_messages({"alice", "振屏", 0, 0, 50});
        CHECK(rows.size() == 1);
        if (rows.size() == 1) {
          CHECK(rows[0].msg_id == mid);
          CHECK(rows[0].type == static_cast<int>(v1::NUDGE));
          CHECK(rows[0].text == "[振屏]");
          CHECK(rows[0].from_account == "alice");
          CHECK(rows[0].to_account == "bob");
        }
        CHECK(s.message_from(mid) == "alice");
      }

      // 连接级限频：1s 内第二条静默拒——无受理回执、对端无投递、不归档
      a.nudge(4, "bob");
      a.expect_pong_only();
      b.expect_pong_only();
      CHECK(s.search_messages({"alice", "振屏", 0, 0, 50}).size() == 1);

      // 群目标拒：无受理回执、无投递、不归档
      a.nudge(5, "group:1");
      a.expect_pong_only();
      b.expect_pong_only();
      CHECK(s.search_messages({"alice", "振屏", 0, 0, 50}).size() == 1);
    } // b 断开（服务端除名并广播 PRESENCE_DATA 给 alice）

    // 未登录拒：新连接直接发振屏——无受理回执、不归档
    {
      TestClient c(io, server.port());
      c.nudge(9, "bob");
      c.expect_pong_only();
      CHECK(s.search_messages({"alice", "振屏", 0, 0, 50}).size() == 1);
    }

    // 离线不补投：静置过限频窗＋排空 bob 断开广播，再向离线 bob 振屏——
    // 归档留痕照常（受理回执照发）但零离线队列；bob 重登后无补投帧
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    a.sync(); // 排空 bob 断开的 PRESENCE_DATA 广播
    a.nudge(6, "bob");
    {
      const auto r = a.read();
      CHECK(r.type() == v1::ACK);
      CHECK(r.seq() == 6);
    }
    CHECK(s.search_messages({"alice", "振屏", 0, 0, 50}).size() == 2);
    CHECK(s.offline_count("bob") == 0); // 振屏不进离线补投
    {
      TestClient b2(io, server.port());
      b2.login("bob", "pc-b2");
      b2.expect_pong_only(); // 重登后无补投帧（否证）
    }

    // 收尾序＝先停 io 线程再关库：io_thread 活着时 s.close() 会与断连链
    // 的 unregister_online（store_.add_presence_event）并发 sqlite3_close
    // ——sqlite 连接被使用中不可关闭（负载下 io_thread 被抢占即 SEGFAULT）。
    // 与 test_read_presence/test_group 同序。
    io.stop();
    io_thread.join();
    s.close();
  }

  if (g_failures == 0) {
    std::cout << "test_nudge: all checks passed\n";
    return 0;
  }
  std::cerr << "test_nudge: " << g_failures << " failure(s)\n";
  return 1;
}
