// 接入会话：帧解码、心跳应答、登录流程与互踢下发。
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>

#include <asio.hpp>

#include <memex/protocol/messages.hpp>

#include "server.hpp"

namespace memex::server {

class Session : public std::enable_shared_from_this<Session> {
public:
  Session(asio::ip::tcp::socket socket, CollabServer& server);

  void start();

  const std::string& account() const { return account_; }
  const std::string& device_name() const { return device_name_; }
  bool desktop() const { return desktop_; }

  // 单点在线互踢：下发 KICK 帧并优雅关闭。
  void kick(const std::string& reason, const std::string& replaced_by);

private:
  void do_read();
  void handle_bytes(std::size_t n);
  void handle_message(const memex::protocol::Message& msg);
  void handle_login(const memex::protocol::Message& msg);
  void handle_logout();
  void send(std::string frame); // 写队列单链驱动
  void do_write();
  void close();
  void log(const std::string& what) const;

  asio::ip::tcp::socket socket_;
  CollabServer& server_;
  std::string remote_;
  memex::protocol::FrameDecoder decoder_;
  std::array<char, 65536> read_buf_{};
  std::deque<std::string> write_queue_;
  bool closed_{false};
  bool close_after_flush_{false}; // 互踢：写空即断开
  bool logged_in_{false};
  std::string account_;
  std::string device_name_;
  std::string device_fingerprint_;
  bool desktop_{true}; // 设备类型 desktop=主（单点在线）/mobile=辅
};

} // namespace memex::server
