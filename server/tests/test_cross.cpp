// T4.2 跨态互通验收：跨态会话日志（时间/双方/时长，无内容）与归档起点
//（A8：首条归档消息之前最近一次成功登录时刻）。
// 库级（ServerStore 直测：闭环幂等／三分支归档起点）＋协议级（真实连接：
// 未登录上报被拒、登录归档起点=登录时刻、start/end 上报闭环）。
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

// 阻塞式协议客户端（与 test_group 同款）
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
  // 同步点：PING/PONG 往返后，服务端已处理完此前发来的全部帧
  void sync() {
    memex::protocol::Message ping;
    ping.set_type(memex::protocol::v1::PING);
    ping.set_from(account_);
    ping.set_to("server");
    ping.set_ts_ms(now_ms());
    send(ping);
    memex::protocol::Message r = read();
    CHECK(r.type() == memex::protocol::v1::PONG);
  }
  void login(const std::string& account, const std::string& fp_seed) {
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
    CHECK(read().login_result().ok());
  }
  // 跨态会话日志上报（start/end）；服务端不回帧，用 sync() 确认处理完
  void cross_log(const std::string& op, const std::string& peer_device,
                 const std::string& peer_name, std::int64_t started_ms,
                 std::int64_t ended_ms = 0) {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::CROSS_LOG);
    m.set_from(account_);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    auto* c = m.mutable_cross_log();
    c->set_op(op);
    c->set_peer_device(peer_device);
    c->set_peer_name(peer_name);
    c->set_started_ms(started_ms);
    if (ended_ms > 0) c->set_ended_ms(ended_ms);
    send(m);
  }
  std::string account_;

private:
  std::unique_ptr<asio::ip::tcp::socket> socket_;
};

} // namespace

