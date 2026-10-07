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
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA && msg.type() != memex::protocol::v1::FAV_DATA) return msg;
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
    CHECK(seed.recall_message("mid-x1", "alice", now_ms())); // 撤回的那条含关键词：导出面仍须完整
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
    CHECK(out.find("audit.message.export") != std::string::npos);
    CHECK(out.find("关键词=合同") != std::string::npos);
    CHECK(out.find("audit.message.search") != std::string::npos);
    CHECK(out.find("账号=alice") != std::string::npos);
  }

  // —— 平台-4 归档事件溯源：created/delivered/recalled 全留痕；
  //     事件重放重建当前态（重投演示）；edited 投影跟随、原文永在 ——
  {
    // 协议腿已在前面走过：TEXT 归档（created）→ bob ACK（delivered）→
    // alice 撤回（recalled＋对账行）。此处对事件序列本身断言。
    const auto seq = store.message_events(msg_id);
    CHECK(seq.size() == 3);
    if (seq.size() == 3) {
      CHECK(seq[0].event == "created" && seq[0].by_account == "alice");
      CHECK(seq[1].event == "delivered" && seq[1].by_account == "bob");
      CHECK(seq[2].event == "recalled" && seq[2].by_account == "alice");
      // created payload 带全量字段（原文、收发、时间）——重放材料
      CHECK(seq[0].payload.find("可留痕的这一条") != std::string::npos);
      CHECK(seq[0].payload.find("\"to\":\"bob\"") != std::string::npos);
    }
    CHECK(store.message_events("mid-不存在").empty());

    // 重投演练：空库只吃事件流 → 重建态与原库物化态逐字段一致
    ServerStore replay;
    CHECK(replay.open(":memory:"));
    const auto all = store.message_events("");
    CHECK(!all.empty());
    for (const auto& e : all) {
      CHECK(replay.append_message_event(e.msg_id, e.event, e.by_account,
                                        e.payload, e.ts_ms));
    }
    const auto reborn = replay.rebuild_messages_from_events();
    const auto live = store.messages("");
    CHECK(reborn.size() == live.size());
    for (const auto& m : live) {
      bool found = false;
      for (const auto& r : reborn) {
        if (r.msg_id != m.msg_id) continue;
        found = true;
        CHECK(r.from_account == m.from_account);
        CHECK(r.to_account == m.to_account);
        CHECK(r.type == m.type);
        CHECK(r.text == m.text);
        CHECK(r.ts_ms == m.ts_ms);
        CHECK(r.recalled == m.recalled);
        break;
      }
      CHECK(found); // 原库每条都能在重建态找到同字段副本
    }
    // created 幂等：重投同一条 created → 拒（已有行不重放、不记重复事件）
    const auto first = store.message_events("mid-2");
    CHECK(first.size() == 1 && first.front().event == "created");
    CHECK(!replay.append_message_event("mid-2", "created", first.front().by_account,
                                       first.front().payload, 1));

    // edited：投影跟随事件、原 created payload 里原文永在；重放一致
    CHECK(store.store_message("mid-evt", "alice", "bob", 10, "初稿",
                              now_ms()));
    CHECK(store.append_message_event("mid-evt", "edited", "alice", "改定稿",
                                     now_ms()));
    {
      const auto rows = store.messages("");
      bool seen = false;
      for (const auto& m : rows) {
        if (m.msg_id != "mid-evt") continue;
        seen = true;
        CHECK(m.text == "改定稿"); // 投影已替换
      }
      CHECK(seen);
      const auto seq2 = store.message_events("mid-evt");
      CHECK(seq2.size() == 2);
      if (seq2.size() == 2) {
        CHECK(seq2[0].event == "created");
        CHECK(seq2[0].payload.find("初稿") != std::string::npos); // 原文在案
        CHECK(seq2[1].event == "edited" && seq2[1].payload == "改定稿");
      }
    }
    {
      const auto rebuilt = store.rebuild_messages_from_events();
      bool seen = false;
      for (const auto& m : rebuilt) {
        if (m.msg_id != "mid-evt") continue;
        seen = true;
        CHECK(m.text == "改定稿"); // 重放与物化同态
      }
      CHECK(seen);
    }
    // 幽灵消息：recalled/edited 对不存在消息拒（投影 0 行=拒）
    CHECK(!store.append_message_event("mid-幽灵", "recalled", "alice", "", 1));
    CHECK(!store.append_message_event("mid-幽灵", "edited", "alice", "x", 1));
  }

  // —— 平台-5 留存策略与 Retention Purge：白名单天数、部门链继承、
  //     双人审批（一人不得自批自清）、清除落 purged 事件＋台账＋审计、
  //     重建跳过已清除（清除可重现而非无痕） ——
  {
    // 策略面：白名单外拒（45 天不在 30/180/365/1095/indefinite）
    CHECK(!store.retention_set(45, "", "admin1", 1));
    CHECK(store.retention_set(0, "", "admin1", 1)); // 全局 Indefinite
    CHECK(store.retention_set(30, "", "admin1", 2)); // 全局覆盖为 30 天
    const auto g = store.retention_resolve("nobody-挂档");
    CHECK(g.retention_days == 30); // 无部门者吃全局行
    CHECK(store.retention_list().size() == 1);
    // 部门行覆盖全局；须挂已存在部门
    CHECK(!store.retention_set(180, "不存在的部门", "admin1", 3));
    store.ensure_department_path("公司/研发部");
    store.ensure_department_path("公司/研发部/客户端组");
    {
      // bob 挂到 公司/研发部（中段）；客户端组未单独配置 → 链上溯命中父行
      CHECK(store.set_member_profile(
          "bob", store.ensure_department_path("公司/研发部"), "", ""));
      CHECK(store.retention_set(180, "公司/研发部", "admin1", 4));
      const auto r = store.retention_resolve("bob");
      CHECK(r.retention_days == 180 && r.department_path == "公司/研发部");
      // 挂到叶子（叶子未配置）→ 仍走父行 180
      CHECK(store.set_member_profile(
          "bob", store.ensure_department_path("公司/研发部/客户端组"), "", ""));
      const auto r2 = store.retention_resolve("bob");
      CHECK(r2.retention_days == 180 && r2.department_path == "公司/研发部");
    }

    // 清除面：三道闸逐一验拒
    const std::int64_t t_purge = now_ms();
    CHECK(store.store_message("mid-old1", "alice", "bob", 10, "远古一",
                              t_purge - 10 * 86400000LL));
    CHECK(store.store_message("mid-old2", "bob", "alice", 10, "远古二",
                              t_purge - 9 * 86400000LL));
    CHECK(store.store_message("mid-old3", "alice", "bob", 10, "远古三",
                              t_purge - 8 * 86400000LL));
    CHECK(store.retention_purge(0, "理由", "alice", "bob", 30, 1) == 0); // 时间线
    CHECK(store.retention_purge(t_purge, "", "alice", "bob", 30, 1) == 0); // 空理由
    CHECK(store.retention_purge(t_purge, "理由", "alice", "alice", 30, 1) ==
          0); // 自批自清
    CHECK(store.retention_purge(t_purge, "理由", "alice", "幽灵", 30, 1) ==
          0); // 批准人不存在
    const std::size_t before_rows = store.messages("").size();
    const std::int64_t before_events =
        store.message_events("").size();
    const auto ledger_id =
        store.retention_purge(t_purge - 7 * 86400000LL, "留存期到批量清除",
                              "alice", "bob", 30, t_purge); // 线取 7 天前：只圈三条远古
    CHECK(ledger_id > 0);
    CHECK(store.messages("").size() == before_rows - 3); // 三条远古被清
    bool proto_kept = false; // 协议腿消息（ts≈now）不在清除线内，仍在
    for (const auto& m : store.messages(""))
      if (m.msg_id == msg_id) proto_kept = true;
    CHECK(proto_kept);
    // 逐条 purged 事件（created→…→purged 全程留痕）
    for (const char* mid : {"mid-old1", "mid-old2", "mid-old3"}) {
      const auto seq = store.message_events(mid);
      CHECK(!seq.empty());
      if (!seq.empty()) {
        CHECK(seq.front().event == "created"); // 原文 payload 永在
        CHECK(seq.back().event == "purged" && seq.back().by_account == "alice");
        CHECK(seq.back().payload == "留存期到批量清除"); // 理由随事件
      }
    }
    CHECK(store.message_events("").size() == before_events + 3);
    // 台账：who/approval/why/policy/count 全在
    const auto ledgers = store.retention_purges(10);
    CHECK(ledgers.size() == 1);
    if (!ledgers.empty()) {
      CHECK(ledgers[0].id == ledger_id);
      CHECK(ledgers[0].purged_by == "alice" && ledgers[0].approved_by == "bob");
      CHECK(ledgers[0].reason == "留存期到批量清除");
      CHECK(ledgers[0].policy_days == 30 && ledgers[0].msg_count == 3);
      CHECK(ledgers[0].before_ms == t_purge - 7 * 86400000LL);
    }
    // 审计留痕：purge 属敏感动作进查阅台账
    bool audited = false;
    for (const auto& a : store.audit_reads(20))
      if (a.action == "purge" && a.op_account == "alice" &&
          a.result_count == 3)
        audited = true;
    CHECK(audited);
    // 重建跳过已清除（清除可重现而非无痕——重放不出已清消息本体）
    bool ghost_reborn = false;
    for (const auto& m : store.rebuild_messages_from_events())
      if (m.msg_id == "mid-old1" || m.msg_id == "mid-old2" ||
          m.msg_id == "mid-old3")
        ghost_reborn = true;
    CHECK(!ghost_reborn);
  }

  // —— 平台-6 审计独立角色：SecurityAuditor≠SystemAdmin——显式账号化
  //     （--as）须持 auditor 有效角色（admin 不自动可读消息）；台账记
  //     操作者账号；被拒尝试留痕（audit.denied） ——
  {
    // 无证账号（bob 基础 member）→ 拒＋留痕
    const std::string deny1 =
        run_cli("messages --keyword 合同 --as bob --db " + db2);
    CHECK(deny1.find("不持 auditor 有效角色") != std::string::npos);
    // 授 admin 依旧拒：SystemAdmin 不自动可读消息（蓝图§十六红线）
    {
      ServerStore seed6;
      CHECK(seed6.open(db2));
      CHECK(seed6.role_grant("bob", "admin", "", 0, 0, "alice", 1) > 0);
      seed6.close();
    }
    const std::string deny2 =
        run_cli("messages --keyword 合同 --as bob --db " + db2);
    CHECK(deny2.find("不持 auditor 有效角色") != std::string::npos);
    // 授 auditor → 过；台账操作者记该账号（非系统用户名）
    {
      ServerStore seed6;
      CHECK(seed6.open(db2));
      CHECK(seed6.role_grant("bob", "auditor", "", 0, 0, "alice", 2) > 0);
      seed6.close();
    }
    const std::string out_path6 = "/tmp/memex-archive-export6.txt";
    std::remove(out_path6.c_str());
    const std::string ok1 = run_cli("messages --keyword 合同 --as bob --export " +
                                    out_path6 + " --db " + db2);
    CHECK(ok1.find("已导出至") != std::string::npos);
    const std::string file6 = read_file(out_path6);
    CHECK(file6.find("# 操作者：bob") != std::string::npos); // 账号化留痕
    // 审计自身可对账：bob 先拒两次（audit.denied）后成功一次
    const auto audit_out = run_cli("audit 20 --db " + db2);
    CHECK(audit_out.find("audit.denied") != std::string::npos);
    int denied_rows = 0, bob_rows = 0;
    std::istringstream ss6(audit_out);
    std::string line6;
    while (std::getline(ss6, line6)) {
      if (line6.find("audit.denied") != std::string::npos) ++denied_rows;
      if (line6.find("audit.message.export") != std::string::npos &&
          line6.find("\tbob\t") != std::string::npos)
        ++bob_rows;
    }
    CHECK(denied_rows == 2);
    CHECK(bob_rows >= 1);
    std::remove(out_path6.c_str());
  }

  // —— 平台-11 远程协助 Security Domain 模型：consent 只属受控方／
  //     audit 恒开／五粒度权限位（实批 ⊆ 申请）／部门开关默认禁
  //     白名单口径、部门链上溯 ——
  {
    // 策略面：默认禁；部门行须已存在；部门行覆盖全局；链上溯命中父行
    CHECK(!store.assist_policy_resolve("nobody-未建档"));
    CHECK(!store.assist_policy_set(true, "不存在的部门", "admin1", 1));
    CHECK(store.assist_policy_set(true, "", "admin1", 1)); // 全局放行
    CHECK(store.assist_policy_resolve("alice")); // 未建档者吃全局行
    CHECK(store.assist_policy_set(false, "公司/研发部", "admin1", 2));
    CHECK(!store.assist_policy_resolve("bob")); // 客户端组上溯命中研发部禁行
    CHECK(store.assist_policy_set(true, "公司/研发部/客户端组", "admin1", 3));
    CHECK(store.assist_policy_resolve("bob"));
    // 发起面四闸逐一验拒：自助／对端不存在／空权限／非法位
    CHECK(store.assist_request("alice", "alice", store.kAssistView, 4).empty());
    CHECK(store.assist_request("alice", "幽灵", store.kAssistView, 4).empty());
    CHECK(store.assist_request("alice", "bob", 0, 4).empty());
    CHECK(store.assist_request("alice", "bob", 32, 4).empty());
    // 双方放行 → 成事；id 库内生成（ra-<hex32>）
    const std::string ra_id =
        store.assist_request("alice", "bob",
                             store.kAssistView | store.kAssistClipboard, 5);
    CHECK(ra_id.size() == 35 && ra_id.substr(0, 3) == "ra-");
    // consent 红线：批准只属受控方本人；实批 ⊆ 申请（可缩不可扩）
    CHECK(!store.assist_approve(ra_id, "alice", store.kAssistClipboard, 6));
    CHECK(!store.assist_approve(ra_id, "bob", 32, 6));
    CHECK(!store.assist_approve(ra_id, "bob",
                                store.kAssistView | store.kAssistKeyboard,
                                6)); // keyboard 未申请
    CHECK(!store.assist_start(ra_id, "alice", 7)); // 未批先启
    CHECK(store.assist_approve(ra_id, "bob", store.kAssistClipboard, 8));
    const auto ra = store.assist_session(ra_id);
    CHECK(ra && ra->status == "approved" &&
          ra->granted_mask == store.kAssistClipboard && ra->approved_ms == 8);
    CHECK(!store.assist_start(ra_id, "幽灵", 9)); // 非当事方
    CHECK(store.assist_start(ra_id, "bob", 9)); // 受控方亦可启动
    // 期间撤权：受控方 end 即时断（action 记 end(revoke)）；终态不可再动
    CHECK(store.assist_end(ra_id, "bob", "撤回授权", 10));
    CHECK(!store.assist_end(ra_id, "alice", "再结", 11));
    const auto ra2 = store.assist_session(ra_id);
    CHECK(ra2 && ra2->status == "closed" && ra2->end_actor == "bob" &&
          ra2->end_reason == "撤回授权" && ra2->ended_ms == 10);
    // deny 腿：第二会话受控方拒；发起方越权拒
    const std::string ra_id2 =
        store.assist_request("alice", "bob", store.kAssistView, 12);
    CHECK(!ra_id2.empty());
    CHECK(!store.assist_deny(ra_id2, "alice", 13));
    CHECK(store.assist_deny(ra_id2, "bob", 13));
    const auto ra3 = store.assist_session(ra_id2);
    CHECK(ra3 && ra3->status == "denied" && ra3->ended_ms == 13);
    // 清单与审计对账：单会话全链四条（request→approve→start→end(revoke)）
    CHECK(store.assist_sessions("bob").size() >= 2);
    const auto trail = store.assist_audits(ra_id);
    CHECK(trail.size() == 4);
    if (trail.size() == 4) {
      CHECK(trail[0].action == "request" && trail[0].actor == "alice");
      CHECK(trail[1].action == "approve" && trail[1].actor == "bob");
      CHECK(trail[2].action == "start" && trail[2].actor == "bob");
      CHECK(trail[3].action == "end(revoke)" && trail[3].actor == "bob" &&
            trail[3].detail == "撤回授权");
    }
    bool pol_audited = false; // 策面变更留痕（session_id 空、部门随详情）
    for (const auto& a : store.assist_audits(""))
      if (a.action == "policy" && a.actor == "admin1" &&
          a.session_id.empty())
        pol_audited = true;
    CHECK(pol_audited);
    // CLI 面：policy set → request → approve(子集) → start → show 全链
    CHECK(run_cli("assist policy set on --by admin1 --db " + db2)
              .find("已配置") != std::string::npos);
    const std::string cli_out =
        run_cli("assist request alice bob --perms view,keyboard --db " + db2);
    CHECK(cli_out.find("ra-") != std::string::npos);
    const auto ra_pos = cli_out.find("ra-");
    const std::string cli_id = cli_out.substr(ra_pos, 35);
    CHECK(run_cli("assist approve " + cli_id + " --by bob --perms keyboard --db " +
                  db2)
              .find("已批准") != std::string::npos);
    CHECK(run_cli("assist start " + cli_id + " --by alice --db " + db2)
              .find("已开始") != std::string::npos);
    const std::string shown = run_cli("assist show " + cli_id + " --db " + db2);
    CHECK(shown.find("active") != std::string::npos &&
          shown.find("keyboard") != std::string::npos);
    CHECK(run_cli("assist policy show bob --db " + db2)
              .find("放行") != std::string::npos);
    CHECK(run_cli("assist end " + cli_id + " --by bob --reason 收工 --db " + db2)
              .find("已结束") != std::string::npos);
  }

  if (g_failures == 0) {
    std::cout << "archive tests: all passed\n";
    return 0;
  }
  std::cerr << "archive tests: " << g_failures << " failure(s)\n";
  return 1;
}
