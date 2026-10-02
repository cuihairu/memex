#include "server.hpp"

#include <iostream>

#include "session.hpp"

namespace memex::server {

CollabServer::CollabServer(asio::io_context& io, ServerStore& store,
                           std::uint16_t port)
    : io_(io), store_(store),
      acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {}

std::uint16_t CollabServer::port() const {
  return acceptor_.local_endpoint().port();
}

void CollabServer::start_accept() {
  std::cout << "[MEMEX] MemexServer 监听 0.0.0.0:" << port() << std::endl;
  do_accept();
}

void CollabServer::do_accept() {
  acceptor_.async_accept(
      [this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (!ec) {
          std::make_shared<Session>(std::move(socket), *this)->start();
        }
        do_accept();
      });
}

std::shared_ptr<Session> CollabServer::register_online(
    const std::string& account, std::shared_ptr<Session> session) {
  std::shared_ptr<Session> kicked;
  const std::string new_device = session->device_name();
  if (const auto it = online_.find(account); it != online_.end()) {
    kicked = it->second;
    // 先顶替再踢：即使旧会话互踢下发失败，在线表也已指向新会话
    it->second = std::move(session);
    kicked->kick("单点在线：同账号在另一台桌面端登录", new_device);
  } else {
    online_.emplace(account, std::move(session));
  }
  return kicked;
}

void CollabServer::unregister_online(const std::string& account,
                                     Session* session) {
  const auto it = online_.find(account);
  if (it != online_.end() && it->second.get() == session) {
    online_.erase(it);
  }
}

} // namespace memex::server
