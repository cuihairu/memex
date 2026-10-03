// T4.10 webhook 通知接入验收：token 鉴权（错 token／吊销后 401）、payload
// 校验（缺字段／坏 urgency 400、目标不一致 403、目标不存在 404、方法／路径
// 405/404）、投递管线（在线即投 NOTICE 帧携三级紧急程度、离线入队与登录补投
// ＋ACK 清队、群扇出全员、归档 type=48 且正文与客户端同源 compose、对账计数）。
// 服务端核心库直链运行（io 线程驱动，不起进程）；HTTP 面为真实 TCP 往返。
#include <asio.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "server.hpp"
#include "store.hpp"
#include "webhook.hpp"

using asio::ip::tcp;
using json = nlohmann::json;
using memex::server::CollabServer;
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

// 阻塞式测试客户端：帧收发（长度前缀 + Envelope）；自动跳过与本文件无关的
// 推送帧（PRESENCE_DATA／FAV_DATA——通知投递会触发常用联系人刷新推送）。
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
          msg.type() != memex::protocol::v1::FAV_DATA) {
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

memex::protocol::Message make_ack(const std::string& msg_id) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::ACK);
  m.set_from("receiver");
  m.set_to("server");
  m.mutable_ack()->set_msg_id(msg_id);
  return m;
}

struct HttpResponse {
  int status{0};
  json body;
};

// 真实 HTTP 往返（独立 io_context，阻塞直到服务端 Connection: close 落 EOF）
HttpResponse http_request(std::uint16_t port, const std::string& method,
                          const std::string& path, const std::string& body) {
  HttpResponse out;
  try {
    asio::io_context io;
    tcp::socket sock(io);
    sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    std::ostringstream req;
    req << method << ' ' << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    const std::string req_bytes = req.str();
    asio::write(sock, asio::buffer(req_bytes));
    std::string response;
    asio::error_code ec;
    asio::read(sock, asio::dynamic_buffer(response), ec); // 读到 EOF
    const auto head_end = response.find("\r\n\r\n");
    if (head_end == std::string::npos) return out;
    out.status = std::atoi(response.c_str() + strlen("HTTP/1.1 "));
    try {
      out.body = json::parse(response.substr(head_end + 4));
    } catch (const std::exception&) {
      // body 非 JSON：保持 body 空，由调用方按 status 断言
    }
  } catch (const std::exception& e) {
    std::cerr << "HTTP 请求异常：" << e.what() << '\n';
  }
  return out;
}

HttpResponse post_json(std::uint16_t port, const std::string& token,
                       const std::string& body) {
  return http_request(port, "POST", "/hook/" + token, body);
}

