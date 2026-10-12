// 消息与归档一致性故障注入矩阵（评审 P0-2，todo 451）：
// ① 断网重复发送→恢复后重试不重复入档（重发洪峰归档仍一行、队列不重排）；
// ② 服务端重启后补传→归档不重复且补投顺序正确（落盘重开＋队序 id ASC）；
// ③ 重复 ACK 幂等＋未知 msg_id ACK（清队 no-op、伪造不留痕、会话不崩）；
// ④ 撤回与补传交错（补传不洗白撤回标记、归档不重复、队列重入良性）；
// ⑤ DB 写入失败不谎报成功（注入：DROP messages prepare 错／BEGIN EXCLUSIVE
//    瞬态 busy——不回 ACK、不投递、不入队）＋恢复后重试恰一次入档；
// ⑥ 进程崩溃恢复后状态一致（句柄弃置→重开：归档／离线队列／事件台账与
//    投影重建逐字段一致＋协议面补传幂等）。
// 故障注入全在测试层：外部第二 sqlite3 连接（DROP／EXCLUSIVE）与句柄弃置，
// 不改生产路径；「不该来的帧不来」用探活序断言——谎报受理的 ACK/TEXT 会
// 排在同连接后续探活帧的 PONG 之前被 read() 先读到。
#include <asio.hpp>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
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

// 探活帧：负向断言「不该来的帧不来」用（同连接按序处理）
memex::protocol::Message make_ping(const std::string& from) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::PING);
  m.set_from(from);
  m.set_to("server");
  m.set_ts_ms(now_ms());
  return m;
}

memex::protocol::Message make_recall(const std::string& from,
                                     const std::string& to,
                                     const std::string& msg_id) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::RECALL);
  m.set_from(from);
  m.set_to(to);
  m.set_ts_ms(now_ms());
  m.mutable_recall()->set_msg_id(msg_id);
  return m;
}

// 协议面实例（文件库＋CollabServer＋io 线程）：六腿各自独立不互染
struct Rig {
  std::string db;
  ServerStore store;
  asio::io_context io;
  std::unique_ptr<CollabServer> server;
  std::thread io_thread;

  explicit Rig(const std::string& db_path) : db(db_path) {
    if (!store.open(db)) {
      CHECK(false); // 开库失败直接记失败（后续腿全数失真）
    }
    server = std::make_unique<CollabServer>(io, store, 0);
    server->start_accept();
    io_thread = std::thread([&] { io.run(); });
  }
  ~Rig() = default; // teardown 铁律：腿内显式 stop()（join 先于 store 析构）

  std::uint16_t port() const { return server->port(); }
  void stop() {
    io.stop();
    io_thread.join();
  }
};

} // namespace

