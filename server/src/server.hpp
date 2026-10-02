// 协作服务端核心：接入会话、账号登录校验、同类型设备单点在线互踢
// （桌面端互踢、移动端互踢，桌面与移动可并存——各类型只留最新一台）。
// io_context 单线程驱动：登录校验与在线表替换天然串行（原子化互踢），
// 不存在两台同时登录都成功的交错。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <asio.hpp>

#include "store.hpp"

namespace memex::server {

class Session;

class CollabServer {
public:
  // db 须已 open。port=0 由系统分配（测试用），实际端口经 port() 取。
  CollabServer(asio::io_context& io, ServerStore& store, std::uint16_t port);

  // 开始接受连接（io.run() 前调用一次）。
  void start_accept();

  std::uint16_t port() const;

  // 登录成功后由 Session 调用：按设备类型登记在线；若该账号同类型已有
  // 会话，原子化顶替——旧会话收到 KICK 后关闭，返回被踢会话（可能为空）。
  std::shared_ptr<Session> register_online(const std::string& account,
                                           const std::string& device_kind,
                                           std::shared_ptr<Session> session);

  // 会话结束时除名。
  void unregister_online(const std::string& account,
                         const std::string& device_kind, Session* session);

  // 账号的全量在线会话（桌面＋手机都在时都投递）；无则空。
  std::vector<std::shared_ptr<Session>> online_sessions(
      const std::string& account);

  ServerStore& store() { return store_; }

private:
  void do_accept();

  asio::io_context& io_;
  ServerStore& store_;
  asio::ip::tcp::acceptor acceptor_;
  // account → 设备类型 → 会话（同类型单点在线，跨类型并存）
  std::map<std::string, std::map<std::string, std::shared_ptr<Session>>>
      online_;
};

} // namespace memex::server
