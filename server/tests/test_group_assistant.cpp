// 群内智能助手（平台三期）验收：真 CollabServer＋WebhookServer＋模型网关
// ＋假上游＋真 Worker 全链——TEXT 群扇出自动入 bot 队列（零改动收信），
// 四指令分发（@助手 RAG 上下文注入＋archive_scope 红线随行／@纪要模板／
// @整理模板／@检索归档命中回群不过模型），非指令不回声，私聊回复发起人，
// 模型不可用回群错误提示，处理过 ack 不重处理。
#include <asio.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <unistd.h>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "group_assistant.hpp"
#include "model_gateway.hpp"
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

// 阻塞式测试客户端（同 test_webhook 骨架：自动跳过推送帧）
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
                                    const std::string& device) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::LOGIN);
  m.set_from(device);
  m.set_to("server");
  m.set_ts_ms(now_ms());
  auto* in = m.mutable_login();
  in->set_account(account);
  in->set_password(password);
  in->set_device_fingerprint(memex::server::sha256_hex(device));
  in->set_device_kind("desktop");
  in->set_device_name(device);
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

// 假上游：把收到的 system+user prompt 拼进回包 content（回显断言用）；
// 恰收 expect 个连接后收摊。
class FakeUpstream {
public:
  explicit FakeUpstream(std::string name) : name_(std::move(name)) {
    acceptor_ = std::make_unique<tcp::acceptor>(
        io_, tcp::endpoint(tcp::v4(), 0));
  }

  std::uint16_t port() const { return acceptor_->local_endpoint().port(); }
  std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port());
  }

  void serve(int expect) {
    thread_ = std::thread([this, expect] {
      for (int i = 0; i < expect; ++i) {
        tcp::socket sock(io_);
        std::error_code ec;
        acceptor_->accept(sock, ec);
        if (ec) break;
        std::string buf;
        std::size_t content_length = 0;
        for (;;) {
          std::array<char, 4096> chunk{};
          const std::size_t n = sock.read_some(asio::buffer(chunk), ec);
          if (ec) break;
          buf.append(chunk.data(), n);
          const auto head_end = buf.find("\r\n\r\n");
          if (head_end == std::string::npos) continue;
          const std::string lower = [&] {
            std::string h = buf.substr(0, head_end);
            for (auto& c : h) c = static_cast<char>(std::tolower(c));
            return h;
          }();
          const auto cl = lower.find("content-length:");
          if (cl != std::string::npos) {
            content_length = static_cast<std::size_t>(
                std::strtoull(buf.c_str() + cl + 15, nullptr, 10));
          }
          if (buf.size() >= head_end + 4 + content_length) break;
        }
        // 提取 messages 数组拼回显（system→user）；scope 不在此断言——
        // 网关发送体只含 model+messages（红线在路由期裁剪，不透传上游），
        // 归档红线由 model_calls 审计行留痕（见③）
        std::string echo;
        const auto body_start = buf.find("\r\n\r\n");
        if (body_start != std::string::npos) {
          const auto body = buf.substr(body_start + 4);
          json j;
          if (json::accept(body)) j = json::parse(body);
          if (j.is_object() && j.contains("messages") &&
              j["messages"].is_array()) {
            for (const auto& m : j["messages"]) {
              if (m.is_object() && m.contains("content") &&
                  m["content"].is_string()) {
                echo += m["content"].get<std::string>();
                echo += "\n---\n";
              }
            }
          }
        }
        int seq = 0;
        {
          std::lock_guard<std::mutex> lock(mu_);
          last_prompt_ = echo;
          seq = ++served_; // 回包带序号：③④⑤ 各等各的回包（内容全同会
                           // 被前一腿的存档残留瞬间满足、读到旧 prompt）
        }
        json out;
        out["choices"] = json::array({json::object(
            {{"message",
              json::object(
                  {{"role", "assistant"},
                   {"content",
                    "答复[" + name_ + "]#" + std::to_string(seq)}})}})});
        const std::string payload = out.dump();
        std::ostringstream head;
        head << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << payload.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << payload;
        const std::string resp = head.str();
        asio::write(sock, asio::buffer(resp));
        std::error_code ignore;
        sock.shutdown(tcp::socket::shutdown_both, ignore);
        sock.close(ignore);
      }
    });
  }

  std::string last_prompt() {
    std::lock_guard<std::mutex> lock(mu_);
    return last_prompt_;
  }

  void join() {
    if (thread_.joinable()) thread_.join();
  }

