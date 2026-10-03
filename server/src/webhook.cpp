#include "webhook.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "session.hpp" // deliver_frame 在 Session 定义内

namespace memex::server {

namespace {

using json = nlohmann::json;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxHead = 8 * 1024;  // 请求头上限（防呆，非安全边界）
constexpr std::size_t kMaxBody = 64 * 1024; // JSON body 上限

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
  default: return "Internal Server Error";
  }
}

// 单连接 HTTP 会话：读到 \r\n\r\n 解析请求行与头，按 Content-Length 读满
// body，鉴权＋JSON 校验后回写 JSON 响应并关闭（Connection: close，无 keep-alive）。
class HttpConn : public std::enable_shared_from_this<HttpConn> {
public:
  HttpConn(tcp::socket sock, CollabServer& server)
      : sock_(std::move(sock)), server_(server) {}

  void start() { do_read(); }

private:
  void do_read() {
    auto self = shared_from_this();
    sock_.async_read_some(asio::buffer(chunk_),
                          [self](std::error_code ec, std::size_t n) {
                            if (ec) return; // 对端断开：静默收场
                            self->buf_.append(self->chunk_.data(), n);
                            self->on_data();
                          });
  }

  void on_data() {
    const std::size_t head_end = buf_.find("\r\n\r\n");
    if (head_end == std::string::npos) {
      if (buf_.size() > kMaxHead) {
        respond(400, {{"ok", false}, {"error", "请求头过长"}});
        return;
      }
      do_read();
      return;
    }
    if (!head_parsed_) {
      if (!parse_head(head_end)) return; // 已 respond（非法请求）
      head_parsed_ = true;
      if (body_too_large_) {
        respond(413, {{"ok", false}, {"error", "body 超过 64KB 上限"}});
        return;
      }
      // Expect: 100-continue（curl 对大 body 会带）：先回继续再等 body
      if (expect_continue_) {
        write_raw(std::make_shared<const std::string>(
                      "HTTP/1.1 100 Continue\r\n\r\n"),
                  /*close_when_done=*/false, [this](std::error_code ec) {
                    if (!ec) do_read();
                  });
        return;
      }
    }
    const std::size_t body_start = head_end + 4;
    if (buf_.size() < body_start + content_length_) {
      do_read();
      return;
    }
    process(buf_.substr(body_start, content_length_));
  }

