// 模型网关（平台三期）：OpenAI 兼容入口——独立端口独立线程（上游调用为
// 秒级阻塞，不与 CollabServer/WebhookServer 抢共享 io_context）；自有
// ServerStore 第二连接开同库（sqlite3_busy_timeout 兜写锁竞争）。鉴权
// 复用 bot token（bot:<name> 即调用主体——群内智能助手等内部服务载体）。
// 红线写死在代码：带归档数据标记的请求只路由 is_local 端点（不做配置）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <asio.hpp>

#include "store.hpp"

namespace memex::server {

// 端点登记行之外的一次上游尝试结果（网关内部用）。
struct UpstreamResult {
  int status{0}; // HTTP 状态；0=连接失败/超时（未收到响应）
  std::string body;
  std::string error; // 连接失败/超时原因（中文，进日志与审计备注口径）
};

// 上游调用：POST {base_url}/chat/completions（OpenAI 兼容形态），读到
// EOF（Connection: close）；30s 死线（独立 io_context + steady_timer，
// 超时关 socket）。仅支持 http://——内网口径，明文面不出机房。
UpstreamResult http_post_json(const std::string& base_url,
                              const std::string& api_key,
                              const std::string& json_body);

class ModelGatewayServer {
public:
  // port=0 由系统分配（测试用），实际端口经 port() 取；db_path 为同库
  // 第二连接（网关线程专用自己的 ServerStore）。
  ModelGatewayServer(std::uint16_t port, const std::string& db_path);
  ~ModelGatewayServer();

  std::uint16_t port() const;

  // 开始接受连接（线程启动前调用一次）；run()/stop() 驱动与收尾。
  void start_accept();
  void run();
  void stop();

private:
  void do_accept();

  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  ServerStore store_; // 网关专用连接（同库第二连接）
};

} // namespace memex::server