int main() {
  namespace v1 = memex::protocol::v1;

  // —— 库级：跨态日志闭环（幂等／拒绝条件）＋归档起点三分支 ——
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    for (const char* acct : {"alice", "bob", "carol"}) {
      CHECK(s.create_account(acct, "pw", acct));
    }

    // start：同 (账号, 设备, 建立时刻) 重复上报只记首条（幂等）
    CHECK(s.cross_log_start("alice", "dev-B2", "Laptop-B", 1000));
    CHECK(s.cross_log_start("alice", "dev-B2", "Laptop-B", 1000));
    CHECK(s.cross_log_start("", "dev-B2", "x", 1000) == false); // 账号空
    CHECK(s.cross_log_start("alice", "", "x", 1000) == false);  // 设备空
    auto rows = s.cross_logs();
    CHECK(rows.size() == 1);
    CHECK(rows[0].account == "alice");
    CHECK(rows[0].peer_device == "dev-B2");
    CHECK(rows[0].peer_name == "Laptop-B");
    CHECK(rows[0].started_ms == 1000);
    CHECK(rows[0].ended_ms == 0); // 进行中：无时长
    CHECK(rows[0].duration_ms == 0);

    // end：结束时刻须晚于建立时刻；错配设备/时刻不闭环
    CHECK(s.cross_log_end("alice", "dev-B2", 1000, 1000) == false); // <=started
    CHECK(s.cross_log_end("alice", "dev-B9", 1000, 6000) == false); // 设备错
    CHECK(s.cross_log_end("alice", "dev-B2", 999, 6000) == false);  // 时刻错
    CHECK(s.cross_log_end("alice", "dev-B2", 1000, 6000));
    CHECK(s.cross_log_end("alice", "dev-B2", 1000, 6000) == false); // 已闭环
    rows = s.cross_logs();
    CHECK(rows.size() == 1);
    CHECK(rows[0].ended_ms == 6000);
    CHECK(rows[0].duration_ms == 5000); // 时长 = 结束 − 建立

    // 同设备第二段会话独立成行（倒序：新在前）
    CHECK(s.cross_log_start("alice", "dev-B2", "Laptop-B", 7000));
    CHECK(s.cross_log_end("alice", "dev-B2", 7000, 8500));
    rows = s.cross_logs();
    CHECK(rows.size() == 2);
    CHECK(rows[0].started_ms == 7000 && rows[0].duration_ms == 1500);

    // 归档起点（A8）三分支：
    // ① 无归档 → 0
    CHECK(s.archive_start_ms("alice") == 0);
    CHECK(s.archive_start_ms("ghost") == 0);

    // ② 有归档＋有登录 → 最近一次成功登录（早于首条归档），不是首条消息时刻
    CHECK(s.add_login_record([] {
      memex::server::LoginRecord r;
      r.account = "alice";
      r.fingerprint = "fp-a";
      r.kind = "desktop";
      r.name = "pc-a";
      r.result = "ok";
      r.ts_ms = 2000;
      return r;
    }()));
    CHECK(s.add_login_record([] { // 失败登录不计
      memex::server::LoginRecord r;
      r.account = "alice";
      r.result = "bad_password";
      r.ts_ms = 2500;
      return r;
    }()));
    CHECK(s.store_message("m1", "alice", "bob", 1, "第一条归档", 5000));
    CHECK(s.add_login_record([] { // 首条归档之后的登录不计（更早的才算）
      memex::server::LoginRecord r;
      r.account = "alice";
      r.result = "ok";
      r.ts_ms = 9000;
      return r;
    }()));
    CHECK(s.archive_start_ms("alice") == 2000);

    // ③ 有归档但无登录记录（旧库数据）→ 退化为首条（收或发）归档消息时刻
    //    bob 收发两侧都算归档：m1（alice→bob，5000）早于 bob 自发的 m2
    CHECK(s.store_message("m2", "bob", "alice", 1, "bob 的第一条", 5500));
    CHECK(s.archive_start_ms("bob") == 5000);

    // 群消息联入归档起点（仅群内发言的账号：无单聊归档也应命中）
    const auto gid = s.create_group("跨态群", "alice", {"carol"});
    CHECK(gid > 0);
    CHECK(s.add_login_record([] {
      memex::server::LoginRecord r;
      r.account = "carol";
      r.result = "ok";
      r.ts_ms = 3000;
      return r;
    }()));
    CHECK(s.store_message("m3", "alice",
                          "group:" + std::to_string(gid), 1, "群消息", 4500));
    CHECK(s.archive_start_ms("carol") == 3000);
    CHECK(s.archive_start_ms("ghost") == 0); // 仍无归档 → 0

    s.close();
  }

  // —— 协议级：未登录上报被拒；登录→归档起点=登录时刻；start/end 闭环 ——
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
    a.account_ = "alice";

    // 未登录上报跨态日志：无通道（不落库）——登录后 sync 再查
    a.cross_log("start", "dev-XX", "Ghost", 1111);
    a.login("alice", "pc-a");
    a.sync();
    CHECK(s.cross_logs().empty());

    // 登录时刻（服务端受理时落 login_records）＝归档起点（A8 核心：
    // 归档起点与实际登录时间一致，而非首条消息时刻）
    const auto logins = s.login_records("alice");
    CHECK(logins.size() == 1 && logins[0].result == "ok");
    const std::int64_t login_ms = logins[0].ts_ms;
    CHECK(login_ms > 0);

    // 登录后发第一条归档消息（bob 未登录 → 归档一次＋离线队列一行）
    memex::protocol::Message t;
    t.set_type(v1::TEXT);
    t.set_seq(7);
    t.set_from("alice");
    t.set_to("bob");
    t.set_ts_ms(now_ms());
    t.mutable_text()->set_text("转协作态后的第一条");
    a.send(t);
    const auto ack = a.read();
    CHECK(ack.type() == v1::ACK && ack.seq() == 7);
    CHECK(s.messages("alice").size() == 1);
    CHECK(s.archive_start_ms("alice") == login_ms); // A8：== 登录时刻
    CHECK(s.offline_count("bob") == 1);

    // 跨态会话日志：start（时间/双方）→ 进行中 → end → 时长闭环。
    // 载荷无内容字段——此处上报的只有时间/双方（留痕纪律）。
    const std::int64_t started = login_ms + 1000;
    a.cross_log("start", "dev-B2", "Laptop-B", started);
    a.sync();
    auto rows = s.cross_logs();
    CHECK(rows.size() == 1);
    CHECK(rows[0].account == "alice");
    CHECK(rows[0].peer_device == "dev-B2");
    CHECK(rows[0].peer_name == "Laptop-B");
    CHECK(rows[0].started_ms == started);
    CHECK(rows[0].ended_ms == 0 && rows[0].duration_ms == 0);

    a.cross_log("end", "dev-B2", "Laptop-B", started, started + 5000);
    a.sync();
    rows = s.cross_logs();
    CHECK(rows.size() == 1);
    CHECK(rows[0].ended_ms == started + 5000);
    CHECK(rows[0].duration_ms == 5000);

    // end 幂等：重复闭环无变化
    a.cross_log("end", "dev-B2", "Laptop-B", started, started + 9000);
    a.sync();
    rows = s.cross_logs();
    CHECK(rows.size() == 1);
    CHECK(rows[0].ended_ms == started + 5000);

    io.stop();
    io_thread.join();
    s.close();
  }

  if (g_failures == 0) {
    std::cout << "cross tests: all passed\n";
    return 0;
  }
  std::cerr << "cross tests: " << g_failures << " failure(s)\n";
  return 1;
}
