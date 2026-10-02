// T2.3 服务端半边验收：协作态消息全量落归档库、撤回仅置标记不清正文、
// 撤回事件独立留痕、越权撤回拒绝、接收端收到 RECALL 转发帧；
// 本地缓存（客户端 SQLite）与服务端归档分离：任何一侧删除不影响另一侧。
// T3.2 检索与导出：按人／时间窗／关键词检索（撤回原文照常可查）、
// 导出留证、检索／导出逐次落查阅日志（audit）。
// 服务端核心库直链运行（io 线程驱动，不起进程）＋CLI 进程级验证。
#include <asio.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
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
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA) return msg;
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

memex::protocol::Message make_ack(const std::string& msg_id) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::ACK);
  m.set_from("receiver");
  m.set_to("server");
  m.mutable_ack()->set_msg_id(msg_id);
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

  TestClient a(io, port), b(io, port);
  a.send(make_login("alice", "pa-1", "pc-alice"));
  CHECK(a.read().login_result().ok());
  b.send(make_login("bob", "pb-1", "pc-bob"));
  CHECK(b.read().login_result().ok());

  // 在线互发：归档落库 + 离线队列入队 + 在线投递
  a.send(make_text("alice", "bob", 1, "可留痕的这一条"));
  const auto got = b.read();
  CHECK(got.type() == memex::protocol::v1::TEXT);
  CHECK(got.text().text() == "可留痕的这一条");
  const auto receipt = a.read();
  CHECK(receipt.type() == memex::protocol::v1::ACK);
  const std::string msg_id = memex::server::sha256_hex("alice:1");
  CHECK(got.msg_id() == msg_id);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto rows = store.messages("bob");
  CHECK(rows.size() == 1); // 全量落库
  if (!rows.empty()) {
    const auto& r = rows[0];
    CHECK(r.msg_id == msg_id);              // msg_id
    CHECK(r.from_account == "alice");       // from
    CHECK(r.text == "可留痕的这一条");       // text 原样保留
    CHECK(r.type == 10);                    // type TEXT
    CHECK(!r.recalled);                     // 初始未撤回
  }
  CHECK(store.offline_count("bob") == 1); // 等接收方 ACK

  b.send(make_ack(msg_id));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(store.offline_count("bob") == 0);

  // 越权撤回：bob 试图撤回 alice 的消息 → 拒绝、归档不动
  b.send(make_recall("bob", "alice", msg_id));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(!store.is_recalled(msg_id));
  CHECK(store.recall_event_count(msg_id) == 0);

  // 合法撤回：alice 撤回自己的消息 → 仅置标记，原文仍在，事件独立留痕
  a.send(make_recall("alice", "bob", msg_id));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(store.is_recalled(msg_id));
  CHECK(store.recall_event_count(msg_id) == 1);
  const auto rows2 = store.messages("bob");
  CHECK(rows2.size() == 1);
  if (!rows2.empty()) {
    CHECK(rows2[0].text == "可留痕的这一条"); // 撤回不清正文
    CHECK(rows2[0].recalled);                // 标记可见（检索面）
  }
  // 接收方在线会话收到 RECALL 转发帧（本地副本置标记用）
  const auto recall_frame = b.read();
  CHECK(recall_frame.type() == memex::protocol::v1::RECALL);
  CHECK(recall_frame.recall().msg_id() == msg_id);

  // —— T3.2 条件检索（库级）：按人／时间窗／关键词，AND 组合 ——
  store.create_account("carol", "pc-1", "Carol");
  store.create_account("dave", "pd-1", "Dave");
  constexpr std::int64_t kHourMs = 3600 * 1000;
  const std::int64_t t0 = now_ms();
  CHECK(store.store_message("mid-2", "bob", "alice", 10, "发票已开", t0 - 2 * kHourMs));
  CHECK(store.store_message("mid-3", "carol", "dave", 10, "周末团建报名", t0 - kHourMs));
  CHECK(store.store_message("mid-4", "alice", "carol", 10, "合同扫描件已发", t0 - kHourMs / 2));
  CHECK(store.store_message("mid-5", "bob", "carol", 10, "进度 100% 了", t0 - kHourMs / 4));

  { // 关键词：命中一条
    memex::server::MessageSearch q;
    q.keyword = "合同";
    const auto hits = store.search_messages(q);
    CHECK(hits.size() == 1);
    if (!hits.empty()) CHECK(hits[0].msg_id == "mid-4");
  }
  { // 撤回消息照常命中：原文保留、标记可见
    memex::server::MessageSearch q;
    q.keyword = "留痕";
    const auto hits = store.search_messages(q);
    CHECK(hits.size() == 1);
    if (!hits.empty()) {
      CHECK(hits[0].recalled);
      CHECK(hits[0].text == "可留痕的这一条");
    }
  }
  { // 按人：收发双侧都算（alice 参与 3 条）
    memex::server::MessageSearch q;
    q.account = "alice";
    CHECK(store.search_messages(q).size() == 3);
  }
  { // 时间窗（含端点）：只取窗内两条
    memex::server::MessageSearch q;
    q.since_ms = t0 - 90 * 60 * 1000;
    q.until_ms = t0 - 20 * 60 * 1000;
    const auto hits = store.search_messages(q);
    CHECK(hits.size() == 2);
    if (hits.size() == 2) {
      CHECK(hits[1].msg_id == "mid-3" || hits[0].msg_id == "mid-4");
    }
  }
  { // 组合：人＋关键词交集
    memex::server::MessageSearch q;
    q.account = "bob";
    q.keyword = "发票";
    CHECK(store.search_messages(q).size() == 1);
  }
  { // LIKE 元字符按字面匹配：100% 命中、下划线不当通配符
    memex::server::MessageSearch q;
    q.keyword = "100%";
    CHECK(store.search_messages(q).size() == 1);
    q.keyword = "合同_";
    CHECK(store.search_messages(q).empty());
  }

  io.stop();
  io_thread.join();

  // —— T3.2 CLI 级：检索／导出留证／查阅日志 ——
  const auto run_cli = [](const std::string& args) {
    const std::string out_path = "/tmp/memex-archive-test-out.txt";
    const std::string cmd = std::string("\"" MEMEX_SERVER_BIN "\" ") + args +
                            " > " + out_path + " 2>&1";
    const int rc = std::system(cmd.c_str());
    (void)rc;
    std::ifstream f(out_path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const auto read_file = [](const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };

  const std::string db2 = "/tmp/memex-archive-cli.db";
  const std::string export_path = "/tmp/memex-archive-export.txt";
  std::remove(db2.c_str());
  std::remove(export_path.c_str());
  {
    ServerStore seed;
    CHECK(seed.open(db2));
    CHECK(seed.create_account("alice", "pa-1", "Alice"));
    CHECK(seed.create_account("bob", "pb-1", "Bob"));
    CHECK(seed.store_message("mid-x1", "alice", "bob", 10, "合同评审通过", now_ms() - 60000));
    CHECK(seed.store_message("mid-x2", "alice", "bob", 10, "明天放假", now_ms() - 30000));
    CHECK(seed.recall_message("mid-x1")); // 撤回的那条含关键词：导出面仍须完整
    seed.close();
  }
  { // 关键词检索＋导出留证
    const std::string out = run_cli("messages --keyword 合同 --export " + export_path +
                                    " --db " + db2);
    CHECK(out.find("共 1 条") != std::string::npos);
    CHECK(out.find("已导出至：" + export_path) != std::string::npos);
    const std::string file = read_file(export_path);
    CHECK(file.find("合同评审通过") != std::string::npos); // 原文在导出面
    CHECK(file.find("已撤回") != std::string::npos);        // 撤回标记可见
    CHECK(file.find("明天放假") == std::string::npos);      // 未命中不进导出
    CHECK(file.find("# 过滤条件：关键词=合同") != std::string::npos);
  }
  { // 按人＋时间：日期粒度端点语义
    const std::string out =
        run_cli("messages alice --since 2000-01-01 --until 2999-01-01 --db " + db2);
    CHECK(out.find("共 2 条") != std::string::npos);
  }
  { // 查阅日志：检索与导出逐次落痕、条件与命中数可对账
    const std::string out = run_cli("audit 10 --db " + db2);
    CHECK(out.find("导出") != std::string::npos);
    CHECK(out.find("关键词=合同") != std::string::npos);
    CHECK(out.find("检索") != std::string::npos);
    CHECK(out.find("账号=alice") != std::string::npos);
  }

  if (g_failures == 0) {
    std::cout << "archive tests: all passed\n";
    return 0;
  }
  std::cerr << "archive tests: " << g_failures << " failure(s)\n";
  return 1;
}