  // 解析请求行＋头；非法即 respond 并返回 false。
  bool parse_head(std::size_t head_end) {
    const std::string head = buf_.substr(0, head_end);
    std::istringstream in(head);
    std::string method, uri, version;
    if (!(in >> method >> uri >> version)) {
      respond(400, {{"ok", false}, {"error", "请求行无法解析"}});
      return false;
    }
    method_ = method;
    // 请求头逐行：只取本协议需要的两个
    std::string line;
    std::getline(in, line); // 请求行余部
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = line.substr(0, colon);
      for (auto& c : key) c = static_cast<char>(std::tolower(c));
      // 值去前导空白（"Content-Length: 51" 冒号后有空格——RFC 允许）
      std::string val = line.substr(colon + 1);
      const auto vb = val.find_first_not_of(" \t");
      val = vb == std::string::npos ? std::string{} : val.substr(vb);
      if (key == "content-length") {
        seen_content_length_ = true;
        if (val.empty() ||
            val.find_first_not_of("0123456789") != std::string::npos) {
          content_length_ = static_cast<std::size_t>(-1); // 非法数值
        } else {
          content_length_ = static_cast<std::size_t>(
              std::strtoull(val.c_str(), nullptr, 10));
        }
      } else if (key == "expect" &&
                 val.find("100-continue") != std::string::npos) {
        expect_continue_ = true;
      }
    }
    if (!seen_content_length_ ||
        content_length_ == static_cast<std::size_t>(-1)) {
      respond(400, {{"ok", false}, {"error", "Content-Length 缺失或非法"}});
      return false;
    }
    if (content_length_ > kMaxBody) {
      body_too_large_ = true;
      return true; // 交由调用方回 413
    }
    uri_ = uri;
    return true;
  }

  // 请求已读满：鉴权 → JSON 校验 → 投递 → 回写。
  void process(const std::string& body) {
    // —— 方法与路径（POST /hook/<token>）——
    if (method_ != "POST") {
      respond(405, {{"ok", false}, {"error", "方法不支持：请用 POST"}});
      return;
    }
    const std::string prefix = "/hook/";
    if (uri_.rfind(prefix, 0) != 0) {
      respond(404, {{"ok", false}, {"error", "路径不存在（应为 /hook/<token>）"}});
      return;
    }
    std::string token = uri_.substr(prefix.size());
    if (const auto q = token.find('?'); q != std::string::npos) {
      token.resize(q);
    }

    // —— token 鉴权（sha256 摘要比对；无效或吊销一律 401）——
    const auto row =
        server_.store().webhook_by_token(sha256_hex(token));
    if (!row) {
      respond(401, {{"ok", false}, {"error", "token 无效或已吊销"}});
      return;
    }

    // —— JSON payload ——
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object()) {
      respond(400, {{"ok", false}, {"error", "请求体应为 JSON 对象"}});
      return;
    }
    for (const char* key : {"title", "content"}) {
      if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
        respond(400,
                {{"ok", false},
                 {"error", std::string("缺少或为空的字段：") + key}});
        return;
      }
    }
    const std::string title = j["title"].get<std::string>();
    const std::string content = j["content"].get<std::string>();

    std::string target = row->target; // 默认用 webhook 绑定目标
    if (j.contains("target")) {
      if (!j["target"].is_string()) {
        respond(400, {{"ok", false}, {"error", "target 应为字符串"}});
        return;
      }
      const std::string req = j["target"].get<std::string>();
      if (req != row->target) {
        respond(403, {{"ok", false},
                      {"error", "payload target 与 webhook 绑定目标不一致"}});
        return;
      }
      target = req;
    }

    int urgency = 1; // 默认普通
    if (j.contains("urgency")) {
      if (!j["urgency"].is_string()) {
        respond(400, {{"ok", false}, {"error", "urgency 应为字符串"}});
        return;
      }
      const std::string u = j["urgency"].get<std::string>();
      if (u == "normal") urgency = 1;
      else if (u == "important") urgency = 2;
      else if (u == "urgent") urgency = 3;
      else {
        respond(400, {{"ok", false},
                      {"error", "urgency 须为 normal|important|urgent"}});
        return;
      }
    }
    std::string jump;
    if (j.contains("jump_url")) {
      if (!j["jump_url"].is_string()) {
        respond(400, {{"ok", false}, {"error", "jump_url 应为字符串"}});
        return;
      }
      jump = j["jump_url"].get<std::string>();
    }

    // —— 投递（目标校验／入队／归档／扇出在 deliver_notice 内）——
    const NoticeDelivery d =
        deliver_notice(server_, target, title, content, urgency, jump);
    std::cout << "[MEMEX] webhook " << d.http_status << " target=" << target
              << " urgency=" << urgency
              << (d.ok ? " msg_id=" + d.msg_id : " error=" + d.error)
              << std::endl;
    if (!d.ok) {
      respond(d.http_status, {{"ok", false}, {"error", d.error}});
      return;
    }
    respond(200, {{"ok", true},
                  {"msg_id", d.msg_id},
                  {"recipients", d.recipients}});
  }

  void respond(int status, const json& body) {
    const std::string payload = body.dump();
    std::ostringstream head;
    head << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n"
         << "Content-Type: application/json; charset=utf-8\r\n"
         << "Content-Length: " << payload.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << payload;
    write_raw(std::make_shared<const std::string>(head.str()),
              /*close_when_done=*/true, [this](std::error_code) {});
  }

  // 写出一段字节：字节堆上持有（async_write 完成前不可失效），完成后再按需
  // 关连接（100-continue 中间响应不关）。sock 由本对象 shared_ptr 兜底。
  void write_raw(std::shared_ptr<const std::string> bytes,
                 bool close_when_done,
                 std::function<void(std::error_code)> done) {
    auto self = shared_from_this();
    asio::async_write(sock_, asio::buffer(*bytes),
                      [self, bytes, close_when_done,
                       done](std::error_code ec, std::size_t) {
                        if (close_when_done) {
                          std::error_code ignore;
                          self->sock_.shutdown(tcp::socket::shutdown_both,
                                               ignore);
                          self->sock_.close(ignore);
                        }
                        done(ec);
                      });
  }

  tcp::socket sock_;
  CollabServer& server_;
  std::array<char, 4096> chunk_{};
  std::string buf_;
  bool head_parsed_{false};
  bool body_too_large_{false};
  bool expect_continue_{false};
  bool seen_content_length_{false};
  std::string method_;
  std::string uri_;
  std::size_t content_length_{0};
};

} // namespace

