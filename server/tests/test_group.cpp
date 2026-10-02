// T4.1 群聊验收：建群／拉人／公告／退群（群主退群＝解散）、成员扇出投递、
// 全量归档一次（to="group:<群号>"）、按成员账号检索联入群消息、
// 离线成员重登补投、非成员发群消息被拒不归档。
// 库级（ServerStore 直测）＋协议级（真实连接）两层。
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

// 阻塞式协议客户端（与 test_org 同款）
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
  // T4.3 在线推送（PRESENCE_DATA）与本文件验收特性无关，自动跳过——
  // 在线表语义由 test_read_presence 显式验收。此处跳过可防他人上线／
  // 下线推送串扰后续帧断言（登录回执／群回执／PONG 探测都不含推送）。
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
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA) return msg;
    }
  }
  // 同步点：PING/PONG 往返后，服务端已处理完此前发来的全部帧
  //（单线程 io，按序处理）——用于「发送后立刻查库」前消除竞态。
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
  // 登录并确认成功（T4.3 起成功登录伴随 PRESENCE_DATA 推送，先于回执）
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
    for (int i = 0; i < 8; ++i) {
      memex::protocol::Message r = read();
      if (r.type() == memex::protocol::v1::LOGIN_RESULT) {
        CHECK(r.login_result().ok());
        return;
      }
      CHECK(r.type() == memex::protocol::v1::PRESENCE_DATA);
    }
    CHECK(false);
  }
  // 群命令；返回回执
  memex::protocol::Message group_cmd(const std::string& op,
                                     std::uint64_t gid = 0,
                                     const std::vector<std::string>& members = {},
                                     const std::string& name = "",
                                     const std::string& announcement = "") {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::GROUP_CMD);
    m.set_from(account_);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    auto* c = m.mutable_group_cmd();
    c->set_op(op);
    c->set_group_id(gid);
    for (const auto& a : members) c->add_members(a);
    c->set_name(name);
    c->set_announcement(announcement);
    send(m);
    memex::protocol::Message r = read();
    CHECK(r.type() == memex::protocol::v1::GROUP_RESULT);
    CHECK(r.group_result().op() == op);
    return r;
  }
  std::string account_;

private:
  std::unique_ptr<asio::ip::tcp::socket> socket_;
};

} // namespace

