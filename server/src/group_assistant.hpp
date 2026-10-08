// 群内智能助手（平台三期）：内置编排器——以 bot 身份轮询收信队列，识别
// 四指令（@助手/@纪要/@整理/@检索），走模型网关（回环）生成回答、走回环
// HTTP /bot/send 回群。独立线程；回群不直调 deliver_notice（CollabServer
// 在线表无锁，跨线程不可用）。检索增强资料=留痕归档（bot 所在群消息天然
// 在检索半径内）；网关请求一律带 memex_archive_scope:true——归档红线
// （仅本地模型）原样生效。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "store.hpp"

namespace memex::server {

class GroupAssistantWorker {
public:
  // bot_name 不含 "bot:" 前缀；bot_token 为该 bot 明文（回环 /bot/send
  // 与网关共用）；webhook_port=回环投递口；gateway_base=模型网关根
  // （http://127.0.0.1:<port>，空=模型未启用）；db_path=自有库第二连接；
  // poll_ms=轮询间隔（测试可调小）。
  GroupAssistantWorker(std::string bot_name, std::string bot_token,
                       std::uint16_t webhook_port, std::string gateway_base,
                       const std::string& db_path, int poll_ms = 1000);
  ~GroupAssistantWorker();

  GroupAssistantWorker(const GroupAssistantWorker&) = delete;
  GroupAssistantWorker& operator=(const GroupAssistantWorker&) = delete;

  void start(); // 起线程（io 无关，纯轮询）
  void stop();  // 停轮询并 join

  // 测试观察点：已处理的指令条数（含失败回群的处理）。
  int handled() const { return handled_.load(); }

private:
  void loop();
  // 轮询一次：拉队列→逐条解码→指令分发→ack。返回处理条数。
  int poll_once();

  std::string bot_name_;   // 不含前缀
  std::string self_;       // bot:<name>
  std::string bot_token_;
  std::uint16_t webhook_port_;
  std::string gateway_base_; // 空=模型未启用
  int poll_ms_;

  ServerStore store_; // 自有库第二连接
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> handled_{0};
};

} // namespace memex::server
