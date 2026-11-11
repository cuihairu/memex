// 单连接 HTTP 会话骨架（LAY-003 拆分）：字节读写、头解析、
// 100-continue、响应写面；路由按域在 files_server/ 各模块。

#include "files_server/conn.hpp"

namespace memex::server {

void FileConn::do_read() {
  auto self = shared_from_this();
  sock_.async_read_some(
      asio::buffer(chunk_),
      [self](std::error_code ec, std::size_t n) {
        if (ec) return;
        self->buf_.append(self->chunk_.data(), n);
        self->on_data();
      });
}
void FileConn::on_data() {
  const std::size_t head_end = buf_.find("\r\n\r\n");
  if (head_end == std::string::npos) {
    if (buf_.size() > kMaxHead) {
      respond_json(400, {{"ok", false}, {"error", "请求头过长"}});
      return;
    }
    do_read();
    return;
  }
  if (!head_parsed_) {
    if (!parse_head(head_end)) return;
    head_parsed_ = true;
    if (content_length_ > body_cap_) {
      respond_json(413, {{"ok", false}, {"error", "body 超过上限"}});
      return;
    }
    if (expect_continue_) {
      write_raw(std::make_shared<const std::string>(
                    "HTTP/1.1 100 Continue\r\n\r\n"),
                false, [this](std::error_code ec) {
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
bool FileConn::parse_head(std::size_t head_end) {
  const std::string head = buf_.substr(0, head_end);
  std::istringstream in(head);
  std::string method, uri, version;
  if (!(in >> method >> uri >> version)) {
    respond_json(400, {{"ok", false}, {"error", "请求行无法解析"}});
    return false;
  }
  method_ = method;
  // path 与 query 拆分
  const std::size_t qpos = uri.find('?');
  path_ = qpos == std::string::npos ? uri : uri.substr(0, qpos);
  query_ = qpos == std::string::npos ? "" : uri.substr(qpos + 1);
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
      seen_content_length_ = true;
      if (val.empty() ||
          val.find_first_not_of("0123456789") != std::string::npos) {
        content_length_ = static_cast<std::size_t>(-1);
      } else {
        content_length_ = static_cast<std::size_t>(
            std::strtoull(val.c_str(), nullptr, 10));
      }
    } else if (key == "expect" &&
               val.find("100-continue") != std::string::npos) {
      expect_continue_ = true;
    } else if (key == "authorization") {
      authorization_ = val;
    } else if (key == "x-file-name") {
      file_name_ = val;
    }
  }
  // 无请求体方法缺 Content-Length 视为 0（curl/浏览器/Qt QNAM 的
  // GET/HEAD 都不发该头，R23-3 走查实录）；带体方法仍强制。
  if (!seen_content_length_ && (method_ == "GET" || method_ == "DELETE" ||
                                method_ == "HEAD")) {
    content_length_ = 0;
  } else if (!seen_content_length_ ||
             content_length_ == static_cast<std::size_t>(-1)) {
    respond_json(400, {{"ok", false}, {"error", "Content-Length 缺失或非法"}});
    return false;
  }
  // 路由感知上限：upload 收大字节，其余路由只收小 JSON/空 body；
  // uplink 面可配独立上限（R23-5，0=沿用全局）；品牌素材路由放行
  // 512KiB＋小余量（素材上限校验在路由层做——读全 body 才能回语义化
  // 413 文案；带外的荒谬大包仍撞 cap 层）
  if (path_ == "/files/upload") {
    body_cap_ = kMaxUpload;
  } else if (path_ == "/uplink/upload") {
    body_cap_ = impl_.uplink_policy.max_upload_bytes > 0
                    ? static_cast<std::size_t>(
                          impl_.uplink_policy.max_upload_bytes)
                    : kMaxUpload;
  } else if (path_ == "/files/branding/logo" ||
             path_ == "/files/branding/splash") {
    body_cap_ = kBrandAssetMax + 64 * 1024;
  } else if (path_ == "/files/emoji/upload") {
    // 表情素材（需求批②）：GIF 动图放宽到 1MiB（上限校验在路由层）
    body_cap_ = kEmojiAssetMax + 64 * 1024;
  } else if (path_ == "/files/avatar/upload") {
    // 头像（需求批⑫）：与表情同口径 1MiB（语义化 413 在路由层，同上）
    body_cap_ = kEmojiAssetMax + 64 * 1024;
  } else {
    body_cap_ = kMaxJsonBody;
  }
  return true;
}
std::string FileConn::account_or_respond() {
  const std::string account = impl_.auth_session(authorization_);
  if (account.empty()) {
    respond_json(401, {{"ok", false},
                       {"error", "会话无效或过期（先 POST /files/session）"}});
  }
  return account;
}
void FileConn::respond_json(int status, const json& body) {
  const std::string payload = body.dump();
  std::ostringstream head;
  head << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n"
       << "Content-Type: application/json; charset=utf-8\r\n"
       << "Content-Length: " << payload.size() << "\r\n"
       << "Connection: close\r\n\r\n"
       << payload;
  write_raw(std::make_shared<const std::string>(head.str()), true,
            [](std::error_code) {});
}
void FileConn::write_raw(std::shared_ptr<const std::string> bytes,
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

} // namespace memex::server
