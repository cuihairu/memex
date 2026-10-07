#include "model_gateway.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "cred.hpp"

namespace memex::server {

namespace {

using json = nlohmann::json;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxBody = 512 * 1024; // 请求体上限（对话 prompt 规模）
constexpr auto kUpstreamDeadline = std::chrono::seconds(30);

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const char* reason_phrase(int status) {
  switch (status) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 413: return "Payload Too Large";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  default:
    return status >= 500 ? "Internal Server Error"
                         : (status >= 400 ? "Bad Request" : "OK");
  }
}

// base_url → (host, port, prefix)。仅 http://；prefix 去尾 '/'。
bool parse_base_url(const std::string& base_url, std::string& host,
                    std::string& port, std::string& prefix,
                    std::string& why) {
  const std::string scheme = "http://";
  if (base_url.rfind(scheme, 0) != 0) {
    why = "base_url 须以 http:// 开头（内网口径）";
    return false;
  }
  std::string rest = base_url.substr(scheme.size());
  const auto slash = rest.find('/');
  std::string host_port =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  prefix = slash == std::string::npos ? "" : rest.substr(slash);
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  if (host_port.empty()) {
    why = "base_url 缺主机";
    return false;
  }
  if (host_port[0] == '[') { // IPv6 字面量 [::1]:8080
    const auto close = host_port.find(']');
    if (close == std::string::npos) {
      why = "base_url IPv6 形态非法";
      return false;
    }
    host = host_port.substr(1, close - 1);
    port = close + 2 < host_port.size() &&
                   host_port[close + 1] == ':'
               ? host_port.substr(close + 2)
               : "80";
    return true;
  }
  const auto colon = host_port.rfind(':');
  if (colon == std::string::npos) {
    host = host_port;
    port = "80";
  } else {
    host = host_port.substr(0, colon);
    port = host_port.substr(colon + 1);
  }
  return true;
}

// 网关连接：一次性 HTTP/1.1 会话（读满→处理→回写→关），语义与 webhook
// 的 HttpConn 同构，但处理在网关自己的 io_context 上跑（阻塞上游调用
// 不影响消息面）。
class GatewayConn : public std::enable_shared_from_this<GatewayConn> {
public:
  GatewayConn(tcp::socket sock, ServerStore& store)
      : sock_(std::move(sock)), store_(store) {}

  void start() { do_read(); }

private:
  void do_read() {
    auto self = shared_from_this();
    sock_.async_read_some(asio::buffer(chunk_),
                          [self](std::error_code ec, std::size_t n) {
                            if (ec) return;
                            self->buf_.append(self->chunk_.data(), n);
                            self->on_data();
                          });
  }

  void on_data() {
    const std::size_t head_end = buf_.find("\r\n\r\n");
    if (head_end == std::string::npos) {
      if (buf_.size() > 8 * 1024) {
        respond(400, {{"ok", false}, {"error", "请求头过长"}});
        return;
      }
      do_read();
      return;
    }
    if (!head_parsed_) {
      if (!parse_head(head_end)) return;
      head_parsed_ = true;
      if (buf_.size() < head_end + 4 + content_length_) {
        do_read();
        return;
      }
    }
    if (buf_.size() < head_end + 4 + content_length_) {
      do_read();
      return;
    }
    process(buf_.substr(head_end + 4, content_length_));
  }

  bool parse_head(std::size_t head_end) {
    const std::string head = buf_.substr(0, head_end);
    std::istringstream in(head);
    std::string method, uri;
    if (!(in >> method >> uri)) {
      respond(400, {{"ok", false}, {"error", "请求行无法解析"}});
      return false;
    }
    method_ = method;
    uri_ = uri;
    std::string line;
    std::getline(in, line); // 请求行余部
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = line.substr(0, colon);
      for (auto& c : key) c = static_cast<char>(std::tolower(c));
      std::string val = line.substr(colon + 1);
      const auto vb = val.find_first_not_of(" \t");
      val = vb == std::string::npos ? std::string{} : val.substr(vb);
      if (key == "content-length") {
        if (!val.empty() &&
            val.find_first_not_of("0123456789") == std::string::npos) {
          content_length_ = static_cast<std::size_t>(
              std::strtoull(val.c_str(), nullptr, 10));
        }
      } else if (key == "authorization") {
        authorization_ = val;
      }
    }
    if (content_length_ > kMaxBody) {
      respond(413, {{"ok", false}, {"error", "请求体超过 512KB 上限"}});
      return false;
    }
    return true;
  }

