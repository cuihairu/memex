// BUG-007 验收（设计 docs/design/消息标识与归档去重.md §6 服务端腿）：
// ① V1 重演——同账号重登发同 seq 不同内容：归档两行齐、新行 msg_id ≠
//    sha256(from:seq)、ACK(seq) 照回且带重排新 id；
// ② 补传幂等——同 (from,seq,to,text,ts) 重发：归档仍一行、离线队列不重排、
//    在线重投照发；
// ③ Ack.msg_id 回带腿（常规派生／重排新值／补传原值三态各自断言）；
// ④ 群消息撞 id 腿（群扇出同口径消歧）；
// ⑤ 库错（非撞键 false）受理行为不变腿——DROP messages 后照常投递回执。
// 服务端核心库直链运行（io 线程驱动，不起进程）。
#include <asio.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

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

  // 在线推送（PRESENCE_DATA/FAV_DATA/DELIVER_NOTICE）与本验收无关，自动跳过
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
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA &&
          msg.type() != memex::protocol::v1::FAV_DATA &&
          msg.type() != memex::protocol::v1::DELIVER_NOTICE)
        return msg;
    }
  }

private:
  std::unique_ptr<tcp::socket> socket_;
};

memex::protocol::Message make_login(const std::string& account,
                                    const std::string& password,
                                    const std::string& device_name) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::LOGIN);
  m.set_from(device_name);
  m.set_to("server");
  m.set_ts_ms(now_ms());
  auto* in = m.mutable_login();
  in->set_account(account);
  in->set_password(password);
  in->set_device_fingerprint(memex::server::sha256_hex(device_name));
  in->set_device_kind("desktop");
  in->set_device_name(device_name);
  in->set_client_version("0.1.0-test");
  return m;
}

memex::protocol::Message make_text(const std::string& from,
                                   const std::string& to, std::uint64_t seq,
                                   const std::string& body,
                                   std::int64_t ts_ms) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::TEXT);
  m.set_seq(seq);
  m.set_from(from);
  m.set_to(to);
  m.set_ts_ms(ts_ms);
  m.mutable_text()->set_text(body);
  return m;
}

memex::protocol::Message make_ack(const std::string& from,
                                  const std::string& msg_id) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::ACK);
  m.set_from(from);
  m.set_to("server");
  m.set_ts_ms(now_ms());
  m.mutable_ack()->set_msg_id(msg_id);
  return m;
}

} // namespace

