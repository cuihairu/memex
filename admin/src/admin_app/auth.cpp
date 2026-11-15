// 登录/登出/whoami 路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"

#include <cred.hpp>

namespace memex::admin {

void register_auth_routes(drogon::HttpAppFramework& app,
                          const std::shared_ptr<AdminContext>& ctx,
                          const Guarded& guarded) {
  // ============ 登录面 ============
  app.registerHandler(
      "/api/login",
      [ctx](const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!csrf_ok(req)) {
          cb(err_resp(drogon::k403Forbidden, "缺少 X-Memex-Admin 头"));
          return;
        }
        const auto j = body_of(req);
        if (!j) {
          cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
          return;
        }
        const std::string account = str_or(*j, "account");
        const std::string password = str_or(*j, "password");
        // 与 /files/session 同源口令校验（credentials 表判登，平台-2；
        // PBKDF2-HMAC-SHA256 60000 轮同参）。Admin 会话与消息面会话
        // 相互独立——这里不发服务端 token，只落本进程 Drogon 会话。
        bool pass = false;
        {
          std::lock_guard<std::mutex> lk(ctx->mu);
          const auto cred = ctx->store->find_credential(account, "password");
          if (cred.has_value()) {
            pass = memex::server::pbkdf2_sha256_hex(password, cred->salt_hex,
                                                    60000) ==
                   cred->digest_hex;
          }
        }
        if (!pass) {
          std::cout << "[MEMEX-ADMIN] login.denied account=" << account
                    << "\n";
          cb(err_resp(drogon::k401Unauthorized, "账号或口令错误"));
          return;
        }
        req->session()->insert("account", account);
        Json::Value roles(Json::arrayValue);
        {
          std::lock_guard<std::mutex> lk(ctx->mu);
          for (const auto& r : ctx->store->effective_roles(account, now_ms()))
            roles.append(r);
        }
        Json::Value out = ok_json();
        out["account"] = account;
        out["roles"] = roles;
        std::cout << "[MEMEX-ADMIN] login account=" << account << "\n";
        cb(json_resp(drogon::k200OK, out));
      },
      {drogon::Post});

  guarded("/api/logout", {drogon::Post}, true,
          [](const drogon::HttpRequestPtr& req,
             std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            req->session()->erase("account");
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  app.registerHandler(
      "/api/whoami",
      [ctx](const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        const std::string account = session_account(req);
        if (account.empty()) {
          cb(err_resp(drogon::k401Unauthorized, "未登录或会话已过期"));
          return;
        }
        Json::Value roles(Json::arrayValue);
        {
          std::lock_guard<std::mutex> lk(ctx->mu);
          for (const auto& r : ctx->store->effective_roles(account, now_ms()))
            roles.append(r);
        }
        Json::Value out = ok_json();
        out["account"] = account;
        out["roles"] = roles;
        cb(json_resp(drogon::k200OK, out));
      },
      {drogon::Get});
}

} // namespace memex::admin