  void process(const std::string& body) {
    const std::string path = "/v1/chat/completions";
    if (uri_.rfind(path, 0) != 0) {
      respond(404, {{"ok", false},
                    {"error", "路径不存在（OpenAI 兼容入口：" + path + "）"}});
      return;
    }
    if (method_ != "POST") {
      respond(405, {{"ok", false}, {"error", "方法不支持：请用 POST"}});
      return;
    }

    // —— Bearer 鉴权：主体=bot（bot:<name>；disabled 同口径 403）——
    const std::string bearer = "Bearer ";
    std::string token;
    if (authorization_.rfind(bearer, 0) == 0) {
      token = authorization_.substr(bearer.size());
    }
    const auto bot = store_.bot_by_token(token.empty() ? std::string{}
                                                       : sha256_hex(token));
    if (!bot) {
      respond(401, {{"ok", false}, {"error", "token 缺失、无效或已删除"}});
      return;
    }
    if (bot->disabled) {
      respond(403, {{"ok", false}, {"error", "bot 已禁用"}});
      return;
    }
    const std::string caller = "bot:" + bot->name;

    // —— OpenAI 兼容 payload：{model?, messages, memex_archive_scope?}——
    json req;
    try {
      req = json::parse(body);
    } catch (const std::exception&) {
      respond(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!req.is_object() || !req.contains("messages") ||
        !req["messages"].is_array() || req["messages"].empty()) {
      respond(400, {{"ok", false}, {"error", "缺少 messages 数组（OpenAI 形态）"}});
      return;
    }
    std::string req_model;
    if (req.contains("model")) {
      if (!req["model"].is_string()) {
        respond(400, {{"ok", false}, {"error", "model 应为字符串"}});
        return;
      }
      req_model = req["model"].get<std::string>();
    }
    // 红线开关：请求声明携带归档数据（调用方自查口径；规则写死在本文件，
    // 不做成配置——归档数据只进本地模型）
    const bool archive_scope =
        req.contains("memex_archive_scope") &&
        req["memex_archive_scope"].is_boolean() &&
        req["memex_archive_scope"].get<bool>();

    // —— 候选端点：enabled；归档 scope 再裁 is_local；显式 model 命中
    // 端点名者提最前（仍在候选集内）——
    std::vector<ModelEndpointRow> candidates;
    for (auto& ep : store_.model_endpoints_list()) {
      if (!ep.enabled) continue;
      if (archive_scope && !ep.is_local) continue; // 红线：归档仅本地
      candidates.push_back(std::move(ep));
    }
    if (!req_model.empty()) {
      std::stable_partition(
          candidates.begin(), candidates.end(),
          [&](const ModelEndpointRow& ep) { return ep.name == req_model; });
    }
    if (candidates.empty()) {
      record_call(caller, "", req_model, archive_scope, body, 0, 503, 0);
      respond(503,
              {{"ok", false},
               {"error", archive_scope
                             ? "归档数据仅限本地模型：无可用本地端点（红线，"
                               "不外送）"
                             : "无可用模型端点"}});
      return;
    }

    // —— 逐候选降级：连接失败/超时/5xx → 下一个；4xx → 客户端错不降级
    //（如上下文超长），原样转发状态与错误体 ——
    std::string upstream_body; // 上游 messages（只发 model+messages）
    json up_req;
    up_req["messages"] = req["messages"];
    if (!req_model.empty()) up_req["model"] = req_model;
    upstream_body = up_req.dump();

    for (const auto& ep : candidates) {
      json sent = up_req;
      if (!ep.model.empty()) sent["model"] = ep.model;
      const auto t0 = std::chrono::steady_clock::now();
      const UpstreamResult r = http_post_json(
          ep.base_url, ep.api_key, sent.dump());
      const auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
      record_call(caller, ep.name, ep.model.empty() ? req_model : ep.model,
                  archive_scope, upstream_body,
                  r.status == 200
                      ? static_cast<int>(extract_completion_chars(r.body))
                      : 0,
                  r.status, latency);
      if (r.status >= 200 && r.status < 300) {
        std::cout << "[MEMEX] model-gateway " << caller << " → " << ep.name
                  << " 200 latency=" << latency << "ms"
                  << (archive_scope ? " (archive-scope)" : "") << std::endl;
        // OpenAI 形态透传（回包注入 memex_endpoint 便于排障；解析失败
        // 则原样回传不加工）
        json out = r.body.empty() ? json::object() : json::accept(r.body)
                                                         ? json::parse(r.body)
                                                         : json::object();
        if (out.is_object()) {
          out["memex_endpoint"] = ep.name;
          respond(200, out);
        } else {
          respond_raw(200, r.body);
        }
        return;
      }
      if (r.status >= 400 && r.status < 500 && r.status != 408 &&
          r.status != 429) {
        // 客户端错（含上游 4xx 语义）不降级
        std::cout << "[MEMEX] model-gateway " << caller << " → " << ep.name
                  << " " << r.status << "（4xx 不降级）" << std::endl;
        respond_raw(r.status, r.body.empty() ? "{}" : r.body);
        return;
      }
      std::cout << "[MEMEX] model-gateway " << caller << " → " << ep.name
                << (r.status == 0 ? " 连接失败/超时：" + r.error
                                  : " " + std::to_string(r.status) + "，降级")
                << std::endl;
    }

    respond(502, {{"ok", false}, {"error", "全部端点不可用（已按注册序降级）"}});
  }

  // 审计一行（成功/失败/降级每次尝试都记；正文不落库，只记元数据）
  void record_call(const std::string& caller, const std::string& endpoint,
                   const std::string& model, bool archive_scope,
                   const std::string& prompt, int completion_chars,
                   int status, std::int64_t latency) {
    ModelCallRow row;
    row.caller = caller;
    row.endpoint = endpoint;
    row.model = model;
    row.archive_scope = archive_scope;
    row.prompt_chars = static_cast<int>(prompt.size());
    row.completion_chars = completion_chars;
    row.status = status;
    row.latency_ms = latency;
    row.created_ms = now_ms();
    store_.model_call_add(row);
  }

  static std::size_t extract_completion_chars(const std::string& body) {
    json j = json::accept(body) ? json::parse(body) : json::object();
    if (!j.is_object()) return 0;
    if (j.contains("usage") && j["usage"].is_object() &&
        j["usage"].contains("completion_tokens") &&
        j["usage"]["completion_tokens"].is_number_integer()) {
      return static_cast<std::size_t>(
          j["usage"]["completion_tokens"].get<std::int64_t>());
    }
    return 0;
  }

  void respond(int status, const json& body) {
    respond_raw(status, body.dump());
  }

  void respond_raw(int status, const std::string& payload) {
    std::ostringstream head;
    head << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n"
         << "Content-Type: application/json; charset=utf-8\r\n"
         << "Content-Length: " << payload.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << payload;
    auto self = shared_from_this();
    auto bytes = std::make_shared<const std::string>(head.str());
    asio::async_write(sock_, asio::buffer(*bytes),
                      [self, bytes](std::error_code, std::size_t) {
                        std::error_code ignore;
                        self->sock_.shutdown(tcp::socket::shutdown_both,
                                             ignore);
                        self->sock_.close(ignore);
                      });
  }

  tcp::socket sock_;
  ServerStore& store_;
  std::array<char, 4096> chunk_{};
  std::string buf_;
  bool head_parsed_{false};
  std::string method_;
  std::string uri_;
  std::string authorization_;
  std::size_t content_length_{0};
};

} // namespace

