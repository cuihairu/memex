// 邮箱·统一收件箱验收（设计稿 docs/design/mailbox.md）：
//  P1 三类条目全真实入箱（公告=GROUP_CMD announce、通知=@我群消息/任务
//     到期回执/审批结果/备忘录变更、告警=webhook 上报/agent 心跳磁盘
//     阈值/连续口令不符登录异常）＋未读计数（服务端为唯一来源，重开库
//     持久）＋标记已读后计数下降且归档行数不变（R3 断言）＋告警标记
//     已处理带处理人留痕；
//  P2 公告已读确认留痕（谁/何时持久化，按条目可查；非收件人 403）；
//  P3 admin 事件源接线（磁盘阈值受众=admin 账号集，member 不可见＝
//     §Ⅵ-2 默认口径；登录异常恰第 3 次入箱一次）。
// 边界②：邮箱操作只动 mailbox 两表，messages 归档行一概不动（计数断言）。
// 服务端核心库直链运行；HTTP 面真实 TCP 往返；协议面走阻塞 TestClient。
#include <asio.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cred.hpp"
#include "files_server.hpp"
#include "server.hpp"
#include "storage.hpp"
#include "store.hpp"
#include "webhook.hpp"

using asio::ip::tcp;
using json = nlohmann::json;
using memex::server::CollabServer;
using memex::server::FileServer;
using memex::server::ServerStore;
using memex::server::WebhookServer;

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

// —— 阻塞式协议客户端（长度前缀帧；跳过与断言无关的推送帧）——
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
    asio::write(*socket_, asio::buffer(memex::protocol::encode(msg)));
  }

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
          msg.type() != memex::protocol::v1::NOTICE) {
        return msg;
      }
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
                                   const std::string& body) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::TEXT);
  m.set_seq(seq);
  m.set_from(from);
  m.set_to(to);
  m.set_ts_ms(now_ms());
  m.mutable_text()->set_text(body);
  return m;
}

struct HttpReply {
  int status{0};
  std::string body;
};

HttpReply http(std::uint16_t port, const std::string& method,
               const std::string& path,
               const std::map<std::string, std::string>& headers,
               const std::string& body) {
  HttpReply rep;
  asio::io_context io;
  asio::ip::tcp::socket s(io);
  s.connect({asio::ip::make_address("127.0.0.1"), port});
  std::string req = method + " " + path + " HTTP/1.1\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
  req += "\r\n" + body;
  asio::write(s, asio::buffer(req));
  asio::error_code ec;
  std::string raw;
  char buf[8192];
  for (;;) {
    const std::size_t n = s.read_some(asio::buffer(buf), ec);
    if (ec) break;
    raw.append(buf, n);
  }
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) return rep;
  rep.status = std::atoi(raw.c_str() + 9); // "HTTP/1.1 NNN"
  rep.body = raw.substr(head_end + 4);
  return rep;
}

std::string jstr(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return "";
  std::size_t i = pos + k.size();
  while (i < j.size() && (j[i] == ' ' || j[i] == '"')) ++i;
  std::string out;
  while (i < j.size() && j[i] != '"') out += j[i++];
  return out;
}
std::int64_t jint(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return 0;
  return std::atoll(j.c_str() + pos + k.size());
}

