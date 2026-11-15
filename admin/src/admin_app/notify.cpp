// 通知与集成路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"

#include <cred.hpp>


namespace memex::admin {

void register_notify_routes(drogon::HttpAppFramework& app,
                            const std::shared_ptr<AdminContext>& ctx,
                            const Guarded& guarded) {
  // ============ 通知与集成（org-admin；波次 2 第 1 条） ============
  // webhook/bot/模型网关管理面（与 CLI webhook|bot|model 子命令同径直连库）。
  // 凭据口径（用户令 2026-10-12）：webhook/bot token 明文只在创建应答里
  // 出现一次，库与后续所有接口只出 sha256 摘要前缀（与 CLI list「token
  // 摘要前缀」同口径 12 位）；模型端点 api_key 库存原文（须可逆代发上游，
  // store 既有口径），管理面只写不读——任何应答不回显。

  guarded("/api/admin/webhooks", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "webhooks.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& w : ctx->store->webhook_list()) {
              Json::Value row;
              row["id"] = w.id;
              row["target"] = w.target;
              row["name"] = w.name;
              row["token_prefix"] = w.token_hash.substr(0, 12); // 摘要前缀，非明文
              row["created_ms"] = w.created_ms;
              row["revoked"] = w.revoked;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["webhooks"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/webhooks", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "webhooks.create")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string target = str_or(*j, "target");
            const std::string name = str_or(*j, "name");
            if (target.empty()) {
              cb(err_resp(drogon::k400BadRequest,
                          "target 必填（账号 或 group:群号）"));
              return;
            }
            // 建即校验目标（与 CLI create 同口径：防错绑不存在的账号/群）
            bool target_ok = false;
            if (target.rfind("group:", 0) == 0) {
              const auto gid = static_cast<std::uint64_t>(
                  std::strtoull(target.c_str() + 6, nullptr, 10));
              std::lock_guard<std::mutex> lk(ctx->mu);
              target_ok = ctx->store->group_info(gid).has_value();
            } else {
              std::lock_guard<std::mutex> lk(ctx->mu);
              target_ok = ctx->store->find_account(target).has_value();
            }
            if (!target_ok) {
              cb(err_resp(drogon::k404NotFound,
                          "目标不存在（账号 或 group:群号）"));
              return;
            }
            // token 明文仅本应答一次；库与台账只存 sha256 摘要
            const std::string token =
                std::string("whk_") + memex::server::random_salt_hex();
            std::int64_t id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              id = ctx->store->webhook_create(memex::server::sha256_hex(token),
                                              target, name, now_ms());
            }
            if (id == 0) {
              cb(err_resp(drogon::k409Conflict, "webhook 台账写入失败"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] webhook.create id=" << id
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["id"] = id;
            out["token"] = token; // 仅此一次显示
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/webhooks/revoke", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "webhooks.revoke")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = ctx->store->webhook_revoke(int_or(*j, "id"));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "webhook 行不存在或已吊销"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] webhook.revoke id=" << int_or(*j, "id")
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/bots", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "bots.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& b : ctx->store->bot_list()) {
              Json::Value row;
              row["name"] = b.name;
              row["full_name"] = "bot:" + b.name;
              row["created_by"] = b.created_by;
              row["token_prefix"] = b.token_hash.substr(0, 12); // 摘要前缀，非明文
              row["created_ms"] = b.created_ms;
              row["disabled"] = b.disabled;
              Json::Value groups(Json::arrayValue);
              for (const auto g : ctx->store->bot_groups_allowed(b.name))
                groups.append(static_cast<Json::Int64>(g));
              row["groups_allowed"] = groups;
              Json::Value depts(Json::arrayValue);
              for (const auto& d : ctx->store->bot_depts_allowed(b.name))
                depts.append(d);
              row["depts_allowed"] = depts;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["bots"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/bots", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "bots.create")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string name = str_or(*j, "name");
            if (name.empty()) {
              cb(err_resp(drogon::k400BadRequest, "name 必填"));
              return;
            }
            if (name.find(':') != std::string::npos) {
              cb(err_resp(drogon::k400BadRequest,
                          "bot 名不得含 ':'（前缀 bot: 为系统保留）"));
              return;
            }
            // token 明文仅本应答一次；库只存 sha256 摘要（CLI add 同口径）
            const std::string token =
                std::string("bot_") + memex::server::random_salt_hex();
            std::int64_t id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              id = ctx->store->bot_add(name, memex::server::sha256_hex(token),
                                       session_account(req), now_ms());
            }
            if (id == 0) {
              cb(err_resp(drogon::k409Conflict, "bot 创建失败（重名）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] bot.create name=" << name
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["id"] = id;
            out["name"] = name;
            out["token"] = token; // 仅此一次显示
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/bots/remove", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "bots.remove")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = ctx->store->bot_remove(str_or(*j, "name"));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "无此 bot"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] bot.remove name=" << str_or(*j, "name")
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/bots/disabled", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "bots.disabled")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = ctx->store->bot_set_disabled(str_or(*j, "name"),
                                                  bool_or(*j, "disabled", true));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "无此 bot"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // 模型端点：api_key 只写不读——list/任何应答不回显原文，只出
  // api_key_set 布尔（base_url/model/本地标记为非敏感登记项照常出）。
  guarded("/api/admin/model/endpoints", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "model.endpoints.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& e : ctx->store->model_endpoints_list()) {
              Json::Value row;
              row["id"] = e.id;
              row["name"] = e.name;
              row["base_url"] = e.base_url;
              row["model"] = e.model;
              row["api_key_set"] = !e.api_key.empty(); // 只出有无，不出原文
              row["is_local"] = e.is_local;
              row["enabled"] = e.enabled;
              row["created_by"] = e.created_by;
              row["created_ms"] = e.created_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["endpoints"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/model/endpoints", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "model.endpoints.add")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string name = str_or(*j, "name");
            const std::string base_url = str_or(*j, "base_url");
            if (name.empty() || base_url.empty()) {
              cb(err_resp(drogon::k400BadRequest, "name 与 base_url 必填"));
              return;
            }
            if (name.find(':') != std::string::npos) {
              cb(err_resp(drogon::k400BadRequest, "端点名不得含 ':'"));
              return;
            }
            if (base_url.rfind("http://", 0) != 0) {
              cb(err_resp(drogon::k400BadRequest,
                          "base_url 须以 http:// 开头（内网口径）"));
              return;
            }
            memex::server::ModelEndpointRow ep;
            ep.name = name;
            ep.base_url = base_url;
            ep.api_key = str_or(*j, "api_key"); // 只入库（代发上游），不回显
            ep.model = str_or(*j, "model");
            ep.is_local = bool_or(*j, "is_local");
            ep.created_by = session_account(req); // 登记者留痕（CLI --by 同位）
            ep.created_ms = now_ms();
            std::int64_t id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              id = ctx->store->model_endpoint_add(ep);
            }
            if (id == 0) {
              cb(err_resp(drogon::k409Conflict, "端点登记失败（重名）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] model.endpoint.add name=" << name
                      << " local=" << (ep.is_local ? "1" : "0")
                      << " by=" << ep.created_by << "\n";
            Json::Value out = ok_json();
            out["id"] = id;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/model/endpoints/enabled", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "model.endpoints.enabled")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = ctx->store->model_endpoint_set_enabled(
                  str_or(*j, "name"), bool_or(*j, "enabled", true));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "端点不存在"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/model/endpoints/remove", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "model.endpoints.remove")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = ctx->store->model_endpoint_remove(str_or(*j, "name"));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "端点不存在"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // 模型调用审计（只读）：一次上游尝试一行，元数据不含正文。
  guarded("/api/admin/model/calls", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "model.calls.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string limit_s = req->getParameter("limit");
            const int limit = std::clamp(
                limit_s.empty() ? 50 : atoi(limit_s.c_str()), 1, 500);
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& c : ctx->store->model_calls_list(limit)) {
              Json::Value row;
              row["id"] = c.id;
              row["caller"] = c.caller;
              row["endpoint"] = c.endpoint;
              row["model"] = c.model;
              row["archive_scope"] = c.archive_scope;
              row["prompt_chars"] = c.prompt_chars;
              row["completion_chars"] = c.completion_chars;
              row["status"] = c.status;
              row["latency_ms"] = c.latency_ms;
              row["created_ms"] = c.created_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["calls"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  // 三级通知偏好只读盘点（用户令：本期只读展示，不提供写入口）。口径
  // 如实：客户端本机偏好（弹窗三级/免打扰/提示音）存于各客户端
  // QSettings，服务端无集中存储；服务端通知面=webhook（个人/群两级
  // 目标）＋bot（群/部门白名单）＋模型网关。本接口按 全局/群/个人
  // 三级汇总台账现值，纯读不改。
  guarded("/api/admin/notify-overview", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "notify.overview")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value out = ok_json();
            out["note"] =
                "客户端本机通知偏好（弹窗三级/免打扰/提示音）存于各客户端，"
                "服务端不集中存储；本页为服务端通知接入台账的三级只读盘点，"
                "本期不提供写入口";
            Json::Value group_scope(Json::arrayValue);  // 群级：group:* webhook
            Json::Value personal_scope(Json::arrayValue); // 个人级：账号 webhook
            Json::Value bot_scopes(Json::arrayValue);   // 群/部门白名单管控面
            int wh_total = 0, wh_active = 0;
            int bot_total = 0, bot_enabled = 0;
            int ep_total = 0, ep_enabled = 0, ep_local = 0;
            std::lock_guard<std::mutex> lk(ctx->mu);
            const auto webhooks = ctx->store->webhook_list();
            wh_total = static_cast<int>(webhooks.size());
            for (const auto& w : webhooks) {
              if (!w.revoked) ++wh_active;
              Json::Value row;
              row["id"] = w.id;
              row["target"] = w.target;
              row["name"] = w.name;
              row["token_prefix"] = w.token_hash.substr(0, 12);
              row["created_ms"] = w.created_ms;
              row["revoked"] = w.revoked;
              if (w.target.rfind("group:", 0) == 0) {
                // 群名解析（群已散则无 group_name，原样留 target）
                const auto gid = static_cast<std::uint64_t>(
                    std::strtoull(w.target.c_str() + 6, nullptr, 10));
                if (const auto g = ctx->store->group_info(gid))
                  row["group_name"] = g->name;
                group_scope.append(row);
              } else {
                personal_scope.append(row);
              }
            }
            const auto bots = ctx->store->bot_list();
            bot_total = static_cast<int>(bots.size());
            for (const auto& b : bots) {
              if (!b.disabled) ++bot_enabled;
              Json::Value row;
              row["name"] = b.name;
              row["disabled"] = b.disabled;
              Json::Value groups(Json::arrayValue);
              for (const auto g : ctx->store->bot_groups_allowed(b.name))
                groups.append(static_cast<Json::Int64>(g));
              row["groups_allowed"] = groups;
              Json::Value depts(Json::arrayValue);
              for (const auto& d : ctx->store->bot_depts_allowed(b.name))
                depts.append(d);
              row["depts_allowed"] = depts;
              bot_scopes.append(row);
            }
            const auto endpoints = ctx->store->model_endpoints_list();
            ep_total = static_cast<int>(endpoints.size());
            for (const auto& e : endpoints) {
              if (e.enabled) ++ep_enabled;
              if (e.is_local) ++ep_local;
            }
            Json::Value global;
            global["webhooks_total"] = wh_total;
            global["webhooks_active"] = wh_active;
            global["bots_total"] = bot_total;
            global["bots_enabled"] = bot_enabled;
            global["endpoints_total"] = ep_total;
            global["endpoints_enabled"] = ep_enabled;
            global["endpoints_local"] = ep_local;
            out["global"] = global;
            out["group_scope"] = group_scope;
            out["personal_scope"] = personal_scope;
            out["bots"] = bot_scopes;
            cb(json_resp(drogon::k200OK, out));
          });
}

} // namespace memex::admin