// —— 校验面：鉴权／payload／目标（不出 400/401/403/404/405，不投递成功） ——
void test_http_validation(ServerStore& store, std::uint16_t port) {
  // 合法 webhook（个人，目标 carol——与投递面的 alice 隔离）＋错绑／幽灵目标
  // webhook（用于 403／404 两态）
  CHECK(store.webhook_create(memex::server::sha256_hex("whk_ok"),
                             "carol", "个人通知", now_ms()) > 0);
  CHECK(store.webhook_create(memex::server::sha256_hex("whk_ghost"),
                             "nobody", "", now_ms()) > 0);
  CHECK(store.webhook_create(memex::server::sha256_hex("whk_ghostgroup"),
                             "group:424242", "", now_ms()) > 0);
  CHECK(store.webhook_create(memex::server::sha256_hex("whk_mismatch"),
                             "bob", "", now_ms()) > 0);

  // ① 错 token → 401（含不存在与已吊销两态，吊销在本节末验证）
  auto r = post_json(port, "whk_wrong",
                     R"({"title":"t","content":"c"})");
  CHECK(r.status == 401);
  CHECK(r.body.contains("ok") && r.body["ok"] == false);

  // ② 非法 JSON → 400；非对象 → 400
  r = post_json(port, "whk_ok", "not-json");
  CHECK(r.status == 400);
  r = post_json(port, "whk_ok", "[1,2]");
  CHECK(r.status == 400);

  // ③ 必填字段：缺 title／缺 content／空 title → 400
  r = post_json(port, "whk_ok", R"({"content":"c"})");
  CHECK(r.status == 400);
  r = post_json(port, "whk_ok", R"({"title":"t"})");
  CHECK(r.status == 400);
  r = post_json(port, "whk_ok", R"({"title":"","content":"c"})");
  CHECK(r.status == 400);

  // ④ urgency 非法值 → 400；类型非字符串 → 400
  r = post_json(port, "whk_ok",
                R"({"title":"t","content":"c","urgency":"fatal"})");
  CHECK(r.status == 400);
  r = post_json(port, "whk_ok",
                R"({"title":"t","content":"c","urgency":3})");
  CHECK(r.status == 400);

  // ⑤ payload target 与绑定目标不一致 → 403；绑定目标不存在 → 404
  r = post_json(port, "whk_mismatch",
                R"({"title":"t","content":"c","target":"alice"})");
  CHECK(r.status == 403);
  r = post_json(port, "whk_ghost", R"({"title":"t","content":"c"})");
  CHECK(r.status == 404);
  r = post_json(port, "whk_ghostgroup", R"({"title":"t","content":"c"})");
  CHECK(r.status == 404); // 绑定目标群不存在
  r = post_json(port, "whk_ghostgroup",
                R"({"title":"t","content":"c","target":"group:424242"})");
  CHECK(r.status == 404); // payload target 与绑定一致（＝幽灵群）→ 404

  // ⑥ 方法与路径：GET → 405；错路径 → 404；无 token → 401
  r = http_request(port, "GET", "/hook/whk_ok", "");
  CHECK(r.status == 405);
  r = http_request(port, "POST", "/nope", "{}");
  CHECK(r.status == 404);
  r = http_request(port, "POST", "/hook/", R"({"title":"t","content":"c"})");
  CHECK(r.status == 401);

  // ⑦ 合法 payload（含跳转）→ 200，msg_id 非空、recipients=1
  r = post_json(port, "whk_ok",
                R"({"title":"发布通知","content":"今晚 20:00 断网演练",)"
                R"("urgency":"important","jump_url":"https://oa.local/123"})");
  CHECK(r.status == 200);
  CHECK(r.body["ok"] == true);
  CHECK(r.body.contains("msg_id") && !r.body["msg_id"].get<std::string>().empty());
  CHECK(r.body["recipients"] == 1);

  // ⑧ 吊销后同 token → 401
  const auto rows = store.webhook_list();
  std::int64_t ok_id = 0;
  for (const auto& w : rows) {
    if (w.token_hash == memex::server::sha256_hex("whk_ok")) ok_id = w.id;
  }
  CHECK(ok_id > 0);
  CHECK(store.webhook_revoke(ok_id));
  CHECK(!store.webhook_revoke(ok_id)); // 重复吊销幂等拒绝
  r = post_json(port, "whk_ok", R"({"title":"t","content":"c"})");
  CHECK(r.status == 401);

  // 合法 payload 的那一次已真实入队（carol 离线）：失败请求不入队
  CHECK(store.offline_count("carol") == 1);
  CHECK(store.offline_count("alice") == 0);
  CHECK(store.offline_count("bob") == 0);
}

// —— 投递面：在线即投／离线补投＋ACK／群扇出／归档对账 ——
void test_delivery_pipeline(ServerStore& store, CollabServer& server,
                            std::uint16_t wh_port) {
  const auto gid = store.create_group("演练群", "alice", {"alice", "bob"});
  CHECK(gid > 0);
  CHECK(store.webhook_create(memex::server::sha256_hex("whk_group"),
                             "group:" + std::to_string(gid), "群通知",
                             now_ms()) > 0);
  CHECK(store.webhook_create(memex::server::sha256_hex("whk_alice2"),
                             "alice", "", now_ms()) > 0);

  // ① 在线即投：alice 登录在先，个人通知 HTTP 触达 → 收 NOTICE 帧 → ACK 清队
  {
    asio::io_context io;
    TestClient a(io, server.port());
    a.send(make_login("alice", "pa-w", "pc-alice"));
    CHECK(a.read().login_result().ok());

    auto r = post_json(wh_port, "whk_undef", "{}"); // 不存在 token → 401
    CHECK(r.status == 401);
    r = post_json(wh_port, "whk_alice2",
                  R"({"title":"站内提醒","content":"请查收周报"})");
    CHECK(r.status == 200);
    const std::string msg_id = r.body["msg_id"].get<std::string>();
    CHECK(store.offline_count("alice") == 1); // 已入队待 ACK

    const auto got = a.read();
    CHECK(got.type() == memex::protocol::v1::NOTICE);
    CHECK(got.has_notice());
    CHECK(got.from() == memex::protocol::kNoticeSender);
    CHECK(got.to() == "alice");
    CHECK(got.msg_id() == msg_id);
    CHECK(got.notice().title() == "站内提醒");
    CHECK(got.notice().content() == "请查收周报");
    CHECK(got.notice().urgency() == memex::protocol::v1::Notice::NORMAL);
    CHECK(got.notice().jump_url().empty());

    a.send(make_ack(msg_id));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(store.offline_count("alice") == 0); // ACK 清队
  }

  // ② 群扇出：bob 离线入队、alice 在线即投；紧急级别随信封；跳转携带
  {
    auto r = post_json(wh_port, "whk_group",
                       R"({"title":"紧急通知","content":"机房割接",)"
                       R"("urgency":"urgent","jump_url":"https://oa.local/456"})");
    CHECK(r.status == 200);
    CHECK(r.body["recipients"] == 2); // 群成员 alice+bob 全员
    const std::string group_msg_id = r.body["msg_id"].get<std::string>();
    CHECK(store.offline_count("bob") == 1); // 离线成员入队
    // 群消息归档一次，to=群键
    const auto archived =
        store.search_messages({"", "机房割接", 0, 0, 50});
    CHECK(archived.size() == 1);
    CHECK(archived[0].msg_id == group_msg_id);
    CHECK(archived[0].to_account == "group:" + std::to_string(gid));
    CHECK(archived[0].type == static_cast<int>(memex::protocol::v1::NOTICE));
    // 归档正文＝客户端同源 compose（标题：正文 跳转）
    CHECK(archived[0].text ==
          memex::protocol::compose_notice_text(
              "紧急通知", "机房割接", "https://oa.local/456"));

    asio::io_context io;
    TestClient a(io, server.port());
    a.send(make_login("alice", "pa-w", "pc-alice2"));
    CHECK(a.read().login_result().ok());
    // alice 重新登录 → 群通知经离线队列补投（POST 时其会话已断）
    const auto got = a.read();
    CHECK(got.type() == memex::protocol::v1::NOTICE);
    CHECK(got.to() == "group:" + std::to_string(gid));
    CHECK(got.notice().urgency() == memex::protocol::v1::Notice::URGENT);
    CHECK(got.notice().jump_url() == "https://oa.local/456");
    a.send(make_ack(got.msg_id()));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(store.offline_count("alice") == 0);
    CHECK(store.offline_count("bob") == 1); // bob 未 ACK 仍在队
  }

  // ③ 离线补投：bob 登录即补投（同 msg_id），ACK 后清队
  {
    asio::io_context io;
    TestClient b(io, server.port());
    b.send(make_login("bob", "pb-w", "pc-bob"));
    CHECK(b.read().login_result().ok());
    const auto got = b.read();
    CHECK(got.type() == memex::protocol::v1::NOTICE);
    CHECK(got.has_notice());
    CHECK(got.notice().title() == "紧急通知");
    b.send(make_ack(got.msg_id()));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(store.offline_count("bob") == 0);
  }

  // ④ 前后对账：成功投递的通知全量归档 type=NOTICE——
  //    校验面 happy-path 1 条（carol）＋投递面个人 1 条＋群 1 条 ＝ 3；
  //    一切 4xx 失败请求都不产生归档行。
  {
    const auto all = store.search_messages({"", "", 0, 0, 200});
    int notice_rows = 0;
    for (const auto& m : all) {
      if (m.type == static_cast<int>(memex::protocol::v1::NOTICE)) {
        ++notice_rows;
      }
    }
    CHECK(notice_rows == 3);
  }
}

} // namespace

int main() {
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    CHECK(store.create_account("alice", "pa-w", "Alice"));
    CHECK(store.create_account("bob", "pb-w", "Bob"));
    CHECK(store.create_account("carol", "pc-w", "Carol"));

    asio::io_context io;
    CollabServer server(io, store, 0);
    WebhookServer wh(io, server, 0);
    server.start_accept();
    wh.start_accept();
    std::thread io_thread([&] { io.run(); });

    test_http_validation(store, wh.port());
    test_delivery_pipeline(store, server, wh.port());

    io.stop();
    io_thread.join();
  }

  if (g_failures == 0) {
    std::cout << "webhook tests: all passed\n";
    return 0;
  }
  std::cerr << "webhook tests: " << g_failures << " failure(s)\n";
  return 1;
}