class FakeS3 final : public memex::server::S3Storage {
 public:
  bool create_bucket() override { return true; }
  bool put_object(const std::string& key, const std::string& body,
                  std::string* etag_out) override {
    objects_[key] = body;
    if (etag_out) *etag_out = "\"fake\"";
    return true;
  }
  bool get_object(const std::string& key, std::string* body_out) override {
    const auto it = objects_.find(key);
    if (it == objects_.end()) return false;
    *body_out = it->second;
    return true;
  }
  bool delete_object(const std::string& key) override {
    return objects_.erase(key) > 0;
  }
  std::vector<std::string> list_objects(const std::string& prefix,
                                        int max_keys) override {
    std::vector<std::string> out;
    for (const auto& [k, v] : objects_) {
      (void)v;
      if (k.rfind(prefix, 0) == 0 && static_cast<int>(out.size()) < max_keys) {
        out.push_back(k);
      }
    }
    return out;
  }
  std::optional<std::string> create_multipart_upload(
      const std::string&) override {
    return "fake-upload-id";
  }
  std::optional<std::string> upload_part(const std::string&,
                                         const std::string&, int,
                                         const std::string&) override {
    return "\"fake-part\"";
  }
  std::optional<memex::server::CompleteUploadResult> complete_multipart_upload(
      const std::string& key, const std::string&,
      const std::vector<memex::server::UploadPartResult>&) override {
    memex::server::CompleteUploadResult r;
    r.bucket = "fake";
    r.key = key;
    return r;
  }
  bool abort_multipart_upload(const std::string&,
                              const std::string&) override {
    return true;
  }
  std::string presign_put(const std::string&, int) override { return ""; }
  std::string presign_get(const std::string&, int) override { return ""; }
  bool head_object(const std::string& key, std::int64_t* size_out,
                   std::string*) override {
    const auto it = objects_.find(key);
    if (it == objects_.end()) return false;
    if (size_out) *size_out = static_cast<std::int64_t>(it->second.size());
    return true;
  }
  bool delete_objects(const std::vector<std::string>& keys) override {
    for (const auto& k : keys) objects_.erase(k);
    return true;
  }

  std::map<std::string, std::string> objects_;
};

} // namespace