NoticeDelivery deliver_notice(CollabServer& server, const std::string& target,
                              const std::string& title,
                              const std::string& content, int urgency,
                              const std::string& jump_url) {
  using Message = memex::protocol::Message;
  NoticeDelivery r;

  // —— 目标校验：群须存在，个人账号须已建 ——
  ServerStore& store = server.store();
  const bool is_group = target.rfind("group:", 0) == 0;
  std::vector<std::string> recipients;
  if (is_group) {
    const auto gid = static_cast<std::uint64_t>(
        std::strtoull(target.c_str() + 6, nullptr, 10));
    const auto info = store.group_info(gid);
    if (!info) {
      r.http_status = 404;
      r.error = "目标群不存在：" + target;
      return r;
    }
    recipients = store.group_members(gid); // 群通知发全体成员（含群主）
    if (recipients.empty()) {
      r.http_status = 404;
      r.error = "目标群无成员：" + target;
      return r;
    }
  } else {
    if (!store.find_account(target)) {
      r.http_status = 404;
      r.error = "目标账号不存在：" + target;
      return r;
    }
    recipients.push_back(target);
  }

  // —— 组 NOTICE 信封（服务端生成：无会话 seq，msg_id 以时间戳＋随机盐
  // 派生，同毫秒并发投递也不撞）——
  const std::int64_t ts = now_ms();
  const std::string msg_id = sha256_hex(
      std::string(memex::protocol::kNoticeSender) + ":" + target + ":" +
      std::to_string(ts) + ":" + random_salt_hex());
  Message out;
  out.set_type(memex::protocol::v1::NOTICE);
  out.set_from(memex::protocol::kNoticeSender);
  out.set_to(target);
  out.set_ts_ms(ts);
  out.set_msg_id(msg_id);
  auto* n = out.mutable_notice();
  n->set_title(title);
  n->set_content(content);
  n->set_urgency(static_cast<memex::protocol::v1::Notice::Urgency>(urgency));
  n->set_jump_url(jump_url);
  const std::string blob = out.SerializeAsString();

  for (const auto& to : recipients) store.queue_offline(msg_id, to, blob);
  // T2.3 全量归档（正文用与客户端同源的 compose_notice_text，前后对账一致）
  store.store_message(msg_id, memex::protocol::kNoticeSender, target,
                      static_cast<int>(memex::protocol::v1::NOTICE),
                      memex::protocol::compose_notice_text(title, content,
                                                           jump_url),
                      ts);
  for (const auto& to : recipients) {
    for (const auto& s : server.online_sessions(to)) s->deliver_frame(blob);
  }
  // T4.5 常用联系人最近刷新（镜像 TEXT 投递面：先 touch 再向在线收件人
  // 回推全量——「通知」会话即时进列表；整表替换，重复推送无害）
  store.fav_touch(memex::protocol::kNoticeSender, target, ts);
  if (is_group) {
    for (const auto& to : recipients) store.fav_touch(to, target, ts);
  } else {
    store.fav_touch(target, memex::protocol::kNoticeSender, ts);
  }
  for (const auto& to : recipients) {
    for (const auto& s : server.online_sessions(to)) {
      Message fo;
      fo.set_type(memex::protocol::v1::FAV_DATA);
      fo.set_from("server");
      fo.set_to(to);
      fo.set_ts_ms(ts);
      auto* d = fo.mutable_fav_data();
      for (const auto& f : store.fav_list(to)) {
        auto* e = d->add_entries();
        e->set_peer(f.peer);
        e->set_starred(f.starred);
        e->set_last_ms(f.last_ms);
      }
      s->deliver_frame(fo.SerializeAsString());
    }
  }

  r.ok = true;
  r.http_status = 200;
  r.msg_id = msg_id;
  r.recipients = static_cast<int>(recipients.size());
  return r;
}

WebhookServer::WebhookServer(asio::io_context& io, CollabServer& server,
                             std::uint16_t port)
    : io_(io), server_(server),
      acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {}

std::uint16_t WebhookServer::port() const {
  return acceptor_.local_endpoint().port();
}

void WebhookServer::start_accept() {
  std::cout << "[MEMEX] Webhook 监听 0.0.0.0:" << port() << std::endl;
  do_accept();
}

void WebhookServer::do_accept() {
  acceptor_.async_accept(
      [this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (!ec) {
          std::make_shared<HttpConn>(std::move(socket), server_)->start();
        }
        do_accept();
      });
}

} // namespace memex::server
