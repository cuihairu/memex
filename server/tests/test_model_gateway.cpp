// 模型网关（平台三期）验收：Bearer 鉴权（bot token／disabled 403）、
// OpenAI 形态校验（缺 messages 400）、端点路由与降级（注册序、5xx 降级、
// 4xx 不降级、全不可用 502）、归档数据红线（archive scope 只路由本地
// 端点——无本地端点 503 且请求不出门，外部假上游零命中）、显式 model
// 路由、调用审计（一次上游尝试一行：降级两行、正文不落库、时延留痕）。
// 假上游=测试内阻塞 acceptor 线程（端口 0）；网关=真 ModelGatewayServer
//（独立线程）；两连接开同一临时文件库。
#include <asio.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

#include <unistd.h> // getpid（临时库目录唯一化）

#include "cred.hpp"
#include "model_gateway.hpp"
#include "store.hpp"

using json = nlohmann::json;
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

struct HttpResponse {
  int status{0};
  json body;
};

HttpResponse http_request(std::uint16_t port, const std::string& method,
                          const std::string& path,
                          const std::string& bearer_token,
                          const std::string& body) {
  HttpResponse out;
  try {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"),
                                         port));
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
    req << "Connection: close\r\n\r\n"
        << body;
    const std::string req_bytes = req.str();
    asio::write(sock, asio::buffer(req_bytes));
    std::string response;
    asio::error_code ec;
    asio::read(sock, asio::dynamic_buffer(response), ec);
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

// 假上游：阻塞 acceptor（独立线程，count 个请求后收摊）。每个请求回
// status_code + JSON 体（体里回显收到的 model 便于断言路由）。崩溃计数
// hit_counter 原子递增（红线腿断言「请求不出门」）。
class FakeUpstream {
public:
  FakeUpstream(int status_code, std::string name, int* hit_counter)
      : status_code_(status_code), name_(std::move(name)),
        hit_counter_(hit_counter) {
    acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(
        io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
  }

  std::uint16_t port() const { return acceptor_->local_endpoint().port(); }

  std::string base_url() const {
    return "http://127.0.0.1:" + std::to_string(port());
  }

  void serve(int count) {
    thread_ = std::thread([this, count] {
      for (int i = 0; i < count; ++i) {
        asio::ip::tcp::socket sock(io_);
        std::error_code ec;
        acceptor_->accept(sock, ec);
        if (ec) break;
        if (hit_counter_) ++(*hit_counter_);
        std::string buf;
        // 读头（到 \r\n\r\n）再按 content-length 读 body
        for (;;) {
          std::array<char, 4096> chunk{};
          const std::size_t n = sock.read_some(asio::buffer(chunk), ec);
          if (ec) break;
          buf.append(chunk.data(), n);
          const auto head_end = buf.find("\r\n\r\n");
          if (head_end == std::string::npos) continue;
          std::size_t content_length = 0;
          const std::string lower_head = [&] {
            std::string h = buf.substr(0, head_end);
            for (auto& c : h) c = static_cast<char>(std::tolower(c));
            return h;
          }();
          const auto cl = lower_head.find("content-length:");
          if (cl != std::string::npos) {
            content_length = static_cast<std::size_t>(
                std::strtoull(buf.c_str() + cl + 15, nullptr, 10));
          }
          if (buf.size() >= head_end + 4 + content_length) break;
        }
        // 回显请求里的 model（断言路由命中）
        std::string model_echo;
        const auto body_start = buf.find("\r\n\r\n");
        if (body_start != std::string::npos) {
          const auto body = buf.substr(body_start + 4);
          json j;
          if (json::accept(body)) j = json::parse(body);
          if (j.is_object() && j.contains("model")) {
            model_echo = j["model"].get<std::string>();
          }
        }
        json out;
        out["choices"] = json::array({json::object(
            {{"message",
              json::object({{"role", "assistant"},
                            {"content", "答复来自 " + name_ +
                                            (model_echo.empty()
                                                 ? ""
                                                 : "（模型 " + model_echo + "）")}})}})});
        const std::string payload = out.dump();
        std::ostringstream head;
        head << "HTTP/1.1 " << status_code_ << " X\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << payload.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << payload;
        const std::string resp = head.str();
        asio::write(sock, asio::buffer(resp));
        std::error_code ignore;
        sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
        sock.close(ignore);
      }
    });
  }

  void join() {
    if (thread_.joinable()) thread_.join();
  }

private:
  asio::io_context io_;
  std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
  int status_code_;
  std::string name_;
  int* hit_counter_;
  std::thread thread_;
};

} // namespace

