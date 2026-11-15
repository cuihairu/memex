#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>

#include "cred.hpp"
#include "ldap.hpp" // 544 企业身份判登路由（route_authentication）
#include "webhook.hpp" // R24-1 公告联动三级推送（deliver_notice）
#include "session/common.hpp"

namespace memex::server {

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
  case v1::ORG_QUERY: h_org_query(msg); break;
  case v1::TEXT: h_text(msg); break;
  case v1::GROUP_CMD: h_group_cmd(msg); break;
  case v1::GROUP_QUERY: h_group_query(msg); break;
  case v1::CROSS_LOG: h_cross_log(msg); break;
  case v1::ACK: h_ack(msg); break;
  case v1::READ: h_read(msg); break;
  case v1::RECEIPT_QUERY: h_receipt_query(msg); break;
  case v1::MESSAGE_QUERY: h_message_query(msg); break;
  case v1::PRESENCE_QUERY: h_presence_query(msg); break;
  case v1::FAV_QUERY: h_fav_query(msg); break;
  case v1::FAV_CMD: h_fav_cmd(msg); break;
  case v1::PROFILE_CMD: h_profile_cmd(msg); break;
  case v1::NUDGE: h_nudge(msg); break;
  case v1::RECALL: h_recall(msg); break;
  case v1::FILE_AUTHZ: h_file_authz(msg); break;
  default:
    break;
  }
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
