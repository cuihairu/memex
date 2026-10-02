#include "server.hpp"

#include <algorithm>
#include <iostream>

#include <memex/protocol/messages.hpp>

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

namespace {
std::string kind_name(const std::string& kind) {
  if (kind == "desktop") return "桌面端";
  if (kind == "mobile") return "移动端";
  return kind;
}
} // namespace

std::shared_ptr<Session> CollabServer::register_online(
    const std::string& account, const std::string& device_kind,
    std::shared_ptr<Session> session) {
  std::shared_ptr<Session> kicked;
  auto& by_kind = online_[account];
  const std::string new_device = session->device_name();
  if (const auto it = by_kind.find(device_kind); it != by_kind.end()) {
    kicked = it->second;
    // 先顶替再踢：即使旧会话互踢下发失败，在线表也已指向新会话
    it->second = std::move(session);
    kicked->kick("单点在线：同账号在另一台" + kind_name(device_kind) + "登录",
                 new_device);
  } else {
    by_kind.emplace(device_kind, std::move(session));
  }
  broadcast_presence(); // 在线表变化即推送（新登录者也收到，含自己）
  return kicked;
}

void CollabServer::unregister_online(const std::string& account,
                                     const std::string& device_kind,
                                     Session* session) {
  const auto it = online_.find(account);
  if (it == online_.end()) return;
  const auto kit = it->second.find(device_kind);
  if (kit != it->second.end() && kit->second.get() == session) {
    it->second.erase(kit);
  } else {
    return; // 不是当前在线会话——无变化，不推送
  }
  if (it->second.empty()) online_.erase(it);
  broadcast_presence(); // 登出／互踢旧会话退出／意外断开即推送
}

std::vector<std::shared_ptr<Session>> CollabServer::online_sessions(
    const std::string& account) {
  std::vector<std::shared_ptr<Session>> out;
  if (const auto it = online_.find(account); it != online_.end()) {
    out.reserve(it->second.size());
    for (const auto& [kind, session] : it->second) out.push_back(session);
  }
  return out;
}

std::vector<std::string> CollabServer::online_accounts() {
  std::vector<std::string> out;
  for (const auto& [account, by_kind] : online_) {
    if (!by_kind.empty()) out.push_back(account);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void CollabServer::broadcast_presence() {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::PRESENCE_DATA);
  m.set_from("server");
  m.set_to("");
  auto* p = m.mutable_presence_data();
  for (const auto& a : online_accounts()) p->add_accounts(a);
  // deliver_frame 入参为纯 Envelope 字节（其内部加长度前缀）——此处传
  // SerializeAsString 而非 encode（后者自带前缀，会造成双重成帧）。
  const std::string blob = m.SerializeAsString();
  for (const auto& [account, by_kind] : online_) {
    for (const auto& [kind, session] : by_kind) session->deliver_frame(blob);
  }
}

} // namespace memex::server