int main() {
  // 文件库（非 :memory:）：重开库读计数＝P1「服务端为唯一来源」断言
  const std::string db_path = "/tmp/memex-test-mailbox-" +
                              std::to_string(static_cast<long>(::getpid())) +
                              ".db";
  ::remove(db_path.c_str());

  asio::io_context io;
  ServerStore store;
  CHECK(store.open(db_path));
  // owner1=admin（告警受众；账号集=T3.1 role 分级）；member1/2 普通成员
  CHECK(store.create_account("owner1", "pw-owner1", "王航", "admin"));
  CHECK(store.create_account("member1", "pw-member1", "李文"));
  CHECK(store.create_account("member2", "pw-member2", "王倩"));
  CHECK(store.create_account("outsider", "pw-outsider", "陈静"));
  const auto gid =
      store.create_group("研发组", "owner1", {"owner1", "member1", "member2"});
  CHECK(gid > 0);
  const std::string gtarget = "group:" + std::to_string(gid);

  CollabServer server(io, store, 0);
  auto fake = std::make_shared<FakeS3>();
  FileServer files(io, store, fake, 0);
  WebhookServer wh(io, server, 0);
  server.start_accept();
  files.start_accept();
  wh.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t fport = files.port();

  std::map<std::string, std::string> tok;
  for (const char* a : {"owner1", "member1", "member2", "outsider"}) {
    const auto r = http(fport, "POST", "/files/session", {},
                        "{\"account\":\"" + std::string(a) +
                            "\",\"password\":\"pw-" + std::string(a) + "\"}");
    CHECK(r.status == 200);
    tok[a] = jstr(r.body, "token");
    CHECK(!tok[a].empty());
  }
  const auto H = [&tok](const char* a) {
    return std::map<std::string, std::string>{
        {"Authorization", "Bearer " + tok.at(a)}};
  };
  const auto mailbox_list = [&](const char* a, const std::string& body) {
    return http(fport, "POST", "/files/mailbox/list", H(a), body);
  };
  const auto unread_of = [&](const char* a, const char* kind) {
    const auto r = mailbox_list(a, "{}");
    CHECK(r.status == 200);
    const std::string k = std::string("\"") + kind + "\":";
    const auto pos = r.body.find(k, r.body.find("\"counts\""));
    return pos == std::string::npos ? -1 : jint(r.body, kind);
  };
  const auto item_id = [&](const char* a, const char* kind,
                           const std::string& title_hit) {
    const auto r = mailbox_list(a, "{}");
    CHECK(r.status == 200);
    const auto pos = r.body.find(title_hit);
    if (pos == std::string::npos) return 0LL;
    // 条目 JSON 键字典序（id 在 title 前）：回溯该条目内最近的 "id": 取值
    const auto idpos = r.body.rfind("\"id\":", pos);
    if (idpos == std::string::npos) return 0LL;
    return std::atoll(r.body.c_str() + idpos + 5);
  };
  const auto archive_count = [&]() {
    return static_cast<std::int64_t>(
        store.search_messages({"", "", 0, 0, 100000}).size());
  };

  // 未登录 401；坏 type 400
  CHECK(http(fport, "POST", "/files/mailbox/list", {}, "{}").status == 401);
  CHECK(mailbox_list("member1", "{\"type\":\"spam\"}").status == 400);

  // —— P1/P2 公告：GROUP_CMD announce → 公告条目＋确认台账 ——
  {
    TestClient a(io, server.port());
    a.send(make_login("owner1", "pw-owner1", "pc-owner1"));
    CHECK(a.read().login_result().ok());
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::GROUP_CMD);
    m.set_from("owner1");
    m.set_to("server");
    m.set_ts_ms(now_ms());
    auto* c = m.mutable_group_cmd();
    c->set_op("announce");
    c->set_group_id(gid);
    c->set_announcement("国庆值班安排（需确认已读）\n第二行正文");
    a.send(m);
    const auto r = a.read();
    CHECK(r.type() == memex::protocol::v1::GROUP_RESULT);
    CHECK(r.group_result().ok());

    // 收件人=群成员不含设置者（镜像 NOTICE 扇出）：member1/member2 各一
    const auto aid = item_id("member1", "announce", "国庆值班安排");
    CHECK(aid > 0);
    CHECK(item_id("member2", "announce", "国庆值班安排") > 0);
    CHECK(mailbox_list("owner1", "{\"type\":\"announce\"}")
               .body.find("国庆值班安排") == std::string::npos);
    // 标题=公告首行；sub/来源群名；确认应收 2（P2 台账口径）
    const auto l1 = mailbox_list("member1", "{\"type\":\"announce\"}");
    CHECK(l1.body.find("研发组") != std::string::npos);
    CHECK(jint(l1.body.c_str(), "confirmTotal") == 2);
    // 确认已读：200 → 已确认（confirmedMs>0，留痕谁/何时）；重复确认 409
    CHECK(http(fport, "POST", "/files/mailbox/confirm", H("member1"),
               "{\"id\":" + std::to_string(aid) + "}")
              .status == 200);
    CHECK(http(fport, "POST", "/files/mailbox/confirm", H("member1"),
               "{\"id\":" + std::to_string(aid) + "}")
              .status == 409);
    // 确认台账：member1 已确认（时间>0）在前、member2 未确认在后；
    // 非收件人 outsider 403（判权=状态行在场）
    const auto cf = http(fport, "POST", "/files/mailbox/confirmations",
                         H("member1"), "{\"id\":" + std::to_string(aid) + "}");
    CHECK(cf.status == 200);
    CHECK(cf.body.find("\"account\":\"member1\"") != std::string::npos);
    CHECK(cf.body.find("\"account\":\"member2\"") != std::string::npos);
    CHECK(cf.body.find("\"confirmedMs\":0") != std::string::npos); // member2 未确认
    CHECK(http(fport, "POST", "/files/mailbox/confirmations", H("outsider"),
               "{\"id\":" + std::to_string(aid) + "}")
              .status == 403);
    // member1 公告未读归零（确认即已读）；member2 仍 1
    CHECK(unread_of("member1", "announce") == 0);
    CHECK(unread_of("member2", "announce") == 1);
  }

  // —— P1 通知·@我：群消息 @member1（显示名「李文」）→ 通知条目 ——
  {
    TestClient b(io, server.port());
    b.send(make_login("member2", "pw-member2", "pc-member2"));
    CHECK(b.read().login_result().ok());
    b.send(make_text("member2", gtarget, 1, "对齐会材料 @李文 请先审"));
    const auto ack = b.read();
    CHECK(ack.type() == memex::protocol::v1::ACK);
    const auto mid = item_id("member1", "notice", "在群「研发组」中 @ 了你");
    CHECK(mid > 0);
    // 未被 @ 的 member2 不产生条目；@member1 不误中 @member12 类前缀
    CHECK(item_id("member2", "notice", "@ 了你") == 0);
    // 标记已读：计数下降＋归档行数不变（R3 断言——邮箱只动状态行）
    const auto before = archive_count();
    CHECK(unread_of("member1", "notice") == 1);
    CHECK(http(fport, "POST", "/files/mailbox/read", H("member1"),
               "{\"id\":" + std::to_string(mid) + "}")
              .status == 200);
    CHECK(unread_of("member1", "notice") == 0);
    CHECK(archive_count() == before);
  }

  // —— P1 通知·任务到期：提醒回执入箱，重复回执不重复入箱（dedup）——
  {
    const auto t = http(fport, "POST", "/files/tasks", H("member1"),
                        "{\"title\":\"Q3 复盘文档\",\"due_ms\":1770000000000}");
    CHECK(t.status == 200);
    const auto tid = jint(t.body, "id");
    CHECK(http(fport, "POST", "/files/tasks/reminded", H("member1"),
               "{\"id\":" + std::to_string(tid) + "}")
              .status == 200);
    CHECK(item_id("member1", "notice", "任务「Q3 复盘文档」已到截止时间") > 0);
    CHECK(http(fport, "POST", "/files/tasks/reminded", H("member1"),
               "{\"id\":" + std::to_string(tid) + "}")
              .status == 200);
    const auto l = mailbox_list("member1", "{\"type\":\"notice\",\"q\":\"复盘\"}");
    CHECK(l.body.find("已到截止时间") != std::string::npos);
    CHECK(l.body.find("已到截止时间", l.body.find("已到截止时间") + 1) ==
          std::string::npos); // dedup：只一条
  }

  // —— P1 通知·审批结果：直属上级决定 → 申请人收通知（落定才提醒）——
  {
    const auto ap = http(fport, "POST", "/files/approvals", H("member1"),
                         "{\"type\":\"年假\"}");
    CHECK(ap.status == 200);
    const auto aid2 = jint(ap.body, "id");
    CHECK(store.reporting_set("member1", "owner1", 1));
    CHECK(http(fport, "POST", "/files/approvals/decide", H("owner1"),
               "{\"id\":" + std::to_string(aid2) + ",\"approved\":true}")
              .status == 200);
    CHECK(item_id("member1", "notice", "「年假」申请已通过") > 0);
    // 再驳回一笔（新申请）→ 被驳回文案入箱
    const auto ap2 = http(fport, "POST", "/files/approvals", H("member1"),
                          "{\"type\":\"事假\"}");
    CHECK(http(fport, "POST", "/files/approvals/decide", H("owner1"),
               "{\"id\":" + std::to_string(jint(ap2.body, "id")) +
                   ",\"approved\":false}")
              .status == 200);
    CHECK(item_id("member1", "notice", "「事假」申请已被驳回") > 0);
  }

  // —— P1 通知·共享变更：群备忘录保存 → 其余成员收通知（编辑者除外）——
  // 备忘录默认管理员维护（memo:write＝群主/管理员）：owner1 建/改，
  // 成员 member1/member2 收通知，编辑者 owner1 不收
  {
    const auto mk = http(fport, "POST", "/files/group-memo/save", H("owner1"),
                         "{\"gid\":" + std::to_string(gid) +
                             ",\"title\":\"测试环境说明\",\"content\":\"首次落库\"}");
    CHECK(mk.status == 200);
    const auto mid = jint(mk.body, "id");
    CHECK(mid > 0);
    CHECK(item_id("member2", "notice", "备忘录「测试环境说明」已创建") > 0);
    CHECK(item_id("member1", "notice", "备忘录「测试环境说明」已创建") > 0);
    CHECK(item_id("owner1", "notice", "备忘录「测试环境说明」已创建") == 0);
    // 同条目同内容重复保存不重复入箱（dedup=群+条目+内容摘要）
    CHECK(http(fport, "POST", "/files/group-memo/save", H("owner1"),
               "{\"gid\":" + std::to_string(gid) + ",\"id\":" +
                   std::to_string(mid) +
                   ",\"title\":\"测试环境说明\",\"content\":\"首次落库\"}")
              .status == 200);
    const auto l =
        mailbox_list("member2", "{\"type\":\"notice\",\"q\":\"备忘录\"}");
    int created_hits = 0;
    for (std::size_t p = l.body.find("已创建"); p != std::string::npos;
         p = l.body.find("已创建", p + 1))
      ++created_hits;
    CHECK(created_hits == 1);
    // 内容变了＝新条目（变更事件）
    CHECK(http(fport, "POST", "/files/group-memo/save", H("owner1"),
               "{\"gid\":" + std::to_string(gid) + ",\"id\":" +
                   std::to_string(mid) +
                   ",\"title\":\"测试环境说明\",\"content\":\"第二次修订\"}")
              .status == 200);
    CHECK(item_id("member2", "notice", "备忘录「测试环境说明」有变更") > 0);
  }

  // —— P3 告警·webhook 上报：NOTICE 照投＋告警条目入箱 ——
  {
    CHECK(store.webhook_create(memex::server::sha256_hex("whk_mbx"), gtarget,
                               "群机器人", now_ms()) > 0);
    HttpReply r;
    {
      asio::io_context io2;
      asio::ip::tcp::socket s(io2);
      s.connect({asio::ip::make_address("127.0.0.1"), wh.port()});
      const std::string body =
          R"({"title":"磁盘告警：srv-app-02","content":"df 91%"})";
      const std::string req =
          "POST /hook/whk_mbx HTTP/1.1\r\nContent-Length: " +
          std::to_string(body.size()) + "\r\n\r\n" + body;
      asio::write(s, asio::buffer(req));
      asio::error_code ec;
      std::string raw;
      char buf[8192];
      for (;;) {
        const std::size_t n = s.read_some(asio::buffer(buf), ec);
        if (ec) break;
        raw.append(buf, n);
      }
      r.status = std::atoi(raw.c_str() + 9);
    }
    CHECK(r.status == 200);
    CHECK(item_id("member1", "warning", "磁盘告警：srv-app-02") > 0);
    CHECK(item_id("owner1", "warning", "磁盘告警：srv-app-02") > 0);
    CHECK(unread_of("member1", "warning") == 1);
  }

  // —— P3 告警·磁盘阈值：agent 心跳 ≥85% → admin 受众；member 不可见
  //     （§Ⅵ-2 默认：非管理员不看系统详情）；持续超阈值不重复报；
  //     全员处理后复发可再报 ——
  {
    const auto sid = store.server_enroll(gid, "srv-db-01", "10.0.0.5",
                                         memex::server::sha256_hex("tok_srv"),
                                         "owner1", now_ms());
    CHECK(sid > 0);
    const auto hb = [&](const char* used, const char* total) {
      std::ostringstream body;
      body << "{\"token\":\"tok_srv\",\"cpu_percent\":12,"
              "\"mem_used_mb\":100,\"mem_total_mb\":1000,\"disk_used_mb\":"
           << used << ",\"disk_total_mb\":" << total
           << ",\"load1\":0.1}";
      return http(fport, "POST", "/files/group-servers/heartbeat", {},
                  body.str());
    };
    const auto before = archive_count();
    CHECK(hb("910", "1000").status == 200); // 91% ≥ 85%
    const auto wid = item_id("owner1", "warning", "磁盘使用率超过 85%");
    CHECK(wid > 0);
    CHECK(item_id("member1", "warning", "磁盘使用率超过 85%") ==
          0); // member 非 admin 不可见
    CHECK(hb("920", "1000").status == 200); // 持续超阈值＝不重复报
    const auto l = mailbox_list("owner1", "{\"type\":\"warning\",\"q\":\"85\"}");
    CHECK(l.body.find("磁盘使用率超过 85%", l.body.find("磁盘使用率超过 85%") +
                                             1) ==
          std::string::npos);
    CHECK(archive_count() == before); // 心跳不上归档（对账）

    // 告警标记已处理：处理人+时间留痕；再心跳＝复发再报（告警再入语义）
    CHECK(http(fport, "POST", "/files/mailbox/handle", H("owner1"),
               "{\"id\":" + std::to_string(wid) + "}")
              .status == 200);
    CHECK(http(fport, "POST", "/files/mailbox/handle", H("owner1"),
               "{\"id\":" + std::to_string(wid) + "}")
              .status == 409); // 重复处理 409
    // 处理留痕（store 直查：处理人=操作账号、时间>0、state=2）
    const auto rows = store.mailbox_of("owner1", "warning", false, "85");
    CHECK(rows.size() == 1);
    CHECK(rows[0].state == 2 && rows[0].handled_by == "owner1" &&
          rows[0].handled_ms > 0);
    CHECK(hb("930", "1000").status == 200); // 修复后复发：新告警条目
    const auto rows2 = store.mailbox_of("owner1", "warning", false, "85");
    CHECK(rows2.size() == 2);
  }

  // —— P3 告警·登录异常：连续 3 次口令不符恰第 3 次入箱一次 ——
  {
    for (int i = 0; i < 3; ++i) {
      TestClient c(io, server.port());
      c.send(make_login("member1", "wrong-pw", "pc-evil"));
      const auto r = c.read();
      CHECK(r.type() == memex::protocol::v1::LOGIN_RESULT);
      CHECK(!r.login_result().ok());
    }
    CHECK(item_id("owner1", "warning", "账号 member1 连续 3 次口令不符") > 0);
    CHECK(item_id("member1", "warning",
                  "账号 member1 连续 3 次口令不符") == 0); // 受众=admin
    // 第 4 次：不再入箱（恰第 3 次一次）
    {
      TestClient c(io, server.port());
      c.send(make_login("member1", "wrong-pw-again", "pc-evil2"));
      CHECK(!c.read().login_result().ok());
    }
    const auto rows =
        store.mailbox_of("owner1", "warning", false, "口令不符");
    CHECK(rows.size() == 1);
  }

  // —— P1 邮箱面 API：过滤/搜索/只看未读/全部已读/未读计数同源 ——
  {
    // type 过滤
    CHECK(mailbox_list("member1", "{\"type\":\"warning\"}")
              .body.find("已到截止时间") == std::string::npos);
    // 只看未读：member1 剩 未读=备忘录变更未读+webhook 告警 等
    const auto un = mailbox_list("member1", "{\"unread\":true}");
    CHECK(un.body.find("\"state\":0") != std::string::npos);
    CHECK(un.body.find("\"state\":1") == std::string::npos);
    // 搜索命中来源
    const auto q = mailbox_list("member1", "{\"q\":\"任务到期\"}");
    CHECK(q.body.find("已到截止时间") != std::string::npos);
    // 全部标记已读：member1 未读清零＋归档行数不变（边界②再断言）
    const auto before = archive_count();
    const auto all = http(fport, "POST", "/files/mailbox/read", H("member1"),
                          "{\"all\":true}");
    CHECK(all.status == 200);
    CHECK(jint(all.body, "marked") > 0);
    CHECK(unread_of("member1", "all") == 0);
    CHECK(archive_count() == before);
    // member2 未读不受影响（按账号·按条目独立）
    CHECK(unread_of("member2", "announce") == 1);
    // 通知条目不可标记已处理（仅告警）：409
    const auto nid = item_id("member1", "notice", "已到截止时间");
    CHECK(http(fport, "POST", "/files/mailbox/handle", H("member1"),
               "{\"id\":" + std::to_string(nid) + "}")
              .status == 409);
    // 公告条目不可 handle；确认非公告条目 409（kind 闸）
    CHECK(http(fport, "POST", "/files/mailbox/confirm", H("member1"),
               "{\"id\":" + std::to_string(nid) + "}")
              .status == 409);
  }

  // —— P1 未读计数跨重开库持久（服务端为唯一来源；三面同源同数）——
  {
    ServerStore s2;
    CHECK(s2.open(db_path));
    const auto c1 = s2.mailbox_counts("member2");
    const auto c2 = store.mailbox_counts("member2");
    CHECK(c1.all == c2.all && c1.all > 0);
    CHECK(c1.warning == c2.warning && c1.notice == c2.notice &&
          c1.announce == c2.announce);
  }

  io.stop();
  io_thread.join();
  ::remove(db_path.c_str());

  if (g_failures == 0) {
    std::cout << "mailbox tests: all passed\n";
    return 0;
  }
  std::cerr << "mailbox tests: " << g_failures << " failure(s)\n";
  return 1;
}
