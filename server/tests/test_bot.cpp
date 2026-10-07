// 机器人（bot）平台验收：Bearer 鉴权（缺失/错 token 401、禁用 403）、
// payload 校验（缺 target/text/msg_id 400、坏 JSON 400）、发送权限边界
// （幽灵群 404 先于非成员 403、加群后 200）、归档 from=bot:<name>（bot
// 消息同链路留痕）、updates 解码 TEXT（群扇出零改动达 bot 队列＋单聊
// 直达）、ack 清队幂等、发送者不收自己的消息（群发不回环）。
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

// 阻塞式测试客户端：帧收发（长度前缀 + Envelope）；自动跳过推送帧。
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

struct HttpResponse {
  int status{0};
  json body;
};

// 真实 HTTP 往返（独立 io_context，阻塞直到服务端 Connection: close 落 EOF；
// Bearer token 走 Authorization 头——/bot/* 鉴权面）。
HttpResponse http_request(std::uint16_t port, const std::string& method,
                          const std::string& path,
                          const std::string& bearer_token,
                          const std::string& body) {
  HttpResponse out;
  try {
    asio::io_context io;
    tcp::socket sock(io);
    sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    std::ostringstream req;
    req << method << ' ' << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "Content-Type: application/json\r\n";
    if (!bearer_token.empty()) {
      req << "Authorization: Bearer " << bearer_token << "\r\n";
    }
    // GET 不带 Content-Length（真 curl 形态——服务端按无 body 处理）
    if (!body.empty() || method != "GET") {
      req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n"
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

HttpResponse bot_post(std::uint16_t port, const std::string& token,
                      const std::string& path, const std::string& body) {
  return http_request(port, "POST", path, token, body);
}

HttpResponse bot_get(std::uint16_t port, const std::string& token,
                     const std::string& path) {
  return http_request(port, "GET", path, token, "");
}

// —— 校验面：鉴权／payload／权限边界（不投递成功） ——
void test_validation(ServerStore& store, std::uint16_t port) {
  const std::string ok_token = "bot_ok_token";
  CHECK(store.bot_add("echo", memex::server::sha256_hex(ok_token), "admin1",
                      now_ms()) > 0);

  // ① 鉴权：无 Authorization → 401；错 token → 401；GET updates 同口径
  auto r = bot_post(port, "", "/bot/send", R"({"target":"alice","text":"hi"})");
  CHECK(r.status == 401);
  r = bot_post(port, "bot_wrong", "/bot/send",
               R"({"target":"alice","text":"hi"})");
  CHECK(r.status == 401);
  r = bot_get(port, "bot_wrong", "/bot/updates");
  CHECK(r.status == 401);

  // ② payload：坏 JSON／非对象 → 400；缺 target／缺 text／空 text → 400
  r = bot_post(port, ok_token, "/bot/send", "not-json");
  CHECK(r.status == 400);
  r = bot_post(port, ok_token, "/bot/send", "[1]");
  CHECK(r.status == 400);
  r = bot_post(port, ok_token, "/bot/send", R"({"text":"hi"})");
  CHECK(r.status == 400);
  r = bot_post(port, ok_token, "/bot/send", R"({"target":"alice"})");
  CHECK(r.status == 400);
  r = bot_post(port, ok_token, "/bot/send",
               R"({"target":"alice","text":""})");
  CHECK(r.status == 400);

  // ③ ack 缺 msg_id → 400
  r = bot_post(port, ok_token, "/bot/ack", "{}");
  CHECK(r.status == 400);

  // ④ 方法错：updates 用 POST → 405；send 用 GET → 405；未知路径 → 404
  r = bot_post(port, ok_token, "/bot/updates", "");
  CHECK(r.status == 405);
  r = bot_get(port, ok_token, "/bot/send");
  CHECK(r.status == 405);
  r = bot_get(port, ok_token, "/bot/nope");
  CHECK(r.status == 405); // /bot/ 前缀内未知路径先按方法裁（POST-only 族）
  r = bot_post(port, ok_token, "/bot/nope", "{}");
  CHECK(r.status == 404);

  // ⑤ 禁用 → 403（收发两路同裁）；启用即恢复
  CHECK(store.bot_set_disabled("echo", true));
  r = bot_post(port, ok_token, "/bot/send",
               R"({"target":"alice","text":"hi"})");
  CHECK(r.status == 403);
  r = bot_get(port, ok_token, "/bot/updates");
  CHECK(r.status == 403);
  CHECK(store.bot_set_disabled("echo", false));
  r = bot_get(port, ok_token, "/bot/updates");
  CHECK(r.status == 200);

  // ⑥ 发送目标：幽灵账号 404（个人）；幽灵群 404 先于非成员 403
  r = bot_post(port, ok_token, "/bot/send",
               R"({"target":"nobody","text":"hi"})");
  CHECK(r.status == 404);
  r = bot_post(port, ok_token, "/bot/send",
               R"({"target":"group:424242","text":"hi"})");
  CHECK(r.status == 404);

  // ⑥′ POST 缺 Content-Length 头 → body 按空处理 → JSON 校验 400
  //    （不 500 不挂连接；curl -d 必带头，裸 POST 亦得体拒绝）
  {
    asio::io_context io;
    tcp::socket sock(io);
    sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    const std::string raw =
        "POST /bot/send HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer bot_ok_token\r\nConnection: close\r\n\r\n";
    asio::write(sock, asio::buffer(raw));
    std::string response;
    asio::error_code ec;
    asio::read(sock, asio::dynamic_buffer(response), ec);
    CHECK(response.rfind("HTTP/1.1 400", 0) == 0);
  }

  // ⑦ 非成员群发 → 403
  const auto gid = store.create_group("机器人演练群", "alice", {"alice", "bob"});
  CHECK(gid > 0);
  r = bot_post(port, ok_token, "/bot/send",
               R"({"target":")" + std::string("group:") + std::to_string(gid) +
                   R"(","text":"hi"})");
  CHECK(r.status == 403);

  // 失败请求零投递：离线队空、归档无 bot 行
  CHECK(store.offline_count("alice") == 0);
  CHECK(store.offline_count("bob") == 0);
  CHECK(store.offline_count("bot:echo") == 0);
  const auto archived = store.search_messages({"bot:echo", "", 0, 0, 50});
  CHECK(archived.empty());
}

// —— 收发全链：加群发送 200＋归档 bot 身份；TEXT 扇出/单聊直达 bot 队列；
//      updates 解码；ack 清队；发送者不收自己。 ——
void test_pipeline(ServerStore& store, CollabServer& server,
                   std::uint16_t port) {
  const std::string ok_token = "bot_ok_token";
  const auto gid = store.create_group("机器人演练群", "alice", {"alice", "bob"});
  CHECK(gid > 0);
  const std::string group_key = "group:" + std::to_string(gid);

  // ① 加群后群发 200：recipients=全员减发送者（alice+bob），归档
  //    from=bot:echo 同链路留痕；bot 自己不收自己的消息（防回环）
  CHECK(store.bot_join_group("echo", gid, now_ms()));
  auto r = bot_post(port, ok_token, "/bot/send",
                    R"({"target":")" + group_key + R"(","text":"构建完成 ✓"})");
  CHECK(r.status == 200);
  CHECK(r.body["ok"] == true);
  CHECK(r.body["recipients"] == 2);
  const std::string bot_msg_id = r.body["msg_id"].get<std::string>();
  const auto archived =
      store.search_messages({"", "构建完成", 0, 0, 50});
  CHECK(archived.size() == 1);
  CHECK(archived[0].from_account == "bot:echo");
  CHECK(archived[0].to_account == group_key);
  CHECK(store.offline_count("bot:echo") == 0); // 发送者不入自己队列

  // ② TEXT 群扇出零改动达 bot 队列：alice 登录发群消息，bot updates 拉到
  {
    asio::io_context io;
    TestClient a(io, server.port());
    a.send(make_login("alice", "pa-w", "pc-alice"));
    CHECK(a.read().login_result().ok());
    a.send(make_text("alice", group_key, 1, "大家好"));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    r = bot_get(port, ok_token, "/bot/updates");
    CHECK(r.status == 200);
    CHECK(r.body["updates"].is_array());
    // 恰 1 条：①中 bot 自己的群发不回环（发送者已排除），此处唯一来源
    // 是 alice 的群消息 TEXT
    CHECK(r.body["updates"].size() == 1);
    CHECK(r.body["updates"][0]["from"] == "alice");
    CHECK(r.body["updates"][0]["text"] == "大家好");
    CHECK(r.body["updates"][0]["to"] == group_key);
    CHECK(r.body["updates"][0]["ts_ms"] > 0);
    const std::string text_msg_id =
        r.body["updates"][0]["msg_id"].get<std::string>();

    // ③ ack 清队：ack 后再拉为空
    r = bot_post(port, ok_token, "/bot/ack",
                 R"({"msg_id":")" + text_msg_id + R"("})");
    CHECK(r.status == 200);
    CHECK(r.body["acked"] == true);
    r = bot_get(port, ok_token, "/bot/updates");
    CHECK(r.body["updates"].empty());
  }

  // ④ 单聊直达：alice 私聊 bot:echo（TEXT 单聊路径不校验账号存在）→
  //    updates 解出 from=alice to=bot:echo
  {
    asio::io_context io;
    TestClient a(io, server.port());
    a.send(make_login("alice", "pa-w", "pc-alice2"));
    CHECK(a.read().login_result().ok());
    a.send(make_text("alice", "bot:echo", 2, "在吗"));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    r = bot_get(port, ok_token, "/bot/updates");
    CHECK(r.status == 200);
    CHECK(r.body["updates"].size() == 1);
    CHECK(r.body["updates"][0]["from"] == "alice");
    CHECK(r.body["updates"][0]["to"] == "bot:echo");
    CHECK(r.body["updates"][0]["text"] == "在吗");
    const std::string dm_id = r.body["updates"][0]["msg_id"].get<std::string>();

    // ⑤ ack 清队：首次 acked=true；重复 ack 幂等仍 200（重放无害，
    //    ack_offline 恒真语义与 TEXT 离线补投 ACK 同源）
    r = bot_post(port, ok_token, "/bot/ack",
                 R"({"msg_id":")" + dm_id + R"("})");
    CHECK(r.status == 200);
    CHECK(r.body["acked"] == true);
    r = bot_post(port, ok_token, "/bot/ack",
                 R"({"msg_id":")" + dm_id + R"("})");
    CHECK(r.status == 200);
    r = bot_get(port, ok_token, "/bot/updates");
    CHECK(r.body["updates"].empty());
  }

  // ⑥ 前后对账：账号检索 bot:echo 恰 3 条——群发 from=bot:echo 一条＋
  //    单聊 to=bot:echo 一条＋所在群消息一条（检索含成员所在群的群消息，
  //    留痕口径：bot 参与的会话可对账），失败请求零归档
  {
    const auto all = store.search_messages({"bot:echo", "", 0, 0, 50});
    CHECK(all.size() == 3);
    int as_sender = 0;
    int as_receiver = 0;
    int in_group = 0;
    for (const auto& m : all) {
      if (m.from_account == "bot:echo" && m.msg_id == bot_msg_id) ++as_sender;
      if (m.to_account == "bot:echo") ++as_receiver;
      if (m.to_account == group_key && m.from_account == "alice") ++in_group;
    }
    CHECK(as_sender == 1);
    CHECK(as_receiver == 1);
    CHECK(in_group == 1);
    const auto dm = store.search_messages({"", "在吗", 0, 0, 50});
    CHECK(dm.size() == 1); // 单聊 TEXT 归档照常（to=bot:echo）
    CHECK(dm[0].to_account == "bot:echo");
  }
}

} // namespace

int main() {
  {
    memex::server::ServerStore store;
    CHECK(store.open(":memory:"));
    CHECK(store.create_account("alice", "pa-w", "Alice"));
    CHECK(store.create_account("bob", "pb-w", "Bob"));

    asio::io_context io;
    CollabServer server(io, store, 0);
    WebhookServer wh(io, server, 0);
    server.start_accept();
    wh.start_accept();
    std::thread io_thread([&] { io.run(); });

    test_validation(store, wh.port());
    test_pipeline(store, server, wh.port());

    io.stop();
    io_thread.join();
  }

  if (g_failures == 0) {
    std::cout << "bot tests: all passed\n";
    return 0;
  }
  std::cerr << "bot tests: " << g_failures << " failure(s)\n";
  return 1;
}