int main() {
  // 临时文件库（网关与断言面两连接同开）
  const std::string dir =
      (std::filesystem::temp_directory_path() /
       ("memex-gw-test-" + std::to_string(::getpid())))
          .string();
  std::filesystem::create_directories(dir);
  const std::string db_path = dir + "/gw.db";

  {
    ServerStore store;
    CHECK(store.open(db_path));
    CHECK(store.create_account("admin1", "pa-w", "Admin"));
    CHECK(store.bot_add("assistant", memex::server::sha256_hex("gw_tok"),
                        "admin1", now_ms()) > 0);

    memex::server::ModelGatewayServer gw(0, db_path);
    CHECK(gw.port() > 0);
    gw.start_accept();
    std::thread gw_thread([&gw] { gw.run(); });
    const std::uint16_t port = gw.port();

    // ① 鉴权：无 Bearer／错 token 401；坏路径 404；GET 405
    auto r = http_request(port, "POST", "/v1/chat/completions", "",
                          R"({"messages":[{"role":"user","content":"hi"}]})");
    CHECK(r.status == 401);
    r = http_request(port, "POST", "/v1/chat/completions", "wrong",
                     R"({"messages":[]})");
    CHECK(r.status == 401);
    r = http_request(port, "POST", "/v1/nope", "gw_tok", "{}");
    CHECK(r.status == 404);
    r = http_request(port, "GET", "/v1/chat/completions", "gw_tok", "");
    CHECK(r.status == 405);

    // ② 形态：坏 JSON／缺 messages／messages 非数组 400
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok", "not-json");
    CHECK(r.status == 400);
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok", "{}");
    CHECK(r.status == 400);
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok",
                     R"({"messages":"x"})");
    CHECK(r.status == 400);

    // ③ 禁用 403（网关与 bot 同口径）；启用恢复
    CHECK(store.bot_set_disabled("assistant", true));
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok",
                     R"({"messages":[{"role":"user","content":"hi"}]})");
    CHECK(r.status == 403);
    CHECK(store.bot_set_disabled("assistant", false));

    // ④ 无端点 503
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok",
                     R"({"messages":[{"role":"user","content":"hi"}]})");
    CHECK(r.status == 503);

    // ⑤ 归档红线：只登记外部端点，archive scope → 503 且外部假上游零命中
    int external_hits = 0;
    FakeUpstream external(200, "cloud-a", &external_hits);
    external.serve(1); // 恰收 1 个连接（红线腿 0 命中＋非 scope 腿 1 命中）
    CHECK(store.model_endpoint_add(
              [&] {
                memex::server::ModelEndpointRow e;
                e.name = "cloud-a";
                e.base_url = external.base_url();
                e.created_by = "admin1";
                e.created_ms = now_ms();
                return e;
              }()) > 0);
    r = http_request(
        port, "POST", "/v1/chat/completions", "gw_tok",
        R"({"model":"cloud-a","messages":[{"role":"user","content":"总结归档"}],)"
        R"("memex_archive_scope":true})");
    CHECK(r.status == 503);
    CHECK(r.body.contains("error"));
    CHECK(std::string(r.body["error"].get<std::string>())
              .find("本地") != std::string::npos);
    CHECK(external_hits == 0); // 请求不出门
    // 非 scope 请求照常路由外部端点（顺带证明 ⑤ 的 503 红线不是端点坏）
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok",
                     R"({"messages":[{"role":"user","content":"你好"}]})");
    CHECK(r.status == 200);
    CHECK(r.body["choices"][0]["message"]["content"] == "答复来自 cloud-a");
    CHECK(r.body["memex_endpoint"] == "cloud-a");
    external.join();
    CHECK(external_hits == 1);
    // cloud-a 假上游已收摊：从台账摘除，防后续腿按注册序撞上死监听
    // （连接进 backlog 无应答 → 30s 死线，测试无谓变慢）
    CHECK(store.model_endpoint_remove("cloud-a"));

    // ⑥ 降级：第一端点 5xx → 第二端点接住；审计两行（一行 500 一行 200）
    FakeUpstream broken(500, "cloud-broken", nullptr);
    broken.serve(1);
    FakeUpstream healthy(200, "cloud-healthy", nullptr);
    healthy.serve(1);
    CHECK(store.model_endpoint_add(
              [&] {
                memex::server::ModelEndpointRow e;
                e.name = "broken";
                e.base_url = broken.base_url();
                e.created_by = "admin1";
                e.created_ms = now_ms();
                return e;
              }()) > 0);
    CHECK(store.model_endpoint_add(
              [&] {
                memex::server::ModelEndpointRow e;
                e.name = "healthy";
                e.base_url = healthy.base_url();
                e.created_by = "admin1";
                e.created_ms = now_ms();
                return e;
              }()) > 0);
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok",
                     R"({"messages":[{"role":"user","content":"hi"}]})");
    CHECK(r.status == 200);
    CHECK(r.body["memex_endpoint"] == "healthy"); // 降级到第二
    broken.join();
    healthy.join();
    {
      const auto calls = store.model_calls_list(5);
      CHECK(calls.size() >= 2);
      CHECK(calls[0].endpoint == "healthy" && calls[0].status == 200);
      CHECK(calls[1].endpoint == "broken" && calls[1].status == 500);
      CHECK(calls[0].caller == "bot:assistant");
      CHECK(calls[0].latency_ms >= 0);
      // 正文不落库：审计行无正文列（结构保证）；模型名留痕
      CHECK(calls[0].model.empty()); // 未指定 model，端点也未配
    }

    // ⑦ 4xx 不降级：上游 400 → 原样转发 400（若降级到 healthy 会得 200，
    //    故 status==400 即证不降级）
    FakeUpstream bad_req(400, "cloud-400", nullptr);
    bad_req.serve(1);
    CHECK(store.model_endpoint_remove("broken"));
    CHECK(store.model_endpoint_add(
              [&] {
                memex::server::ModelEndpointRow e;
                e.name = "b4";
                e.base_url = bad_req.base_url();
                e.created_by = "admin1";
                e.created_ms = now_ms();
                return e;
              }()) > 0);
    // 注册序= id 序（b4 的 id 大于 healthy）——用显式 model 路由到 b4
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok",
                     R"({"model":"b4","messages":[{"role":"user","content":"x"}]})");
    CHECK(r.status == 400); // 4xx 原样转发（若降级到 healthy 会得 200）
    bad_req.join();
    {
      const auto calls = store.model_calls_list(3);
      CHECK(calls[0].endpoint == "b4" && calls[0].status == 400);
    }

    // ⑧ 显式 model 命中端点名提最前＋端点配了上游模型名则改写 model：
    //    用 healthy（未配 model）+ 新本地端点（配 model）验归档路由正路
    FakeUpstream local(200, "local-a", nullptr);
    local.serve(1);
    CHECK(store.model_endpoint_add(
              [&] {
                memex::server::ModelEndpointRow e;
                e.name = "local-a";
                e.base_url = local.base_url();
                e.model = "qwen-local";
                e.is_local = true;
                e.created_by = "admin1";
                e.created_ms = now_ms();
                return e;
              }()) > 0);
    r = http_request(
        port, "POST", "/v1/chat/completions", "gw_tok",
        R"({"messages":[{"role":"user","content":"归档检索"}],)"
        R"("memex_archive_scope":true})");
    CHECK(r.status == 200);
    CHECK(r.body["memex_endpoint"] == "local-a");
    // 上游收到的 model=端点配置的 qwen-local（假上游回显）
    CHECK(std::string(r.body["choices"][0]["message"]["content"])
              .find("qwen-local") != std::string::npos);
    local.join();
    {
      const auto calls = store.model_calls_list(2);
      CHECK(calls[0].endpoint == "local-a" && calls[0].status == 200);
      CHECK(calls[0].archive_scope == true); // 归档红线留痕
      CHECK(calls[0].model == "qwen-local");
    }

    // ⑨ 全端点不可用 502（连接拒绝＝端口没起；其余端点先停用——防降级
    //    链撞上已收摊假上游的 backlog 监听拖 30s 死线）
    CHECK(store.model_endpoint_set_enabled("healthy", false));
    CHECK(store.model_endpoint_set_enabled("b4", false));
    CHECK(store.model_endpoint_set_enabled("local-a", false));
    CHECK(store.model_endpoint_add(
              [&] {
                memex::server::ModelEndpointRow e;
                e.name = "dead";
                e.base_url = "http://127.0.0.1:1"; // 不可能连上
                e.created_by = "admin1";
                e.created_ms = now_ms();
                return e;
              }()) > 0);
    r = http_request(port, "POST", "/v1/chat/completions", "gw_tok",
                     R"({"model":"dead","messages":[{"role":"user","content":"x"}]})");
    CHECK(r.status == 502);
    {
      const auto calls = store.model_calls_list(1);
      CHECK(calls[0].endpoint == "dead");
      CHECK(calls[0].status == 0); // 连接失败＝status 0
    }

    gw.stop();
    gw_thread.join();
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  if (g_failures == 0) {
    std::cout << "model gateway tests: all passed\n";
    return 0;
  }
  std::cerr << "model gateway tests: " << g_failures << " failure(s)\n";
  return 1;
}