private:
  asio::io_context io_;
  std::unique_ptr<tcp::acceptor> acceptor_;
  std::string name_;
  std::thread thread_;
  std::mutex mu_;
  std::string last_prompt_;
  int served_{0}; // mu_ 下读写
};

// 等到条件成立（worker 轮询是异步的）
template <typename F>
bool wait_for(F&& cond, int timeout_ms = 5000) {
  for (int waited = 0; waited < timeout_ms; waited += 50) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return cond();
}

// 群里 alice 侧最近一条 NOTICE（bot 回复）的文本
std::string last_notice_text(ServerStore& store, const std::string& account,
                             const std::string& keyword) {
  const auto rows = store.search_messages({account, keyword, 0, 0, 5});
  for (const auto& r : rows) {
    if (r.type == static_cast<int>(memex::protocol::v1::NOTICE)) return r.text;
  }
  return "";
}

struct HttpResponse {
  int status{0};
  json body;
};

// 真实 HTTP 往返（同 test_bot 骨架：阻塞至服务端 Connection: close）
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
    if (!body.empty() || method != "GET") {
      req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "Connection: close\r\n\r\n" << body;
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
    }
  } catch (const std::exception& e) {
    std::cerr << "HTTP 请求异常：" << e.what() << '\n';
  }
  return out;
}

} // namespace

