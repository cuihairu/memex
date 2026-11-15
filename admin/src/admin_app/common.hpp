// admin_app 内部共性面（LAY-004 拆分自 admin_app.cpp）：AdminContext
// 共享状态、Handler/Guarded 注册器形态、应答/请求小工具与判权门声明、
// 各域路由组注册入口。实现见 common.cpp；域处理器见同目录各 .cpp；
// 装配（配置/az/guarded/静态页）仍在 admin_app.cpp。
#pragma once

#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <authz.hpp>
#include <store.hpp>

namespace memex::admin {


// —— 路由面共享状态：setup() 会返回而注册的处理器长命——全部状态进
//    堆上 AdminContext，处理器按值捕获 shared_ptr（按引用捕获局部量
//    =悬垂 UB：进程内测试表现为判登错乱，独立进程表现为段错误）。
struct AdminContext {
  std::shared_ptr<memex::server::ServerStore> store;
  std::mutex mu; // 兜底锁（当前单 IO 线程实际无竞争）
  memex::server::AuthorizationService az;
  std::int64_t started_ms{0};
  std::string files_url;
  std::string storage_url;
};

// 处理器统一形态与「登录态＋CSRF 闸」注册器（guarded）签名
using Handler = std::function<void(const drogon::HttpRequestPtr&,
                                   std::function<void(
                                       const drogon::HttpResponsePtr&)>&&)>;
using Guarded = std::function<void(
    const char*, const std::vector<drogon::internal::HttpConstraint>&, bool,
    const Handler&)>;

// —— 小工具/判权门（实现见 common.cpp；原匿名空间面逐字搬移，默认参数
//    只在声明处）——
// 时间戳小工具收进本头文件 inline（同 store/common.hpp 口径）：9 个模块
// 都调用，留实现文件要多写一次声明且收益仅一行。
inline std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
drogon::HttpResponsePtr json_resp(drogon::HttpStatusCode code,
                                  const Json::Value& j);
Json::Value ok_json();
drogon::HttpResponsePtr err_resp(drogon::HttpStatusCode code,
                                 const std::string& msg);
std::shared_ptr<Json::Value> body_of(const drogon::HttpRequestPtr& req);
std::string str_or(const Json::Value& j, const char* key,
                   const std::string& dflt = {});
std::int64_t int_or(const Json::Value& j, const char* key,
                    std::int64_t dflt = 0);
bool bool_or(const Json::Value& j, const char* key, bool dflt = false);
bool parse_gid_q(const std::string& s, std::uint64_t* out);
std::string assist_mask_names(int mask);
std::pair<int, std::string> png_asset_check(const std::string& b);
std::string session_account(const drogon::HttpRequestPtr& req);
bool csrf_ok(const drogon::HttpRequestPtr& req);
std::optional<drogon::HttpResponsePtr> require_manage(
    AdminContext& ctx, const drogon::HttpRequestPtr& req,
    const std::string& action_detail);
std::optional<drogon::HttpResponsePtr> require_audit(
    AdminContext& ctx, const drogon::HttpRequestPtr& req);

// —— 路由分组注册入口（setup 装配调用；实现见同目录各 .cpp）——
void register_auth_routes(drogon::HttpAppFramework& app,
                        const std::shared_ptr<AdminContext>& ctx,
                        const Guarded& guarded);
void register_account_org_routes(drogon::HttpAppFramework& app,
                               const std::shared_ptr<AdminContext>& ctx,
                               const Guarded& guarded);
void register_audit_device_routes(drogon::HttpAppFramework& app,
                                const std::shared_ptr<AdminContext>& ctx,
                                const Guarded& guarded);
void register_notify_routes(drogon::HttpAppFramework& app,
                          const std::shared_ptr<AdminContext>& ctx,
                          const Guarded& guarded);
void register_files_quota_routes(drogon::HttpAppFramework& app,
                               const std::shared_ptr<AdminContext>& ctx,
                               const Guarded& guarded);
void register_group_routes(drogon::HttpAppFramework& app,
                         const std::shared_ptr<AdminContext>& ctx,
                         const Guarded& guarded);
void register_flow_routes(drogon::HttpAppFramework& app,
                        const std::shared_ptr<AdminContext>& ctx,
                        const Guarded& guarded);
void register_brand_health_routes(drogon::HttpAppFramework& app,
                                const std::shared_ptr<AdminContext>& ctx,
                                const Guarded& guarded);

} // namespace memex::admin
