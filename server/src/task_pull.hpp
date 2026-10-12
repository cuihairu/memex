// R27-5 服务端任务源拉取：定时从外部 provider（现只接 GitHub）拉任务落
// tasks 表（provider/ext_key 现口径），新落/有变化经既有长连接推（notice
// 回调＝main 接 deliver_notice 同一投递面；客户端 30s 拉取降为兜底）。
// 数据外发默认关硬口径的三道闸（任一不满足＝零外呼）：
//   ①主密钥未配（--task-pull-secret 不给）＝拉取面未启用，timer 不武装；
//   ②admin 开关 server_flags「task_pull_enabled」非 "1"＝tick 短路；
//   ③任务源配置行 enabled=0＝单源跳过。
// 出网传输抽象 TaskHttp 可注入（测试注桩不打外网）；凭据 AES-256-GCM
// 密封落库（密码箱同水位 PBKDF2→KEK→GCM），拉取时服务端内存内解密，
// 明文不落库不进日志。
#pragma once

#include <asio.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "store.hpp"

namespace memex::server {

// 单条外部任务（源适配器输出；落库口径=provider+ext_key 引用，服务端
// 不解析不存详情 URL——跳转由客户端 SPI 现解析）
struct PulledTask {
  std::string ext_key; // 源内标识（GitHub=issue 编号）
  std::string title;
  std::string note; // 描述正文（GitHub=body；可为空）
};

// 出网 HTTP 传输抽象（异步，完成回调在 io 线程；注入点＝测试桩不打外网）。
// deadline_ms 到点未完按失败收口（ok=false）——外源挂起不拖垮拉取链。
class TaskHttp {
 public:
  using Headers = std::vector<std::pair<std::string, std::string>>;
  using ResponseFn = std::function<void(bool ok, int status,
                                        std::string body)>;
  virtual ~TaskHttp() = default;
  virtual void get(const std::string& url, const Headers& headers,
                   int deadline_ms, ResponseFn done) = 0;
};

// 生产传输：drogon HttpClient（进程内既有依赖；http/https 都覆盖，TLS
// 证书校验默认开——凭据不出明文网）。body 上限 512KB（超限按失败收口）。
std::unique_ptr<TaskHttp> make_task_http(asio::io_context& io);

// GitHub issues JSON → 任务列表（v3 /repos/{o}/{r}/issues 数组；滤除带
// pull_request 键的行＝只收 issue；ext_key=number 十进制串）。坏 JSON/非
// 数组＝空列表（拉取链记失败日志，不造假数据）。
std::vector<PulledTask> parse_github_issues(const std::string& body);

// 拉取器：60s 主循（tick）遍历到期任务源（due=enabled 且 now−last_pull_ms
// ≥ interval_s*1000；interval_s≤0＝每次都拉，测试用）逐源拉取落库。
class TaskPuller {
 public:
  // 与 FileServer::GroupNoticeFn 同形（target/title/content/urgency）；
  // main 接同一个 deliver_notice lambda＝离线入队＋归档＋在线扇出同投递面。
  using NoticeFn = std::function<void(const std::string& target,
                                      const std::string& title,
                                      const std::string& content,
                                      int urgency)>;
  TaskPuller(asio::io_context& io, ServerStore& store);

  // 32B 拉取凭据密钥（derive_task_pull_key 派生；空＝拉取面未启用）
  void set_key(const std::string& key32);
  void set_notice(NoticeFn fn);
  // 传输注入（缺省=make_asio_task_http(io)；测试注桩）
  void set_http(std::unique_ptr<TaskHttp> http);

  // 武装 60s 主循（key 空＝不武装＝零外呼零定时器；io.run() 前调用）。
  void start();
  // 单轮拉取（测试直呼）：三道闸逐道短路，然后逐到期源串行拉取。
  void tick();

 private:
  void arm_master();
  void pull_due(std::size_t idx,
                std::shared_ptr<std::vector<ServerStore::TaskPullConfig>> due,
                std::int64_t now);

  asio::io_context& io_;
  ServerStore& store_;
  std::string key_;
  NoticeFn notice_;
  std::unique_ptr<TaskHttp> http_;
  asio::steady_timer master_;
};

} // namespace memex::server
