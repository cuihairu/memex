#include "session.hpp"

#include <chrono>
#include <iostream>

#include "cred.hpp"

namespace memex::server {

namespace v1 = memex::protocol::v1;

namespace {
std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

Session::Session(asio::ip::tcp::socket socket, CollabServer& server)
    : socket_(std::move(socket)), server_(server),
      remote_([&] {
        try {
          return socket_.remote_endpoint().address().to_string();
        } catch (...) {
          return std::string{"?"};
        }
      }()) {}

void Session::start() {
  log("接入");
  do_read();
}

void Session::kick(const std::string& reason, const std::string& replaced_by) {
  memex::protocol::Message k;
  k.set_type(memex::protocol::v1::KICK);
  k.set_from("server");
  k.set_to(account_);
  k.set_ts_ms(now_ms());
  k.mutable_kick()->set_reason(reason);
  k.mutable_kick()->set_replaced_by(replaced_by);
  log("互踢下发（" + reason + "）");
  close_after_flush_ = true;
  logged_in_ = false;
  send(memex::protocol::encode(k));
}

// 发送统一入口：写队列单链驱动（async_write 不可并发，队空闲才起链）
void Session::send(std::string frame) {
  const bool idle = write_queue_.empty();
  write_queue_.push_back(std::move(frame));
  if (idle && !closed_) do_write();
}

void Session::do_read() {
  auto self = shared_from_this();
  socket_.async_read_some(
      asio::buffer(read_buf_),
      [this, self](std::error_code ec, std::size_t n) {
        if (ec) {
          log(std::string{"断开："} + ec.message());
          if (logged_in_) server_.unregister_online(account_, this);
          return;
        }
        handle_bytes(n);
        if (!closed_) do_read();
      });
}

void Session::handle_bytes(std::size_t n) {
  std::vector<std::string> frames;
  const auto st = decoder_.feed(std::string_view(read_buf_.data(), n), frames);
  if (st == memex::protocol::DecodeStatus::kZeroLength ||
      st == memex::protocol::DecodeStatus::kTooLarge) {
    log(std::string{"非法帧："} + memex::protocol::decode_status_name(st));
    close();
    return;
  }
  for (const auto& f : frames) {
    if (closed_) return;
    try {
      handle_message(memex::protocol::decode_payload(f));
    } catch (const memex::protocol::ProtocolError& e) {
      log(std::string{"协议错误："} + e.what());
    }
  }
}

void Session::handle_message(const memex::protocol::Message& msg) {
  if (logged_in_) {
    log(std::string{"收到 "} + memex::protocol::msg_type_name(msg.type()) +
        "（" + account_ + "）");
  } else {
    log(std::string{"收到 "} + memex::protocol::msg_type_name(msg.type()));
  }

  switch (msg.type()) {
  case v1::PING: {
    memex::protocol::Message pong;
    pong.set_type(v1::PONG);
    pong.set_seq(msg.seq());
    pong.set_from("server");
    pong.set_to(msg.from());
    pong.set_ts_ms(now_ms());
    send(memex::protocol::encode(pong));
    break;
  }
  case v1::LOGIN:
    if (!logged_in_) handle_login(msg);
    break;
  case v1::LOGOUT:
    if (logged_in_) handle_logout();
    break;
  default:
    // 消息路由与归档在 T2.2／T2.3 接入
    break;
  }
}

// 登录校验 → 记录（成功失败都记）→ 原子化互踢 → 回结果。
void Session::handle_login(const memex::protocol::Message& msg) {
  if (!msg.has_login()) {
    log("登录载荷缺字段，拒绝");
    close();
    return;
  }
  const auto& in = msg.login();
  account_ = in.account();
  device_fingerprint_ = in.device_fingerprint();
  device_name_ = in.device_name();
  desktop_ = in.device_kind() != "mobile"; // 桌面=主设备；mobile=辅

  LoginRecord rec;
  rec.account = account_;
  rec.fingerprint = device_fingerprint_;
  rec.kind = in.device_kind();
  rec.name = in.device_name();
  rec.source_ip = remote_;
  rec.version = in.client_version();

  const auto row = server_.store().find_account(account_);
  bool ok = false;
  std::string reason;
  if (!row) {
    reason = "账号不存在";
    rec.result = "no_account";
  } else {
    const std::string digest =
        pbkdf2_sha256_hex(in.password(), row->salt_hex, 60000);
    if (digest == row->digest_hex) {
      ok = true;
      rec.result = "ok";
    } else {
      reason = "口令不符";
      rec.result = "bad_password";
    }
  }
  server_.store().add_login_record(rec);

  memex::protocol::Message result;
  result.set_type(v1::LOGIN_RESULT);
  result.set_to(account_);
  result.set_ts_ms(now_ms());
  if (ok) {
    // 单点在线：桌面端（主设备）同账号只允许一台在线——
    // register_online 在 io 线程内完成顶替，登录回包与互踢不会交错
    const auto kicked =
        desktop_ ? server_.register_online(account_, shared_from_this())
                 : nullptr;
    (void)kicked;
    logged_in_ = true;
    result.mutable_login_result()->set_ok(true);
    result.mutable_login_result()->set_display_name(
        row->display_name);
    log("登录成功（" + account_ + "，" + in.device_kind() + "）");
  } else {
    result.mutable_login_result()->set_ok(false);
    result.mutable_login_result()->set_reason(reason);
    log("登录失败：" + reason);
  }
  send(memex::protocol::encode(result));
}

void Session::handle_logout() {
  log("登出（" + account_ + "）");
  server_.unregister_online(account_, this);
  logged_in_ = false;
  close();
}

void Session::do_write() {
  auto self = shared_from_this();
  asio::async_write(socket_, asio::buffer(write_queue_.front()),
                    [this, self](std::error_code ec, std::size_t) {
                      if (ec) {
                        close();
                        return;
                      }
                      write_queue_.pop_front();
                      if (!write_queue_.empty()) {
                        do_write();
                      } else if (close_after_flush_) {
                        close(); // 互踢：KICK 帧落网后断开
                      }
                    });
}

void Session::close() {
  if (closed_) return;
  closed_ = true;
  std::error_code ignore;
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
  socket_.close(ignore);
}

void Session::log(const std::string& what) const {
  std::cout << "[MEMEX][session " << remote_ << "] " << what << std::endl;
}

} // namespace memex::server
