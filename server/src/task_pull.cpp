// R27-5 服务端任务源拉取实现：出网传输走 drogon HttpClient（进程内既有
// 依赖，http/https 都覆盖；TLS 证书校验默认开）＋GitHub issues 适配＋60s
// 主循拉取链。store/notice/拉取链仍在 asio io 单线程（drogon 回包经
// asio::post 折回，trantor 线程只碰网）；外源挂起有 deadline 兜底不拖垮面。
#include "task_pull.hpp"

#include <nlohmann/json.hpp>

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "cred.hpp"

namespace memex::server {

namespace {

using json = nlohmann::json;

constexpr std::size_t kPullBodyMax = 512 * 1024; // 响应体上限（issue 页规模）
constexpr auto kMasterTick = std::chrono::seconds(60); // 拉取主循节拍

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 拉到的任务归类通知标题里的源显示名
const char* provider_label(const std::string& provider) {
  if (provider == "github") return "GitHub";
  return provider.c_str();
}

// url → (https?, host, port, path_with_query)。仅 http(s)；path 缺省 "/"。
bool parse_url(const std::string& url, bool& https, std::string& host,
               std::string& port, std::string& path) {
  if (url.rfind("https://", 0) == 0) {
    https = true;
  } else if (url.rfind("http://", 0) == 0) {
    https = false;
  } else {
    return false;
  }
  std::string rest = url.substr(https ? 8 : 7);
  const auto slash = rest.find('/');
  const std::string host_port =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (host_port.empty()) return false;
  if (host_port[0] == '[') { // IPv6 字面量 [::1]:8080
    const auto close = host_port.find(']');
    if (close == std::string::npos) return false;
    host = host_port.substr(1, close - 1);
    port = close + 2 < host_port.size() && host_port[close + 1] == ':'
               ? host_port.substr(close + 2)
               : (https ? "443" : "80");
    return true;
  }
  const auto colon = host_port.rfind(':');
  if (colon == std::string::npos) {
    host = host_port;
    port = https ? "443" : "80";
  } else {
    host = host_port.substr(0, colon);
    port = host_port.substr(colon + 1);
    if (port.empty()) port = https ? "443" : "80";
  }
  return true;
}

} // namespace

// 生产传输：drogon HttpClient。自持一条 trantor 事件循环线程（纯 HTTP
// I/O——不占 asio io 线程，外源慢不拖垮消息面）；回包经 asio::post 折回
// io 线程再交 done——store/notice 与拉取链保持单线程无锁模型。
class DrogonTaskHttp final : public TaskHttp {
 public:
  explicit DrogonTaskHttp(asio::io_context& io) : io_(io) {
    loop_thread_.run();
  }

  ~DrogonTaskHttp() override = default; // EventLoopThread 析构自停自收

  void get(const std::string& url, const Headers& headers, int deadline_ms,
           ResponseFn done) override {
    bool https = false;
    std::string host, port, path_and_query;
    if (!parse_url(url, https, host, port, path_and_query)) {
      done(false, 0, "url 非法（仅 http/https）");
      return;
    }
    trantor::EventLoop* loop = loop_thread_.getLoop();
    if (loop == nullptr) {
      done(false, 0, "HTTP 线程未就绪");
      return;
    }
    std::string path = path_and_query, query;
    const auto q = path_and_query.find('?');
    if (q != std::string::npos) {
      path = path_and_query.substr(0, q);
      query = path_and_query.substr(q + 1);
    }
    // 证书校验默认开（validateCert=true）：凭据不出明文网，MITM/自签＝
    // 拉取失败明示，不静默降级
    auto client = drogon::HttpClient::newHttpClient(
        host, static_cast<std::uint16_t>(std::atoi(port.c_str())), https,
        loop, /*useOldTLS=*/false, /*validateCert=*/true);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPathEncode(false); // 源 URL 由本模块拼装，路径按原文不动
    req->setPath(path);
    std::istringstream qin(query);
    std::string pair;
    while (std::getline(qin, pair, '&')) {
      if (pair.empty()) continue;
      const auto eq = pair.find('=');
      if (eq == std::string::npos) continue;
      req->setQueryParameter(pair.substr(0, eq), pair.substr(eq + 1));
    }
    for (const auto& [k, v] : headers) req->addHeader(k, v);
    const double timeout_s = deadline_ms > 0 ? deadline_ms / 1000.0 : 0.0;
    asio::io_context* io = &io_;
    // 回调捕获 client 保活（在途回包不解引用已析构对象）；折回 io 线程
    // 收口（恰一次：sendRequest 回调恰一次语义由 drogon 保证）
    client->sendRequest(
        req,
        [client, io, done = std::move(done)](
            drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
          bool ok = r == drogon::ReqResult::Ok && resp != nullptr;
          int status = 0;
          std::string body;
          if (ok) {
            status = static_cast<int>(resp->getStatusCode());
            body = resp->body();
            if (body.size() > kPullBodyMax) {
              ok = false; // 响应体超限＝失败收口
              status = 0;
              body.clear();
            }
          }
          asio::post(*io, [done = std::move(done), ok, status,
                           body = std::move(body)]() mutable {
            done(ok, status, std::move(body));
          });
        },
        timeout_s);
  }