UpstreamResult http_post_json(const std::string& base_url,
                              const std::string& api_key,
                              const std::string& json_body) {
  UpstreamResult out;
  std::string host, port, prefix, why;
  if (!parse_base_url(base_url, host, port, prefix, why)) {
    out.error = why;
    return out;
  }

  asio::io_context io;
  tcp::socket sock(io);
  asio::steady_timer deadline(io);
  bool finished = false; // 仅 io 线程内访问（run 期间串行）
  const auto finish = [&](std::string err) {
    if (finished) return;
    finished = true;
    out.error = std::move(err);
    deadline.cancel(); // 正常完成→定时器回调经 abort 出口
  };

  deadline.expires_after(kUpstreamDeadline);
  deadline.async_wait([&](std::error_code ec) {
    if (ec) return; // 已取消=正常完成路径
    if (!finished) {
      finished = true;
      out.error = "上游超时（30s 死线）";
      std::error_code ignore;
      sock.close(ignore);
    }
  });

  tcp::resolver res(io);
  res.async_resolve(host, port, [&](std::error_code ec,
                                    tcp::resolver::results_type results) {
    if (finished) return;
    if (ec) {
      finish("上游解析失败：" + host);
      return;
    }
    asio::async_connect(
        sock, results,
        [&](std::error_code ec, const tcp::endpoint&) {
          if (finished) return;
          if (ec) {
            finish("上游连接失败：" + host + ":" + port);
            return;
          }
          std::ostringstream req;
          req << "POST " << (prefix.empty() ? "" : prefix)
              << "/chat/completions HTTP/1.1\r\n"
              << "Host: " << host << "\r\n"
              << "Content-Type: application/json\r\n";
          if (!api_key.empty()) {
            req << "Authorization: Bearer " << api_key << "\r\n";
          }
          req << "Content-Length: " << json_body.size() << "\r\n"
              << "Connection: close\r\n\r\n"
              << json_body;
          const auto bytes = std::make_shared<const std::string>(req.str());
          asio::async_write(
              sock, asio::buffer(*bytes),
              [&, bytes](std::error_code ec, std::size_t) {
                if (finished) return;
                if (ec) {
                  finish("上游发送失败");
                  return;
                }
                asio::async_read(
                    sock, asio::dynamic_buffer(out.body),
                    [&](std::error_code ec, std::size_t) {
                      if (finished) return;
                      if (ec && ec != asio::error::eof) {
                        finish("上游读取失败");
                        return;
                      }
                      const auto head_end = out.body.find("\r\n\r\n");
                      if (head_end == std::string::npos) {
                        finish("上游响应无头");
                        return;
                      }
                      const std::string head = out.body.substr(0, head_end);
                      out.status = std::atoi(head.c_str() + strlen("HTTP/1.1 "));
                      out.body = out.body.substr(head_end + 4);
                      finish("");
                    });
              });
        });
  });
  io.run();
  if (!finished) out.error = "上游调用异常中止";
  return out;
}

ModelGatewayServer::ModelGatewayServer(std::uint16_t port,
                                       const std::string& db_path)
    : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {
  if (!store_.open(db_path)) {
    throw std::runtime_error("模型网关本地库打开失败：" + db_path);
  }
}

ModelGatewayServer::~ModelGatewayServer() { store_.close(); }

std::uint16_t ModelGatewayServer::port() const {
  return acceptor_.local_endpoint().port();
}

void ModelGatewayServer::start_accept() {
  std::cout << "[MEMEX] 模型网关监听 0.0.0.0:" << port()
            << "（OpenAI 兼容 /v1/chat/completions）" << std::endl;
  do_accept();
}

void ModelGatewayServer::do_accept() {
  acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
    if (!ec) {
      std::make_shared<GatewayConn>(std::move(socket), store_)->start();
    }
    do_accept();
  });
}

void ModelGatewayServer::run() { io_.run(); }

void ModelGatewayServer::stop() { io_.stop(); }

} // namespace memex::server