int main() {
  const std::string dir =
      (std::filesystem::temp_directory_path() /
       ("memex-ga-test-" + std::to_string(::getpid())))
          .string();
  std::filesystem::create_directories(dir);
  const std::string db_path = dir + "/ga.db";

  {
    ServerStore store;
    CHECK(store.open(db_path));
    CHECK(store.create_account("alice", "pa-w", "Alice"));
    CHECK(store.create_account("bob", "pb-w", "Bob"));
    CHECK(store.bot_add("assistant", memex::server::sha256_hex("ga_tok"),
                        "alice", now_ms()) > 0);
    const auto gid = store.create_group("项目群", "alice", {"alice", "bob"});
    CHECK(gid > 0);
    const std::string group_key = "group:" + std::to_string(gid);
    CHECK(store.bot_join_group("assistant", gid, now_ms()));
    // 预置归档（检索增强资料）：两条群消息（经真 TEXT 扇出会达 bot 队列——
    // 非指令会被 ack 跳过；归档已在）
    CHECK(store.store_message("seed-1", "alice", group_key,
                              static_cast<int>(memex::protocol::v1::TEXT),
                              "项目立项会定于周五", now_ms()));
    CHECK(store.store_message("seed-2", "bob", group_key,
                              static_cast<int>(memex::protocol::v1::TEXT),
                              "预算表已发群文件", now_ms()));

    asio::io_context io;
    CollabServer server(io, store, 0);
    WebhookServer webhook(io, server, 0);
    server.start_accept();
    webhook.start_accept();
    std::thread io_thread([&] { io.run(); });

    // 模型网关＋假上游（本地端点——archive 红线路由正路）
    FakeUpstream upstream("local-llm");
    upstream.serve(3); // @助手/@纪要/@整理 三腿
    memex::server::ModelGatewayServer gw(
        0, db_path);
    gw.start_accept();
    std::thread gw_thread([&gw] { gw.run(); });
    CHECK(store.model_endpoint_add(
              [&] {
                memex::server::ModelEndpointRow e;
                e.name = "local-llm";
                e.base_url = upstream.base_url();
                e.model = "qwen-local";
                e.is_local = true;
                e.created_by = "alice";
                e.created_ms = now_ms();
                return e;
              }()) > 0);

    // 真 Worker（轮询 60ms）
    memex::server::GroupAssistantWorker worker(
        "assistant", "ga_tok", webhook.port(),
        "http://127.0.0.1:" + std::to_string(gw.port()) +
            "/v1", // http_post_json 拼 {prefix}/chat/completions
        db_path,
        /*poll_ms=*/60);
    worker.start();

    // ① 非指令不回声：alice 发「大家好」→ bot 消费队列（ack 清队）但
    // 不回复。非指令不增 handled 计数、消费快过轮询——观察点=宽限数拍
    // 后队列空且 alice 侧零 NOTICE。
    {
      asio::io_context cio;
      TestClient a(cio, server.port());
      a.send(make_login("alice", "pa-w", "pc-a"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", group_key, 1, "大家好"));
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      CHECK(store.pending_offline("bot:assistant").empty()); // 已消费
      const auto rows = store.search_messages({"alice", "", 0, 0, 100});
      int notices = 0;
      for (const auto& r : rows) {
        if (r.type == static_cast<int>(memex::protocol::v1::NOTICE)) {
          ++notices;
        }
      }
      CHECK(notices == 0); // bot 未回声
    }

    // ② @检索：归档命中回群（不过模型——假上游零消耗）
    {
      asio::io_context cio;
      TestClient a(cio, server.port());
      a.send(make_login("alice", "pa-w", "pc-a2"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", group_key, 2, "@检索 立项"));
      CHECK(wait_for([&] {
        return store.search_messages({"alice", "命中", 0, 0, 5}).size() > 0;
      }));
      const std::string notice =
          last_notice_text(store, "alice", "命中");
      CHECK(notice.find("立项") != std::string::npos);
      CHECK(notice.find("项目立项会定于周五") != std::string::npos);
    }

    // ③ @助手：RAG 上下文＋问题进 prompt；archive_scope 红线随行；
    //    回答回群（bot:assistant 发言）
    {
      asio::io_context cio;
      TestClient a(cio, server.port());
      a.send(make_login("alice", "pa-w", "pc-a3"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", group_key, 3, "@助手 立项会是什么时候"));
      CHECK(wait_for([&] {
        return !last_notice_text(store, "alice", "答复[local-llm]#1")
                    .empty();
      }, 8000));
      const std::string prompt = upstream.last_prompt();
      CHECK(prompt.find("群聊资料") != std::string::npos);
      CHECK(prompt.find("项目立项会定于周五") != std::string::npos);
      CHECK(prompt.find("立项会是什么时候") != std::string::npos);
      // 归档红线：scope 在路由期裁剪（不透传上游），留痕见审计行——
      // bot:assistant 的调用命中本地端点且 archive_scope=1
      bool audited = false;
      for (const auto& c : store.model_calls_list(10)) {
        if (c.caller == "bot:assistant" && c.endpoint == "local-llm" &&
            c.archive_scope && c.status == 200) {
          audited = true;
        }
      }
      CHECK(audited);
    }

    // ④ @纪要：群最近消息进纪要模板
    {
      asio::io_context cio;
      TestClient a(cio, server.port());
      a.send(make_login("alice", "pa-w", "pc-a4"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", group_key, 4, "@纪要"));
      CHECK(wait_for([&] {
        return !last_notice_text(store, "alice", "答复[local-llm]#2")
                    .empty();
      }, 8000));
      const std::string prompt = upstream.last_prompt();
      CHECK(prompt.find("会议纪要") != std::string::npos);
      CHECK(prompt.find("预算表已发群文件") != std::string::npos);
    }

    // ⑤ @整理：文本进整理模板
    {
      asio::io_context cio;
      TestClient a(cio, server.port());
      a.send(make_login("alice", "pa-w", "pc-a5"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", group_key, 5, "@整理 做一个审批提醒功能"));
      CHECK(wait_for([&] {
        return !last_notice_text(store, "alice", "答复[local-llm]#3")
                    .empty();
      }, 8000));
      const std::string prompt = upstream.last_prompt();
      CHECK(prompt.find("结构化需求") != std::string::npos);
      CHECK(prompt.find("审批提醒功能") != std::string::npos);
      upstream.join(); // 恰三腿：@助手/@纪要/@整理（@检索不过模型）
      CHECK(wait_for([&] { return worker.handled() >= 4; })); // ②③④⑤
    }

    // ⑥ 私聊 bot：回复发起人（不回群）
    {
      asio::io_context cio;
      TestClient a(cio, server.port());
      a.send(make_login("alice", "pa-w", "pc-a6"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", "bot:assistant", 6, "@检索 私聊"));
      CHECK(wait_for([&] {
        return store.search_messages({"alice", "私聊", 0, 0, 20}).size() >= 2;
      }));
      // 回复 to=alice（个人）；无群键
      const auto rows = store.search_messages({"alice", "（检索）", 0, 0, 5});
      bool to_alice = false;
      for (const auto& r : rows) {
        if (r.to_account == "alice") to_alice = true;
      }
      CHECK(to_alice);
    }

    // ⑦ 模型不可用：网关还在但本地端点停用→红线 503→回群错误提示
    {
      CHECK(store.model_endpoint_set_enabled("local-llm", false));
      asio::io_context cio;
      TestClient a(cio, server.port());
      a.send(make_login("alice", "pa-w", "pc-a7"));
      CHECK(a.read().login_result().ok());
      a.send(make_text("alice", group_key, 7, "@助手 还在吗"));
      CHECK(wait_for([&] {
        const auto rows = store.search_messages({"alice", "（助手）", 0, 0, 5});
        for (const auto& r : rows) {
          if (r.text.find("本地") != std::string::npos ||
              r.text.find("不可用") != std::string::npos) {
            return true;
          }
        }
        return false;
      }, 8000));
      CHECK(store.model_endpoint_set_enabled("local-llm", true));
    }

    // ⑧ 他人 bot 群发（deliver_notice 入队=NOTICE 信封，非 TEXT）：助手
    // ack 丢弃——不积压（worker 只认 TEXT，留行即永不清）也不触发回复
    {
      CHECK(store.bot_add("relay", memex::server::sha256_hex("relay_tok"),
                          "alice", now_ms()) > 0);
      CHECK(store.bot_join_group("relay", gid, now_ms()));
      const std::size_t before =
          store.search_messages({"alice", "", 0, 0, 200}).size();
      const HttpResponse r = http_request(
          webhook.port(), "POST", "/bot/send", "relay_tok",
          R"({"target":")" + group_key + R"(","text":"转发一条通知"})");
      CHECK(r.status == 200);
      CHECK(wait_for([&] {
        return store.pending_offline("bot:assistant").empty();
      })); // NOTICE 信封被清，不积压
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      const std::size_t after =
          store.search_messages({"alice", "", 0, 0, 200}).size();
      CHECK(after == before + 1); // 只多 relay 的归档，助手未回复
    }

    // ⑨ 畸形信封（无 msg_id=ack 键缺失，正常链路由服务端派生必有）：
    // 不处理不回复——否则每轮重处理会刷群刷网关
    {
      memex::protocol::Message malformed = make_text("alice", group_key, 9,
                                                     "@检索 畸形");
      // 不设 msg_id（空=ack 键缺失）
      CHECK(store.queue_offline("",
                                "bot:assistant",
                                malformed.SerializeAsString()));
      const int handled_before = worker.handled();
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      CHECK(worker.handled() == handled_before); // 未消费处理
      int notices = 0;
      for (const auto& r : store.search_messages({"alice", "畸形", 0, 0, 50})) {
        if (r.type == static_cast<int>(memex::protocol::v1::NOTICE)) {
          ++notices;
        }
      }
      CHECK(notices == 0); // 无回复产生（行保留可见=畸形数据可对账）
      store.ack_offline("", "bot:assistant"); // 测试清理（删 0 行，无害）
    }

    worker.stop();
    gw.stop();
    gw_thread.join();
    io.stop();
    io_thread.join();
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  if (g_failures == 0) {
    std::cout << "group assistant tests: all passed\n";
    return 0;
  }
  std::cerr << "group assistant tests: " << g_failures << " failure(s)\n";
  return 1;
}