 private:
  asio::io_context& io_;
  trantor::EventLoopThread loop_thread_;
};

std::unique_ptr<TaskHttp> make_task_http(asio::io_context& io) {
  return std::make_unique<DrogonTaskHttp>(io);
}

std::vector<PulledTask> parse_github_issues(const std::string& body) {
  std::vector<PulledTask> out;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    return out; // 坏 JSON＝空（上层记失败，不造假数据）
  }
  if (!j.is_array()) return out;
  for (const auto& item : j) {
    if (!item.is_object() || !item.contains("number") ||
        !item.contains("title")) {
      continue;
    }
    if (item.contains("pull_request")) continue; // 只收 issue，滤 PR 行
    if (!item["number"].is_number_integer() || !item["title"].is_string()) {
      continue;
    }
    PulledTask t;
    t.ext_key = std::to_string(item["number"].get<std::int64_t>());
    t.title = item["title"].get<std::string>();
    if (item.contains("body") && item["body"].is_string()) {
      t.note = item["body"].get<std::string>();
    }
    if (t.title.empty()) continue;
    out.push_back(std::move(t));
  }
  return out;
}

// —— TaskPuller ——

TaskPuller::TaskPuller(asio::io_context& io, ServerStore& store)
    : io_(io), store_(store), master_(io) {}

void TaskPuller::set_key(const std::string& key32) { key_ = key32; }

void TaskPuller::set_notice(NoticeFn fn) { notice_ = std::move(fn); }

void TaskPuller::set_http(std::unique_ptr<TaskHttp> http) {
  http_ = std::move(http);
}

void TaskPuller::arm_master() {
  master_.expires_after(kMasterTick);
  master_.async_wait([this](std::error_code ec) {
    if (ec) return; // 取消＝停循（随 io 停摆）
    tick();
    arm_master();
  });
}

void TaskPuller::start() {
  if (key_.empty()) {
    // 三道闸第一道：主密钥未配＝拉取面未启用（零外呼零定时器零传输线程
    // ——审计面一行明示，不静默）
    std::cout << "[MEMEX] 任务源拉取面未启用（未配 --task-pull-secret）"
              << std::endl;
    return;
  }
  if (!http_) http_ = make_task_http(io_);
  arm_master();
}

void TaskPuller::tick() {
  // 闸②：admin 开关关＝零外呼（缺行/非 "1" 同口径＝默认关）
  const auto flag = store_.server_flag_get("task_pull_enabled");
  if (!flag.has_value() || *flag != "1") return;
  // 闸①：主密钥未配＝拉取面未启用
  if (key_.empty()) return;
  const std::int64_t now = now_ms();
  auto due = std::make_shared<std::vector<ServerStore::TaskPullConfig>>();
  for (const auto& c : store_.task_pull_configs()) {
    if (!c.enabled) continue; // 闸③：单源关＝跳过
    if (c.interval_s > 0 && now - c.last_pull_ms < c.interval_s * 1000) {
      continue; // 未到期
    }
    due->push_back(c);
  }
  pull_due(0, std::move(due), now);
}

void TaskPuller::pull_due(
    std::size_t idx,
    std::shared_ptr<std::vector<ServerStore::TaskPullConfig>> due,
    std::int64_t now) {
  if (!due || idx >= due->size()) return;
  const ServerStore::TaskPullConfig& c = (*due)[idx];
  const auto next = [this, idx, due, now] { pull_due(idx + 1, due, now); };
  // provider→URL（现只接 github；gitlab/jira/linear 未接——配置面已 400 拦）
  if (c.provider != "github" || c.repo.empty()) {
    std::cerr << "[MEMEX] task-pull 跳过未接源 owner=" << c.owner
              << " provider=" << c.provider << std::endl;
    next();
    return;
  }
  const std::string url = "https://api.github.com/repos/" + c.repo +
                          "/issues?state=open&per_page=50";
  TaskHttp::Headers headers{
      {"Accept", "application/vnd.github+json"},
      {"X-GitHub-Api-Version", "2022-11-28"},
  };
  // 凭据内存内解密（密封行→GCM 开封；开封失败=无凭据裸拉公开源——不落
  // 明文不进日志）
  const auto sealed = store_.task_source_cred_sealed(c.owner, c.provider);
  if (sealed.has_value() && !sealed->empty()) {
    const std::string token = gcm_open(key_, *sealed);
    if (!token.empty()) headers.push_back({"Authorization", "Bearer " + token});
  }
  http_->get(url, headers, 30000,
             [this, next, c, now](bool ok, int status, std::string body) {
               if (!ok || status != 200) {
                 std::cerr << "[MEMEX] task-pull 失败 owner=" << c.owner
                           << " repo=" << c.repo << " status=" << status
                           << std::endl;
                 next();
                 return;
               }
               const auto tasks = parse_github_issues(body);
               std::size_t created = 0, changed = 0;
               for (const auto& t : tasks) {
                 const auto r = store_.task_upsert_external(
                     c.owner, t.title, t.note, c.provider, t.ext_key, now);
                 if ((r.created || r.changed) && notice_) {
                   notice_(c.owner,
                           "任务源更新：" +
                               std::string(provider_label(c.provider)),
                           t.title, 1);
                 }
                 if (r.created) ++created;
                 if (r.changed) ++changed;
               }
               store_.task_pull_config_mark_pulled(c.owner, c.provider, now);
               std::cout << "[MEMEX] task-pull owner=" << c.owner
                         << " repo=" << c.repo << " tasks=" << tasks.size()
                         << " new=" << created << " changed=" << changed
                         << std::endl;
               next();
             });
}

} // namespace memex::server