int main() {
  // —— ① 断网重复发送→恢复后重试不重复入档 ——
  {
    std::remove("/tmp/memex-msg-faults-1.db");
    Rig r("/tmp/memex-msg-faults-1.db");
    CHECK(r.store.create_account("alice", "pa-1", "Alice"));
    CHECK(r.store.create_account("bob", "pb-1", "Bob"));
    const std::string id1 = memex::server::sha256_hex("alice:1");
    const std::int64_t t1 = now_ms();
    TestClient a(r.io, r.port());
    a.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a.read().login_result().ok());
    // bob 离线＝收发链路断：客户端断网期重发洪峰（同帧 3 次）
    for (int i = 0; i < 3; ++i) {
      a.send(make_text("alice", "bob", 1, "断网重发", t1));
      const auto ack = a.read();
      CHECK(ack.type() == memex::protocol::v1::ACK);
      CHECK(ack.seq() == 1);
      CHECK(ack.ack().msg_id() == id1); // 同派生 id：幂等受理
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(r.store.messages("bob").size() == 1); // 重试不重复入档
    CHECK(r.store.offline_count("bob") == 1);   // 队列不重排（OR-IGNORE）
    // 恢复：bob 上线收补投 → ACK 清队
    {
      TestClient b(r.io, r.port());
      b.send(make_login("bob", "pb-1", "pc-bob"));
      CHECK(b.read().login_result().ok());
      const auto got = b.read();
      CHECK(got.type() == memex::protocol::v1::TEXT);
      CHECK(got.msg_id() == id1);
      b.send(make_ack("bob", id1));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r.store.offline_count("bob") == 0);
      // 恢复后重试（发送端 ACK 丢失场景）：真补传回原 id、归档仍一行；
      // 队列重入一行（重投帧同 msg_id，客户端按 id 去重不出重行）
      a.send(make_text("alice", "bob", 1, "断网重发", t1));
      const auto ack2 = a.read();
      CHECK(ack2.ack().msg_id() == id1);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r.store.messages("bob").size() == 1);
      CHECK(r.store.offline_count("bob") == 1);
      const auto got2 = b.read(); // 在线重投照发
      CHECK(got2.type() == memex::protocol::v1::TEXT);
      CHECK(got2.msg_id() == id1);
      b.send(make_ack("bob", id1)); // 二次 ACK 幂等
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r.store.offline_count("bob") == 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // EOF 收尸
    r.stop();
    r.store.close();
    std::remove(r.db.c_str());
  }

  // —— ② 服务端重启后补传→归档不重复且顺序正确 ＋ ③ 重复/未知 ACK 幂等 ——
  {
    const std::string db = "/tmp/memex-msg-faults-2.db";
    std::remove(db.c_str());
    std::vector<std::string> ids;
    for (int i = 1; i <= 3; ++i) {
      ids.push_back(
          memex::server::sha256_hex("alice:" + std::to_string(i)));
    }
    std::int64_t t0 = 0;
    {
      Rig r(db);
      CHECK(r.store.create_account("alice", "pa-1", "Alice"));
      CHECK(r.store.create_account("bob", "pb-1", "Bob"));
      t0 = now_ms();
      TestClient a(r.io, r.port());
      a.send(make_login("alice", "pa-1", "pc-alice"));
      CHECK(a.read().login_result().ok());
      for (int i = 1; i <= 3; ++i) { // bob 离线收队：3 条入档入队
        a.send(make_text("alice", "bob", i,
                         "重启前第" + std::to_string(i) + "条", t0 + i));
        const auto ack = a.read();
        CHECK(ack.ack().msg_id() == ids[i - 1]);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r.store.messages("bob").size() == 3);
      CHECK(r.store.offline_count("bob") == 3);
      std::this_thread::sleep_for(std::chrono::milliseconds(50)); // EOF 收尸
      r.stop();
      r.store.close(); // —— 重启：落盘关闭，库文件保留 ——
    }
    Rig r2(db); // 重开同一文件（ensure_schema 幂等：不重建不重复）
    // 库面：归档与离线队列随库存活
    CHECK(r2.store.messages("bob").size() == 3);
    CHECK(r2.store.offline_count("bob") == 3);
    // 补投顺序＝入队顺序（pending_offline 按 id ASC）
    const auto pend = r2.store.pending_offline("bob");
    CHECK(pend.size() == 3);
    for (int i = 0; i < 3 && i < static_cast<int>(pend.size()); ++i) {
      const auto env = memex::protocol::decode_payload(pend[i]);
      CHECK(env.type() == memex::protocol::v1::TEXT);
      CHECK(env.msg_id() == ids[i]);
    }
    // 协议面补传：重发 seq=2 同帧 → 真补传回原 id，归档/队列不增
    TestClient a2(r2.io, r2.port());
    a2.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a2.read().login_result().ok());
    a2.send(make_text("alice", "bob", 2, "重启前第2条", t0 + 2));
    const auto ack = a2.read();
    CHECK(ack.ack().msg_id() == ids[1]);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(r2.store.messages("bob").size() == 3);
    CHECK(r2.store.offline_count("bob") == 3);
    // bob 上线按队序收满 3 条（顺序正确）→ 逐条 ACK
    {
      TestClient b(r2.io, r2.port());
      b.send(make_login("bob", "pb-1", "pc-bob"));
      CHECK(b.read().login_result().ok());
      for (int i = 0; i < 3; ++i) {
        const auto got = b.read();
        CHECK(got.type() == memex::protocol::v1::TEXT);
        CHECK(got.msg_id() == ids[i]); // 补投顺序正确
        b.send(make_ack("bob", ids[i]));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r2.store.offline_count("bob") == 0);
      // —— ③ 重复 ACK 幂等 ＋ 未知 msg_id ACK ——
      b.send(make_ack("bob", ids[1]));       // 重复 ACK：清队 no-op
      b.send(make_ack("bob", "mid-未知id")); // 未知/伪造：不留痕不崩
      b.send(make_ping("bob"));
      CHECK(b.read().type() == memex::protocol::v1::PONG); // 会话存活
      CHECK(r2.store.offline_count("bob") == 0);
      CHECK(r2.store.messages("bob").size() == 3); // 归档不增
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r2.stop();
    r2.store.close();
    std::remove(db.c_str());
  }

  // —— ④ 撤回与补传交错：补传不洗白撤回标记、归档不重复、队列重入良性 ——
  {
    std::remove("/tmp/memex-msg-faults-4.db");
    Rig r("/tmp/memex-msg-faults-4.db");
    CHECK(r.store.create_account("alice", "pa-1", "Alice"));
    CHECK(r.store.create_account("bob", "pb-1", "Bob"));
    const std::string id4 = memex::server::sha256_hex("alice:1");
    const std::int64_t t4 = now_ms();
    TestClient a(r.io, r.port());
    a.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a.read().login_result().ok());
    a.send(make_text("alice", "bob", 1, "撤回我", t4));
    CHECK(a.read().ack().msg_id() == id4);
    {
      TestClient b(r.io, r.port());
      b.send(make_login("bob", "pb-1", "pc-bob"));
      CHECK(b.read().login_result().ok());
      const auto got = b.read();
      CHECK(got.type() == memex::protocol::v1::TEXT && got.msg_id() == id4);
      b.send(make_ack("bob", id4));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r.store.offline_count("bob") == 0);
      // 撤回：置标记不清正文；在线对端收 RECALL 帧
      a.send(make_recall("alice", "bob", id4));
      const auto rec = b.read();
      CHECK(rec.type() == memex::protocol::v1::RECALL);
      CHECK(rec.msg_id() == id4);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const auto row = r.store.message_by_id(id4);
      CHECK(row.has_value() && row->recalled);
      CHECK(r.store.messages("bob").size() == 1); // 不增行不清正文
      // 补传交错（撤回后 ACK 丢失重发同帧）：真补传回原 id；撤回标记
      // 不洗白；队列重入（重投帧无撤回态——客户端按 msg_id 去重且本地行
      // 已带撤回标记，不出重复不复活正文）
      a.send(make_text("alice", "bob", 1, "撤回我", t4));
      CHECK(a.read().ack().msg_id() == id4); // 原派生 id（同帧真补传）
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r.store.messages("bob").size() == 1);   // 归档不重复
      CHECK(r.store.message_by_id(id4)->recalled);  // 补传不洗白撤回
      CHECK(r.store.offline_count("bob") == 1);     // 队列重入（OR-IGNORE）
      const auto got2 = b.read();
      CHECK(got2.type() == memex::protocol::v1::TEXT && got2.msg_id() == id4);
      b.send(make_ack("bob", id4));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(r.store.offline_count("bob") == 0);
    }
    // 撤回未知目标：拒不留痕不崩（探活序：PONG 前无其他显著帧）
    a.send(make_recall("alice", "bob", "mid-不存在"));
    a.send(make_ping("alice"));
    CHECK(a.read().type() == memex::protocol::v1::PONG);
    CHECK(r.store.messages("bob").size() == 1); // 未知撤回不落痕
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.stop();
    r.store.close();
    std::remove(r.db.c_str());
  }

  // —— ⑤ DB 写入失败不谎报成功（丢表＋瞬态锁两种注入）＋恢复后重试恰一次 ——
  {
    const std::string db = "/tmp/memex-msg-faults-5.db";
    std::remove(db.c_str());
    {
      Rig r(db);
      CHECK(r.store.create_account("alice", "pa-1", "Alice"));
      CHECK(r.store.create_account("bob", "pb-1", "Bob"));
      TestClient a(r.io, r.port()), b(r.io, r.port());
      a.send(make_login("alice", "pa-1", "pc-alice"));
      CHECK(a.read().login_result().ok());
      b.send(make_login("bob", "pb-1", "pc-bob"));
      CHECK(b.read().login_result().ok());
      const std::int64_t t5 = now_ms();
      // 注入一：外部连接 DROP messages——prepare 级永久错
      sqlite3* raw = nullptr;
      CHECK(sqlite3_open(db.c_str(), &raw) == SQLITE_OK);
      char* err = nullptr;
      CHECK(sqlite3_exec(raw, "DROP TABLE messages;", nullptr, nullptr,
                         &err) == SQLITE_OK);
      sqlite3_free(err);
      a.send(make_text("alice", "bob", 30, "丢表不谎报", t5));
      a.send(make_ping("alice"));
      CHECK(a.read().type() == memex::protocol::v1::PONG); // 无 ACK（谎报则先到）
      b.send(make_ping("bob"));
      CHECK(b.read().type() == memex::protocol::v1::PONG); // 无 TEXT
      CHECK(r.store.offline_count("bob") == 0);            // 不入队
      CHECK(!r.store.message_by_id(memex::server::sha256_hex("alice:30"))
                 .has_value()); // 未落档
      // 注入二：外部连接 BEGIN EXCLUSIVE 占库——busy_timeout(5s) 后写失败
      err = nullptr;
      CHECK(sqlite3_exec(raw, "BEGIN EXCLUSIVE;", nullptr, nullptr,
                         &err) == SQLITE_OK);
      sqlite3_free(err);
      a.send(make_text("alice", "bob", 31, "锁库不谎报", t5 + 1));
      a.send(make_ping("alice"));
      CHECK(a.read().type() == memex::protocol::v1::PONG); // ~10s 后无 ACK
      CHECK(r.store.offline_count("bob") == 0);            // 两条都没入队
      err = nullptr;
      CHECK(sqlite3_exec(raw, "ROLLBACK;", nullptr, nullptr, &err) ==
            SQLITE_OK);
      sqlite3_free(err);
      sqlite3_close(raw);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      r.stop();
      r.store.close();
    }
    // 恢复：重开库（ensure_schema 重建空 messages 表）→ 重试恰一次入档
    Rig r2(db);
    CHECK(r2.store.messages("bob").empty()); // 丢表期消息确未归档（无谎报残留）
    TestClient a2(r2.io, r2.port()), b2(r2.io, r2.port());
    a2.send(make_login("alice", "pa-1", "pc-alice"));
    CHECK(a2.read().login_result().ok());
    b2.send(make_login("bob", "pb-1", "pc-bob"));
    CHECK(b2.read().login_result().ok());
    const std::int64_t t6 = now_ms();
    const std::string id30 = memex::server::sha256_hex("alice:30");
    const std::string id31 = memex::server::sha256_hex("alice:31");
    a2.send(make_text("alice", "bob", 30, "丢表不谎报", t6));
    CHECK(a2.read().ack().msg_id() == id30);
    a2.send(make_text("alice", "bob", 31, "锁库不谎报", t6 + 1));
    CHECK(a2.read().ack().msg_id() == id31);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(r2.store.messages("bob").size() == 2);
    CHECK(r2.store.offline_count("bob") == 2);
    // 重发洪峰（恢复后重试）：幂等不重复
    a2.send(make_text("alice", "bob", 30, "丢表不谎报", t6));
    CHECK(a2.read().ack().msg_id() == id30);
    a2.send(make_text("alice", "bob", 31, "锁库不谎报", t6 + 1));
    CHECK(a2.read().ack().msg_id() == id31);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(r2.store.messages("bob").size() == 2); // 重试不重复入档
    CHECK(r2.store.offline_count("bob") == 2);
    for (int i = 0; i < 4; ++i) { // 登录补投 2 ＋ 在线重投 2；后 2 次 ACK 幂等
      const auto got = b2.read();
      CHECK(got.type() == memex::protocol::v1::TEXT);
      b2.send(make_ack("bob", got.msg_id()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(r2.store.offline_count("bob") == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r2.stop();
    r2.store.close();
    std::remove(db.c_str());
  }

  // —— ⑥ 进程崩溃恢复后状态一致（句柄弃置→重开：归档/队列/台账一致） ——
  {
    const std::string db = "/tmp/memex-msg-faults-6.db";
    std::remove(db.c_str());
    std::string idc1, idc2;
    std::int64_t tc = 0;
    {
      // 堆上持库：崩溃腿不 close 不 delete（句柄随进程弃置，等价 kill -9
      // 后的磁盘态——SQLite 语句级提交，已受理消息必然已落盘）
      auto* store = new ServerStore();
      CHECK(store->open(db));
      CHECK(store->create_account("alice", "pa-1", "Alice"));
      CHECK(store->create_account("bob", "pb-1", "Bob"));
      asio::io_context io;
      CollabServer server(io, *store, 0);
      server.start_accept();
      std::thread t([&] { io.run(); });
      tc = now_ms();
      idc1 = memex::server::sha256_hex("alice:1");
      idc2 = memex::server::sha256_hex("alice:2");
      TestClient a(io, server.port());
      a.send(make_login("alice", "pa-1", "pc-alice"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", "bob", 1, "崩溃前一", tc + 1));
      CHECK(a.read().ack().msg_id() == idc1);
      a.send(make_text("alice", "bob", 2, "崩溃前二", tc + 2));
      CHECK(a.read().ack().msg_id() == idc2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(store->messages("bob").size() == 2);
      CHECK(store->offline_count("bob") == 2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      io.stop();
      t.join(); // 会话线程先停（不再有库操作在途）；store 句柄弃置不 close
      // 故意不 delete store：崩溃语义＝磁盘态停在最后一条已提交语句之后
    }
    // 恢复：重开同一文件 → 归档/队列/台账三面一致
    ServerStore rec;
    CHECK(rec.open(db));
    const auto rows = rec.messages("bob");
    CHECK(rows.size() == 2);
    bool saw1 = false, saw2 = false;
    for (const auto& m : rows) {
      if (m.msg_id == idc1) {
        saw1 = true;
        CHECK(m.text == "崩溃前一");
        CHECK(m.from_account == "alice");
        CHECK(m.to_account == "bob");
        CHECK(m.type == static_cast<int>(memex::protocol::v1::TEXT));
      }
      if (m.msg_id == idc2) {
        saw2 = true;
        CHECK(m.text == "崩溃前二");
        CHECK(m.ts_ms == tc + 2);
      }
    }
    CHECK(saw1 && saw2);
    const auto pend = rec.pending_offline("bob");
    CHECK(pend.size() == 2);
    if (pend.size() == 2) {
      CHECK(memex::protocol::decode_payload(pend[0]).msg_id() == idc1);
      CHECK(memex::protocol::decode_payload(pend[1]).msg_id() == idc2);
    }
    // 台账一致：事件重放重建的投影与物化态逐字段一致
    const auto evs = rec.message_events("");
    CHECK(evs.size() >= 2);
    ServerStore replay;
    CHECK(replay.open(":memory:"));
    for (const auto& e : evs) {
      CHECK(replay.append_message_event(e.msg_id, e.event, e.by_account,
                                        e.payload, e.ts_ms));
    }
    const auto reborn = replay.rebuild_messages_from_events();
    const auto live = rec.messages("");
    CHECK(reborn.size() == live.size());
    for (const auto& m : live) {
      bool found = false;
      for (const auto& rb : reborn) {
        if (rb.msg_id != m.msg_id) continue;
        found = true;
        CHECK(rb.from_account == m.from_account);
        CHECK(rb.to_account == m.to_account);
        CHECK(rb.type == m.type);
        CHECK(rb.text == m.text);
        CHECK(rb.ts_ms == m.ts_ms);
        CHECK(rb.recalled == m.recalled);
        break;
      }
      CHECK(found); // 原库每条都能在重建态找到同字段副本
    }
    // 协议面恢复：补传 seq=2 真补传回原 id；bob 上线按队序收 2 条
    asio::io_context io2;
    CollabServer server2(io2, rec, 0);
    server2.start_accept();
    std::thread t2([&] { io2.run(); });
    {
      TestClient a2(io2, server2.port());
      a2.send(make_login("alice", "pa-1", "pc-alice"));
      CHECK(a2.read().login_result().ok());
      a2.send(make_text("alice", "bob", 2, "崩溃前二", tc + 2));
      const auto ack = a2.read();
      CHECK(ack.ack().msg_id() == idc2); // 真补传原 id
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(rec.messages("bob").size() == 2); // 归档不重复
      CHECK(rec.offline_count("bob") == 2);
      TestClient b2(io2, server2.port());
      b2.send(make_login("bob", "pb-1", "pc-bob"));
      CHECK(b2.read().login_result().ok());
      for (int i = 0; i < 2; ++i) { // 队序补投
        const auto got = b2.read();
        CHECK(got.type() == memex::protocol::v1::TEXT);
        CHECK(got.msg_id() == (i == 0 ? idc1 : idc2));
        b2.send(make_ack("bob", got.msg_id()));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK(rec.offline_count("bob") == 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    io2.stop();
    t2.join();
    rec.close();
    std::remove(db.c_str());
  }

  if (g_failures == 0) {
    std::cout << "msg_faults tests: all passed\n";
    return 0;
  }
  std::cerr << "msg_faults tests: " << g_failures << " failure(s)\n";
  return 1;
}
