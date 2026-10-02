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

void Session::deliver_frame(const std::string& envelope_blob) {
  send(memex::protocol::encode_frame(envelope_blob));
}

void Session::do_read() {
  auto self = shared_from_this();
  socket_.async_read_some(
      asio::buffer(read_buf_),
      [this, self](std::error_code ec, std::size_t n) {
        if (ec) {
          log(std::string{"断开："} + ec.message());
          if (logged_in_) server_.unregister_online(account_, kind_, this);
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
  case v1::ORG_QUERY: {
    // 组织架构下发（T3.1）：登录后可查——部门树＋成员资料（含直属上级与角色）
    if (!logged_in_) break;
    memex::protocol::Message out;
    out.set_type(v1::ORG_DATA);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* data = out.mutable_org_data();
    for (const auto& [id, path] : server_.store().department_list()) {
      (void)id;
      data->add_departments()->set_path(path);
    }
    for (const auto& m : server_.store().member_list()) {
      auto* om = data->add_members();
      om->set_account(m.account);
      om->set_display_name(m.display_name);
      om->set_title(m.title);
      om->set_department_path(m.department_path);
      om->set_manager(m.manager);
      om->set_role(m.role);
    }
    send(memex::protocol::encode(out));
    break;
  }
  case v1::TEXT: {
    // 协作态消息路由：先入离线队列（至少一次投递），在线即投；
    // 接收方 ACK(msg_id) 清队列，未 ACK 的下次登录重投（接收端按 msg_id 去重）。
    if (!logged_in_ || !msg.has_text()) break;
    const std::string msg_id =
        sha256_hex(msg.from() + ":" + std::to_string(msg.seq()));
    memex::protocol::Message out = msg;
    out.set_msg_id(msg_id);
    const std::string blob = out.SerializeAsString();
    server_.store().queue_offline(msg_id, msg.to(), blob);
    // T2.3 全量归档：协作态消息原样落服务端归档库（本地缓存另行存于客户端）
    server_.store().store_message(msg_id, msg.from(), msg.to(),
                                  static_cast<int>(msg.type()),
                                  msg.text().text(), msg.ts_ms());
    // 在线即投（桌面＋手机都在则都投，任一端 ACK 即清队列）
    for (const auto& target : server_.online_sessions(msg.to())) {
      target->deliver_frame(blob);
    }
    // 发送方受理回执（原 seq）：消息已被服务端接收并负责投递
    memex::protocol::Message ack;
    ack.set_type(v1::ACK);
    ack.set_seq(msg.seq());
    ack.set_to(msg.from());
    send(memex::protocol::encode(ack));
    break;
  }
  case v1::ACK:
    // 接收方回执：消息已收取，清离线队列
    if (logged_in_ && msg.has_ack() && !msg.ack().msg_id().empty()) {
      server_.store().ack_offline(msg.ack().msg_id());
    }
    break;
  case v1::RECALL: {
    // 撤回：仅置标记不清正文；事件独立留痕；转发给对端会话（本地展示标记）。
    if (!logged_in_ || !msg.has_recall()) break;
    const std::string target = msg.recall().msg_id();
    if (target.empty()) break;
    const std::string original_from = server_.store().message_from(target);
    if (original_from.empty()) {
      log("撤回目标不存在：" + target);
      break;
    }
    if (original_from != account_) {
      log("越权撤回被拒：目标发送方为 " + original_from);
      break;
    }
    server_.store().recall_message(target);
    server_.store().record_recall_event(target, account_, now_ms());
    log("撤回留痕：" + target);
    // 转发给消息会话双方的在线会话（不含本会话），客户端按 msg_id 标记本地副本
    for (const auto& s : server_.online_sessions(msg.to())) {
      memex::protocol::Message out = msg;
      out.set_msg_id(target);
      s->deliver_frame(out.SerializeAsString());
    }
    for (const auto& s : server_.online_sessions(account_)) {
      if (s.get() == this) continue;
      memex::protocol::Message out = msg;
      out.set_msg_id(target);
      s->deliver_frame(out.SerializeAsString());
    }
    break;
  }
  default:
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
  kind_ = in.device_kind().empty() ? "desktop" : in.device_kind();

  LoginRecord rec;
  rec.account = account_;
  rec.fingerprint = device_fingerprint_;
  rec.kind = kind_;
  rec.name = device_name_;
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
    // 同类型单点在线：同账号桌面端互踢、移动端互踢，桌面与手机并存。
    // register_online 在 io 线程内完成顶替，登录回包与互踢不会交错
    const auto kicked =
        server_.register_online(account_, kind_, shared_from_this());
    (void)kicked;
    logged_in_ = true;
    result.mutable_login_result()->set_ok(true);
    result.mutable_login_result()->set_display_name(
        row->display_name);
    log("登录成功（" + account_ + "，" + kind_ + "）");
  } else {
    result.mutable_login_result()->set_ok(false);
    result.mutable_login_result()->set_reason(reason);
    log("登录失败：" + reason);
  }
  send(memex::protocol::encode(result));
  if (ok) {
    // 离线消息补投：登录回执之后推未 ACK 的队列（重复投递由接收端 msg_id 去重）
    for (const auto& blob : server_.store().pending_offline(account_)) {
      deliver_frame(blob);
    }
  }
}

void Session::handle_logout() {
  log("登出（" + account_ + "）");
  server_.unregister_online(account_, kind_, this);
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