int main() {
  memex::server::ServerStore store;
  CHECK(store.open(":memory:"));
  CHECK(store.create_account("alice", "pa-1", "Alice"));
  CHECK(store.create_account("bob", "pb-1", "Bob"));

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  // —— ① V1 重演：重登后同 seq 新内容 → 两行齐＋换盐重排 ——
  const std::string id1 = memex::server::sha256_hex("alice:1");
  {
    TestClient a1(io, port);
    a1.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a1.read().login_result().ok());
    a1.send(make_text("alice", "bob", 1, "第一版", now_ms())); // bob 离线收队
    const auto ack1 = a1.read();
    CHECK(ack1.type() == memex::protocol::v1::ACK);
    CHECK(ack1.seq() == 1);
    CHECK(ack1.ack().msg_id() == id1); // 常规路径：派生 id 回带（③）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto rows1 = store.messages("bob");
    CHECK(rows1.size() == 1);
    if (!rows1.empty()) CHECK(rows1[0].msg_id == id1);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 服务端收尸 EOF
  {
    TestClient a2(io, port);
    a2.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a2.read().login_result().ok());
    a2.send(make_text("alice", "bob", 1, "第二版", now_ms()));
    const auto ack2 = a2.read();
    CHECK(ack2.type() == memex::protocol::v1::ACK);
    CHECK(ack2.seq() == 1); // ACK(seq) 照回
    CHECK(ack2.ack().msg_id() != id1); // 重排新 id（③）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto rows2 = store.messages("bob");
    CHECK(rows2.size() == 2); // 两行齐：旧档不丢、新档不吞
    bool old_row = false, new_row = false;
    for (const auto& r : rows2) {
      if (r.text == "第一版") {
        old_row = true;
        CHECK(r.msg_id == id1);
      }
      if (r.text == "第二版") {
        new_row = true;
        CHECK(r.msg_id == ack2.ack().msg_id()); // 回带值与归档一致
      }
    }
    CHECK(old_row && new_row);
    CHECK(store.offline_count("bob") == 2); // 两条都进补投队列
  }

  // —— ② 补传幂等：同 (from,seq,to,text,ts) 重发 → 仍一行、不重排队 ——
  const std::string id2 = memex::server::sha256_hex("alice:2");
  const std::int64_t t3 = now_ms();
  {
    TestClient a2b(io, port);
    a2b.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a2b.read().login_result().ok());
    a2b.send(make_text("alice", "bob", 2, "重发这条", t3));
    const auto ack = a2b.read();
    CHECK(ack.ack().msg_id() == id2); // 常规派生 id（③）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(store.messages("bob").size() == 3);
    CHECK(store.offline_count("bob") == 3);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    // 重登补传（桌面 flush_pending_sends 同路径）：同 seq 同内容同 ts 重发
    TestClient a2c(io, port);
    a2c.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a2c.read().login_result().ok());
    a2c.send(make_text("alice", "bob", 2, "重发这条", t3));
    const auto ack_dup = a2c.read();
    CHECK(ack_dup.type() == memex::protocol::v1::ACK);
    CHECK(ack_dup.seq() == 2);
    CHECK(ack_dup.ack().msg_id() == id2); // 补传回带原派生 id（③）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(store.messages("bob").size() == 3); // 归档仍一行
    CHECK(store.offline_count("bob") == 3);   // 离线队列不重排

    // 在线重投照发：bob 上线收满三条补投，逐条 ACK 清队
    TestClient b(io, port);
    b.send(make_login("bob", "pb-1", "pc-bob"));
    CHECK(b.read().login_result().ok());
    for (int i = 0; i < 3; ++i) {
      const auto m = b.read();
      CHECK(m.type() == memex::protocol::v1::TEXT);
      CHECK(!m.msg_id().empty());
      b.send(make_ack("bob", m.msg_id()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(store.offline_count("bob") == 0);
    // a2c 在线再补传同条 → bob 收到重投帧（在线重投照发）、归档不增行
    a2c.send(make_text("alice", "bob", 2, "重发这条", t3));
    const auto dup = b.read();
    CHECK(dup.type() == memex::protocol::v1::TEXT);
    CHECK(dup.text().text() == "重发这条");
    CHECK(dup.msg_id() == id2);
    b.send(make_ack("bob", id2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(store.messages("bob").size() == 3);
    CHECK(store.offline_count("bob") == 0);
    // 消费 dup 送信的受理回执（在线补传幂等 id 三态③之补传面），防止
    // 滞留帧串进群腿的读序
    const auto dup_ack = a2c.read();
    CHECK(dup_ack.type() == memex::protocol::v1::ACK);
    CHECK(dup_ack.seq() == 2);
    CHECK(dup_ack.ack().msg_id() == id2);

    // —— ④ 群消息撞 id 腿：群扇出同口径消歧 ——
    const auto gid = store.create_group("项目群", "alice", {"bob"});
    CHECK(gid > 0);
    const std::string gto = "group:" + std::to_string(gid);
    const std::string g1 = memex::server::sha256_hex("alice:5");
    a2c.send(make_text("alice", gto, 5, "群第一版", now_ms()));
    const auto gack1 = a2c.read();
    CHECK(gack1.ack().msg_id() == g1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto oldg = store.message_by_id(g1);
    CHECK(oldg.has_value());
    if (oldg.has_value()) CHECK(oldg->text == "群第一版");
    a2c.send(make_text("alice", gto, 5, "群第二版", now_ms())); // 同 seq 撞键
    const auto gack2 = a2c.read();
    CHECK(gack2.ack().msg_id() != g1); // 重排新 id（③）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto newg = store.message_by_id(gack2.ack().msg_id());
    CHECK(newg.has_value());
    if (newg.has_value()) {
      CHECK(newg->text == "群第二版");
      CHECK(newg->to_account == gto);
    }
    CHECK(store.message_by_id(g1).has_value()); // 旧群档原样
    CHECK(store.offline_count("bob") == 2);     // 群扇出两条都进补投
  }

  io.stop();
  io_thread.join();

  // —— ⑤ 库错（非撞键 false）受理行为不变腿：DROP messages 后照常
  //     受理投递回执、不崩不吞帧（独立文件库实例，不污染主 store） ——
  {
    const std::string db5 = "/tmp/memex-msg-dedupe-5.db";
    std::remove(db5.c_str());
    ServerStore store5;
    CHECK(store5.open(db5));
    CHECK(store5.create_account("alice", "pa-1", "Alice"));
    CHECK(store5.create_account("bob", "pb-1", "Bob"));
    asio::io_context io5;
    CollabServer server5(io5, store5, 0);
    server5.start_accept();
    std::thread t5([&] { io5.run(); });
    {
      TestClient a(io5, server5.port()), b(io5, server5.port());
      a.send(make_login("alice", "pa-1", "pc-alice"));
      CHECK(a.read().login_result().ok());
      b.send(make_login("bob", "pb-1", "pc-bob"));
      CHECK(b.read().login_result().ok());
      // 外部连接 DROP messages 表：store_message 非撞键失败（prepare 报错），
      // message_by_id 查无此行 → 维持现状照常受理（派生 id 回带）
      sqlite3* raw = nullptr;
      CHECK(sqlite3_open(db5.c_str(), &raw) == SQLITE_OK);
      char* err = nullptr;
      const int rc =
          sqlite3_exec(raw, "DROP TABLE messages;", nullptr, nullptr, &err);
      CHECK(rc == SQLITE_OK);
      sqlite3_free(err);
      sqlite3_close(raw);
      a.send(make_text("alice", "bob", 1, "库错受理", now_ms()));
      const auto got5 = b.read(); // 在线照投
      CHECK(got5.type() == memex::protocol::v1::TEXT);
      CHECK(got5.text().text() == "库错受理");
      CHECK(got5.msg_id() == memex::server::sha256_hex("alice:1"));
      const auto ack5 = a.read();
      CHECK(ack5.type() == memex::protocol::v1::ACK);
      CHECK(ack5.seq() == 1);
      CHECK(ack5.ack().msg_id() == memex::server::sha256_hex("alice:1"));
      CHECK(!store5.message_by_id(memex::server::sha256_hex("alice:1"))
                 .has_value()); // 未落档（库错口径）
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // EOF 收尸
    io5.stop();
    t5.join(); // teardown 口径：join 先于 store5 析构
    std::remove(db5.c_str());
  }

  if (g_failures == 0) {
    std::cout << "msg_dedupe tests: all passed\n";
    return 0;
  }
  std::cerr << "msg_dedupe tests: " << g_failures << " failure(s)\n";
  return 1;
}