int main() {
  namespace v1 = memex::protocol::v1;
  // —— 库级：群生命周期 ——
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    for (const auto& [acct, name] :
         std::vector<std::pair<std::string, std::string>>{
             {"alice", "Alice"}, {"bob", "Bob"}, {"carol", "Carol"},
             {"dave", "Dave"}}) {
      CHECK(s.create_account(acct, "pw", name));
    }
    const auto gid = s.create_group("项目群", "alice", {"bob", "carol", "bob"});
    CHECK(gid > 0);
    auto info = s.group_info(gid);
    CHECK(info.has_value());
    CHECK(info->name == "项目群");
    CHECK(info->owner == "alice");
    CHECK(info->announcement.empty());
    CHECK(info->members.size() == 3); // 去重后 bob 一份；群主自动入群
    CHECK(s.is_group_member(gid, "alice"));
    CHECK(s.is_group_member(gid, "carol"));
    CHECK(!s.is_group_member(gid, "dave"));

    CHECK(s.create_group("坏账号", "alice", {"ghost"}) == 0); // 账号不存在
    CHECK(s.create_group("", "alice", {"bob"}) == 0);         // 群名空

    // 拉人：成功；已在群／账号不存在／群不存在拒绝
    CHECK(s.group_invite(gid, "dave"));
    CHECK(!s.group_invite(gid, "dave"));
    CHECK(!s.group_invite(gid, "ghost"));
    CHECK(!s.group_invite(99999, "dave"));

    // 公告：仅群主可设；空串＝清除
    CHECK(s.group_announce(gid, "bob", "bob 版公告") == false);
    CHECK(s.group_announce(gid, "alice", "每周五例会"));
    CHECK(s.group_info(gid)->announcement == "每周五例会");
    CHECK(s.group_announce(gid, "alice", ""));
    CHECK(s.group_info(gid)->announcement.empty());

    // groups_of：成员视角
    CHECK(s.groups_of("dave").size() == 1);
    CHECK(s.groups_of("dave")[0].group_id == gid);
    CHECK(s.groups_of("alice").size() == 1);
    CHECK(s.groups_of("ghost").empty());

    // 退群：普通成员退出；群主退群＝解散（成员表清空，群号与归档保留）
    CHECK(s.group_leave(gid, "carol"));
    CHECK(!s.is_group_member(gid, "carol"));
    CHECK(!s.group_leave(gid, "carol"));  // 已不在群里
    CHECK(!s.group_leave(99999, "alice")); // 群不存在
    CHECK(s.group_leave(gid, "alice"));    // 群主退群＝解散
    CHECK(s.group_members(gid).empty());
    CHECK(s.group_info(gid).has_value());  // 群行仍在（归档可对账）
    CHECK(s.groups_of("bob").empty());
    s.close();
  }

  // —— 协议级：建群→扇出投递→归档→检索→补投→越权拒绝 ——
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    for (const char* acct : {"alice", "bob", "carol", "dave"}) {
      CHECK(s.create_account(acct, "pw", acct));
    }
    asio::io_context io;
    memex::server::CollabServer server(io, s, 0);
    server.start_accept();
    std::thread io_thread([&] { io.run(); });

    TestClient a(io, server.port());
    a.account_ = "alice";
    a.login("alice", "pc-a");
    TestClient b(io, server.port());
    b.account_ = "bob";
    b.login("bob", "pc-b");

    // 建群（alice 建，成员 bob）
    const auto created = a.group_cmd("create", 0, {"bob"}, "项目群");
    CHECK(created.group_result().ok());
    const auto gid = created.group_result().group_id();
    CHECK(gid > 0);

    // 群消息：alice 发，bob 收（to=group:N）；归档恰好一条（to=群标识）
    memex::protocol::Message t;
    t.set_type(v1::TEXT);
    t.set_seq(11);
    t.set_from("alice");
    t.set_to("group:" + std::to_string(gid));
    t.set_ts_ms(now_ms());
    t.mutable_text()->set_text("群内第一条");
    a.send(t);
    const auto ack = a.read();
    CHECK(ack.type() == v1::ACK && ack.seq() == 11); // 发送方受理回执
    const auto got = b.read();
    CHECK(got.type() == v1::TEXT);
    CHECK(got.to() == "group:" + std::to_string(gid));
    CHECK(got.from() == "alice");
    CHECK(got.text().text() == "群内第一条");
    CHECK(!got.msg_id().empty());
    // bob ACK 清自己的队列行（群扇出按接收方清）
    memex::protocol::Message ack2;
    ack2.set_type(v1::ACK);
    ack2.set_from("bob");
    ack2.set_to("server");
    ack2.set_ts_ms(now_ms());
    ack2.mutable_ack()->set_msg_id(got.msg_id());
    b.send(ack2);
    b.sync(); // 服务端处理完 ACK 后再查库
    CHECK(s.offline_count("bob") == 0);

    // 归档断言：一条群消息＝一行（不随成员数翻倍）；按成员账号检索联入
    memex::server::MessageSearch q;
    q.keyword = "群内第一条";
    const auto hits = s.search_messages(q);
    CHECK(hits.size() == 1);
    CHECK(hits[0].to_account == "group:" + std::to_string(gid));
    CHECK(hits[0].from_account == "alice");
    q.keyword.clear();
    q.account = "bob"; // bob 收发的＋所在群的
    const auto bob_view = s.search_messages(q);
    CHECK(bob_view.size() == 1 && bob_view[0].to_account == "group:" + std::to_string(gid));

    // 公告：仅群主；GROUP_QUERY 下发群列表
    CHECK(a.group_cmd("announce", gid, {}, "", "每周五例会").group_result().ok());
    CHECK(!b.group_cmd("announce", gid, {}, "", "bob 版").group_result().ok());
    memex::protocol::Message gq;
    gq.set_type(v1::GROUP_QUERY);
    gq.set_from("bob");
    gq.set_to("server");
    gq.set_ts_ms(now_ms());
    b.send(gq);
    const auto gdata = b.read();
    CHECK(gdata.type() == v1::GROUP_DATA);
    bool seen = false;
    for (const auto& g : gdata.group_data().groups()) {
      if (g.group_id() == gid) {
        seen = g.name() == "项目群" && g.owner() == "alice" &&
               g.announcement() == "每周五例会" && g.members().size() == 2;
      }
    }
    CHECK(seen);

    // 拉人（仅群成员可拉）＋非成员被拒
    CHECK(a.group_cmd("invite", gid, {"carol"}).group_result().ok());
    {
      TestClient d(io, server.port());
      d.account_ = "dave";
      d.login("dave", "pc-d");
      CHECK(!d.group_cmd("invite", gid, {"dave"}).group_result().ok());
      // 非成员发群消息：被拒（无受理回执）且不归档。
      // 用 PING/PONG 探测：若误发 ACK 会先于 PONG 到达。
      memex::protocol::Message bad;
      bad.set_type(v1::TEXT);
      bad.set_seq(21);
      bad.set_from("dave");
      bad.set_to("group:" + std::to_string(gid));
      bad.set_ts_ms(now_ms());
      bad.mutable_text()->set_text("外部塞话");
      d.send(bad);
      memex::protocol::Message ping;
      ping.set_type(v1::PING);
      ping.set_from("dave");
      ping.set_to("server");
      ping.set_ts_ms(now_ms());
      d.send(ping);
      const auto probe = d.read();
      CHECK(probe.type() == v1::PONG); // 未收到群消息受理回执＝被拒
      memex::server::MessageSearch qd;
      qd.keyword = "外部塞话";
      CHECK(s.search_messages(qd).empty()); // 不归档
      CHECK(s.offline_count("bob") == 0);   // 也不入任何成员队列
    } // d 析构断开（下线推送由 read() 自动跳过，不串扰）

    // 离线成员补投：carol 被拉群后未登录，alice 发第二条 → 重登补投
    memex::protocol::Message t2;
    t2.set_type(v1::TEXT);
    t2.set_seq(12);
    t2.set_from("alice");
    t2.set_to("group:" + std::to_string(gid));
    t2.set_ts_ms(now_ms());
    t2.mutable_text()->set_text("给离线成员的群消息");
    a.send(t2);
    CHECK(a.read().type() == v1::ACK);
    CHECK(b.read().type() == v1::TEXT); // bob 在线即投
    CHECK(s.offline_count("carol") == 1); // carol 一行（bob 的已收不重排）

    TestClient c(io, server.port());
    c.account_ = "carol";
    c.login("carol", "pc-c"); // read() 已吃掉 LOGIN_RESULT（推送自动跳过）
    const auto replay = c.read();
    CHECK(replay.type() == v1::TEXT);
    CHECK(replay.to() == "group:" + std::to_string(gid));
    CHECK(replay.text().text() == "给离线成员的群消息");
    CHECK(replay.from() == "alice");
    CHECK(replay.msg_id() ==
          memex::server::sha256_hex("alice:12")); // sha256(from:seq)
    // carol ACK 后队列清空
    memex::protocol::Message ack3;
    ack3.set_type(v1::ACK);
    ack3.set_from("carol");
    ack3.set_to("server");
    ack3.set_ts_ms(now_ms());
    ack3.mutable_ack()->set_msg_id(replay.msg_id());
    c.send(ack3);
    c.sync();
    CHECK(s.offline_count("carol") == 0);
    // 归档仍只有两条群消息（补投不重复归档）
    memex::server::MessageSearch q2;
    q2.account = "carol";
    CHECK(s.search_messages(q2).size() == 2);

    // 群主退群＝解散：群列表清空，前成员发群消息被拒不归档（PING 探测）
    CHECK(a.group_cmd("leave", gid).group_result().ok());
    memex::protocol::Message gq2;
    gq2.set_type(v1::GROUP_QUERY);
    gq2.set_from("bob");
    gq2.set_to("server");
    gq2.set_ts_ms(now_ms());
    b.send(gq2);
    const auto gdata2 = b.read();
    CHECK(gdata2.type() == v1::GROUP_DATA);
    CHECK(gdata2.group_data().groups().empty()); // 解散后群列表为空
    memex::protocol::Message bad2;
    bad2.set_type(v1::TEXT);
    bad2.set_seq(31);
    bad2.set_from("bob");
    bad2.set_to("group:" + std::to_string(gid));
    bad2.set_ts_ms(now_ms());
    bad2.mutable_text()->set_text("解散后的群消息");
    b.send(bad2);
    memex::protocol::Message ping2;
    ping2.set_type(v1::PING);
    ping2.set_from("bob");
    ping2.set_to("server");
    ping2.set_ts_ms(now_ms());
    b.send(ping2);
    CHECK(b.read().type() == v1::PONG); // 无受理回执＝被拒
    memex::server::MessageSearch q3;
    q3.keyword = "解散后的群消息";
    CHECK(s.search_messages(q3).empty()); // 不归档（历史两条保留）

    io.stop();
    io_thread.join();
    s.close();
  }

  if (g_failures == 0) {
    std::cout << "group tests: all passed\n";
    return 0;
  }
  std::cerr << "group tests: " << g_failures << " failure(s)\n";
  return 1;
}
