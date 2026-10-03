// webhook 通知接入（T4.10）：独立端口上的最小 HTTP/1.1 POST 接收器——
// POST /hook/<token>，JSON body（目标／标题／内容／紧急程度／可选跳转），
// token sha256 摘要比对鉴权。与 CollabServer 共用同一 io_context（单线程
// 驱动，无并发状态）；投递面镜像 TEXT（离线入队＋归档＋在线扇出＋常用联系人）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <asio.hpp>

#include "server.hpp"

namespace memex::server {

// 通知投递结果：ok=false 时 http_status/error 供 HTTP 层回写响应。
struct NoticeDelivery {
  bool ok{false};
  int http_status{500};
  std::string error;  // 失败原因（中文，进 JSON error 字段）
  std::string msg_id; // 成功：服务端分配的消息标识（去重与对账键）
  int recipients{0};  // 成功：收件人数（群＝成员数，个人＝1）
};

// 投递一条通知：目标校验 → 组 NOTICE 信封 → 离线入队＋全量归档＋在线扇出
// ＋常用联系人刷新。target="group:<群号>" 或账号；urgency 取 Notice::Urgency
// 数值（1 普通／2 重要／3 紧急）。目标不存在 http_status=404。
NoticeDelivery deliver_notice(CollabServer& server, const std::string& target,
                              const std::string& title,
                              const std::string& content, int urgency,
                              const std::string& jump_url);

class WebhookServer {
public:
  // port=0 由系统分配（测试用），实际端口经 port() 取。
  WebhookServer(asio::io_context& io, CollabServer& server,
                std::uint16_t port);

  std::uint16_t port() const;

  // 开始接受连接（io.run() 前调用一次）。
  void start_accept();

private:
  void do_accept();

  asio::io_context& io_;
  CollabServer& server_;
  asio::ip::tcp::acceptor acceptor_;
};

} // namespace memex::server
