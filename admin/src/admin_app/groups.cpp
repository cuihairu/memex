// 群组管理面路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"

#include <cred.hpp>


namespace memex::admin {

void register_group_routes(drogon::HttpAppFramework& app,
                           const std::shared_ptr<AdminContext>& ctx,
                           const Guarded& guarded) {
  // ============ 群组管理面（org-admin；波次 2 第 3 条） ============
  // 群台账与能力开关（CLI group list/capability 同径直连库）＋群 CI
  // （流水线/触发/run 台账）＋群服务器与远程会话（登记 token 仅一次、
  // 会话台账只读）＋群密码箱（信息/acl/rekey/审计——密文与包裹块不回
  // 显）＋群备忘录（列表/修订史/回滚=一次编辑留痕）＋投票（匿名票不回
  // 带 voter 身份——延续成员面承诺）/群任务/群工具凭证（掩码元数据）。
  // 口径如实：org-admin=组织级管理面，直连库即 CLI 同权，不过成员面的
  // 能力闸与工具白名单（开关本身就在本页操作）；admin 进程不持 webhook
  // 分发器——CI 触发只落台账，结果卡片回群走 files 面触发路径。

  guarded("/api/admin/groups", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "groups.list")) {
              cb(std::move(*deny));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            Json::Value arr(Json::arrayValue);
            for (const auto& g : ctx->store->groups_list()) {
              Json::Value row;
              row["gid"] = static_cast<Json::Int64>(g.group_id);
              row["name"] = g.name;
              row["owner"] = g.owner;
              // 群组-1：群资料主题/目的同源回带（空=未填）
              row["topic"] = g.topic;
              row["purpose"] = g.purpose;
              Json::Value members(Json::arrayValue);
              Json::Value admins(Json::arrayValue);
              for (const auto& m : g.members) {
                members.append(m);
                if (ctx->store->group_role(g.group_id, m) == "admin") {
                  admins.append(m);
                }
              }
              row["members"] = members;
              row["admins"] = admins;
              row["memo_open_edit"] =
                  ctx->store->group_memo_open_edit(g.group_id);
              // 能力：词汇表逐项给配置行＋生效值（未配置=现行允许）
              Json::Value caps(Json::arrayValue);
              for (const auto& n :
                   memex::server::ServerStore::group_capability_names()) {
                Json::Value c;
                c["capability"] = n;
                c["configured"] = false;
                c["effective"] =
                    ctx->store->group_capability_enabled(g.group_id, n);
                for (const auto& cr :
                     ctx->store->group_capabilities_list(g.group_id)) {
                  if (cr.capability == n) {
                    c["configured"] = true;
                    c["configured_enabled"] = cr.enabled;
                    c["updated_by"] = cr.updated_by;
                    c["updated_ms"] =
                        static_cast<Json::Int64>(cr.updated_ms);
                    break;
                  }
                }
                caps.append(c);
              }
              row["capabilities"] = caps;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["groups"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/groups/capability", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "groups.capability")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t gid = int_or(*j, "gid");
            const std::string cap = str_or(*j, "capability");
            const bool enabled = bool_or(*j, "enabled", true);
            if (gid <= 0 || cap.empty()) {
              cb(err_resp(drogon::k400BadRequest,
                          "缺少字段：gid/capability"));
              return;
            }
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              if (!ctx->store->group_info(
                      static_cast<std::uint64_t>(gid))) {
                cb(err_resp(drogon::k404NotFound, "群不存在"));
                return;
              }
              bool known = false;
              std::string vocab;
              for (const auto& n :
                   memex::server::ServerStore::group_capability_names()) {
                vocab += (vocab.empty() ? "" : ",") + n;
                if (n == cap) known = true;
              }
              if (!known) {
                cb(err_resp(drogon::k400BadRequest,
                            "能力名不在词汇表（" + vocab + "）"));
                return;
              }
              // store 层变更落查阅台账（action=group.capability，CLI
              // group capability set 同径留痕）
              if (!ctx->store->group_capability_set(
                      static_cast<std::uint64_t>(gid), cap, enabled,
                      session_account(req), now_ms())) {
                cb(err_resp(drogon::k500InternalServerError, "写入失败"));
                return;
              }
            }
            std::cout << "[MEMEX-ADMIN] groups.capability gid=" << gid
                      << " capability=" << cap
                      << " enabled=" << (enabled ? "1" : "0")
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["gid"] = gid;
            out["capability"] = cap;
            out["enabled"] = enabled;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-ci", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-ci.list")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            Json::Value pipes(Json::arrayValue);
            for (const auto& p : ctx->store->ci_pipeline_list(gid)) {
              Json::Value row;
              row["name"] = p.name;
              row["description"] = p.description;
              row["updated_by"] = p.updated_by;
              row["updated_ms"] = static_cast<Json::Int64>(p.updated_ms);
              // 红绿灯=该流水线最近一笔 run（与 files 面口径一致）
              const auto last = ctx->store->ci_run_list(gid, p.name, 1);
              row["last_status"] = last.empty() ? "" : last[0].status;
              row["last_ts"] = last.empty()
                                   ? Json::Int64(0)
                                   : static_cast<Json::Int64>(last[0].ts_ms);
              pipes.append(row);
            }
            Json::Value runs(Json::arrayValue);
            for (const auto& r : ctx->store->ci_run_list(gid, "", 50)) {
              Json::Value row;
              row["id"] = static_cast<Json::Int64>(r.id);
              row["pipeline"] = r.pipeline;
              row["actor"] = r.actor;
              row["status"] = r.status;
              row["ts_ms"] = static_cast<Json::Int64>(r.ts_ms);
              row["params_json"] = r.params_json;
              row["result_json"] = r.result_json;
              runs.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["pipelines"] = pipes;
            out["runs"] = runs;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-ci/trigger", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-ci.trigger")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t gid = int_or(*j, "gid");
            const std::string pipeline = str_or(*j, "pipeline");
            if (gid <= 0 || pipeline.empty()) {
              cb(err_resp(drogon::k400BadRequest,
                          "缺少字段：gid/pipeline"));
              return;
            }
            const Json::Value params =
                j->isMember("params") && (*j)["params"].isObject()
                    ? (*j)["params"]
                    : Json::Value(Json::objectValue);
            // stub 执行器与 files 面同语义：params.fail=true 即失败
            const bool fail = params.isMember("fail") &&
                              params["fail"].isBool() &&
                              params["fail"].asBool();
            const std::string status = fail ? "failed" : "success";
            Json::Value result(Json::objectValue);
            result["stub"] = true;
            result["status"] = status;
            std::int64_t run_id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              if (!ctx->store->group_info(
                      static_cast<std::uint64_t>(gid))) {
                cb(err_resp(drogon::k404NotFound, "群不存在"));
                return;
              }
              bool exists = false;
              for (const auto& p : ctx->store->ci_pipeline_list(
                       static_cast<std::uint64_t>(gid))) {
                if (p.name == pipeline) {
                  exists = true;
                  break;
                }
              }
              if (!exists) {
                cb(err_resp(drogon::k404NotFound, "流水线不存在"));
                return;
              }
              Json::StreamWriterBuilder wb;
              const std::string params_s = Json::writeString(wb, params);
              const std::string result_s = Json::writeString(wb, result);
              const std::int64_t now = now_ms();
              run_id = ctx->store->ci_run_add(
                  static_cast<std::uint64_t>(gid), pipeline,
                  session_account(req), status, params_s, result_s, now);
              // 动作留痕进 R25-1 工具审计（谁触发可回溯，与 files 面同源）
              ctx->store->tool_audit_add(
                  static_cast<std::uint64_t>(gid), "ci", "trigger",
                  session_account(req), Json::writeString(wb, *j), result_s,
                  now);
            }
            std::cout << "[MEMEX-ADMIN] group-ci.trigger gid=" << gid
                      << " pipeline=" << pipeline << " status=" << status
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["gid"] = gid;
            out["run_id"] = run_id;
            out["pipeline"] = pipeline;
            out["actor"] = session_account(req);
            out["status"] = status;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-servers", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-servers.list")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            // 在线红绿灯与 files 面同口径（90s 无心跳=离线）
            constexpr std::int64_t kAgentOnlineMs = 90 * 1000;
            const std::int64_t now = now_ms();
            Json::Value arr(Json::arrayValue);
            for (const auto& r : ctx->store->server_list(gid)) {
              Json::Value row;
              row["id"] = static_cast<Json::Int64>(r.id);
              row["name"] = r.name;
              row["host"] = r.host;
              row["enrolled_by"] = r.enrolled_by;
              row["created_ms"] = static_cast<Json::Int64>(r.created_ms);
              row["last_seen_ms"] = static_cast<Json::Int64>(r.last_seen_ms);
              row["online"] = now - r.last_seen_ms < kAgentOnlineMs;
              row["cpu_percent"] = r.cpu_percent;
              row["mem_used_mb"] = r.mem_used_mb;
              row["mem_total_mb"] = r.mem_total_mb;
              row["disk_used_mb"] = r.disk_used_mb;
              row["disk_total_mb"] = r.disk_total_mb;
              row["load1"] = r.load1;
              // 凭据掩码元数据：只言「是否已配置/谁/何时」——密文不出门
              row["cred"] = !r.cred_updated_by.empty();
              row["cred_updated_by"] = r.cred_updated_by;
              row["cred_updated_ms"] =
                  static_cast<Json::Int64>(r.cred_updated_ms);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["servers"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-servers/enroll", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny =
                    require_manage(*ctx, req, "group-servers.enroll")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t gid = int_or(*j, "gid");
            const std::string name = str_or(*j, "name");
            const std::string host = str_or(*j, "host");
            if (gid <= 0 || name.empty() || host.empty()) {
              cb(err_resp(drogon::k400BadRequest,
                          "缺少字段：gid/name/host"));
              return;
            }
            // 注册令牌只在登记回包出现一次（库存 SHA-256 摘要，CLI/文件
            // 面同口径）；同 gid+name 重登记=轮换令牌、id 稳定
            const std::string token = memex::server::random_salt_hex();
            std::uint64_t id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              if (!ctx->store->group_info(
                      static_cast<std::uint64_t>(gid))) {
                cb(err_resp(drogon::k404NotFound, "群不存在"));
                return;
              }
              id = ctx->store->server_enroll(
                  static_cast<std::uint64_t>(gid), name, host,
                  memex::server::sha256_hex(token), session_account(req),
                  now_ms());
            }
            if (id == 0) {
              cb(err_resp(drogon::k404NotFound, "登记失败"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] group-servers.enroll gid=" << gid
                      << " id=" << id << " name=" << name << " host=" << host
                      << " by=" << session_account(req) << "\n"; // 令牌不进日志
            Json::Value out = ok_json();
            out["gid"] = gid;
            out["id"] = static_cast<Json::Int64>(id);
            out["name"] = name;
            out["token"] = token;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-servers/sessions", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny =
                    require_manage(*ctx, req, "group-servers.sessions")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            for (const auto& s : ctx->store->server_session_list(gid)) {
              Json::Value row;
              row["id"] = static_cast<Json::Int64>(s.id);
              row["server_id"] = static_cast<Json::Int64>(s.server_id);
              row["server_name"] = s.server_name;
              row["host"] = s.host;
              row["actor"] = s.actor;
              row["protocol"] = s.protocol;
              row["opened_ms"] = static_cast<Json::Int64>(s.opened_ms);
              row["redeemed"] = s.redeemed_ms > 0;
              row["open"] = s.closed_ms == 0;
              row["duration_ms"] = static_cast<Json::Int64>(
                  s.closed_ms > 0 ? s.closed_ms - s.opened_ms : 0);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["sessions"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-vault", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-vault.info")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            Json::Value out = ok_json();
            const auto v = ctx->store->group_vault_info(gid);
            out["exists"] = v.has_value();
            if (v.has_value()) {
              // kdf_salt/wrapped_dek 不回显（后台无解锁用途，密文不出门；
              // 成员面经既有 files 路由取包裹块）
              out["gid"] = static_cast<Json::Int64>(gid);
              out["kdf_iters"] = v->kdf_iters;
              out["created_ms"] = static_cast<Json::Int64>(v->created_ms);
              out["updated_ms"] = static_cast<Json::Int64>(v->updated_ms);
              Json::Value acl(Json::arrayValue);
              for (const auto& a : ctx->store->vault_acl_list(gid)) {
                acl.append(a);
              }
              out["acl"] = acl;
              Json::Value entries(Json::arrayValue);
              for (const auto& e : ctx->store->vault_list_entries(gid)) {
                Json::Value row;
                row["id"] = e.id;
                row["name"] = e.name;
                row["account_name"] = e.account_name;
                row["created_by"] = e.created_by;
                row["created_ms"] = static_cast<Json::Int64>(e.created_ms);
                row["updated_ms"] = static_cast<Json::Int64>(e.updated_ms);
                entries.append(row); // secret_* 密文不回显
              }
              out["entries"] = entries;
            }
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-vault/acl", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-vault.acl")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t gid = int_or(*j, "gid");
            if (gid <= 0 || !j->isMember("accounts") ||
                !(*j)["accounts"].isArray()) {
              cb(err_resp(drogon::k400BadRequest,
                          "缺少字段：gid/accounts（数组，空=恢复全成员）"));
              return;
            }
            std::vector<std::string> accounts;
            for (const auto& a : (*j)["accounts"]) {
              if (!a.isString()) {
                cb(err_resp(drogon::k400BadRequest,
                            "accounts 须为字符串数组"));
                return;
              }
              accounts.push_back(a.asString());
            }
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              if (!ctx->store->group_info(
                      static_cast<std::uint64_t>(gid))) {
                cb(err_resp(drogon::k404NotFound, "群不存在"));
                return;
              }
              if (!ctx->store->group_vault_info(
                      static_cast<std::uint64_t>(gid))) {
                cb(err_resp(drogon::k404NotFound, "密码箱未建立"));
                return;
              }
              // 事务替换；空名单=清名单恢复全成员（与 files 面同径）。
              // 成员面授权名单=群主专属（vault:config 显式排除群管理员）；
              // 本面=org 级治理入口（CLI 同权），操作留会话账号痕。
              if (!ctx->store->vault_set_acl(
                      static_cast<std::uint64_t>(gid), accounts)) {
                cb(err_resp(drogon::k500InternalServerError, "名单写入失败"));
                return;
              }
            }
            std::cout << "[MEMEX-ADMIN] group-vault.acl gid=" << gid
                      << " size=" << accounts.size()
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["gid"] = gid;
            out["size"] = static_cast<Json::Int64>(accounts.size());
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-vault/rekey", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-vault.rekey")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t gid = int_or(*j, "gid");
            const std::string salt = str_or(*j, "kdf_salt");
            const std::int64_t iters = int_or(*j, "kdf_iters");
            const std::string wrapped = str_or(*j, "wrapped_dek");
            if (gid <= 0 || salt.empty() || wrapped.empty() || iters < 10000) {
              cb(err_resp(drogon::k400BadRequest,
                          "缺少字段：gid/kdf_salt/kdf_iters(≥10000)/"
                          "wrapped_dek"));
              return;
            }
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              if (!ctx->store->group_info(
                      static_cast<std::uint64_t>(gid))) {
                cb(err_resp(drogon::k404NotFound, "群不存在"));
                return;
              }
              if (!ctx->store->group_vault_info(
                      static_cast<std::uint64_t>(gid))) {
                cb(err_resp(drogon::k404NotFound, "密码箱未建立"));
                return;
              }
              // 无损重包裹是客户端仪式（同 DEK 新口令重包裹，成员面既有
              // rekey 路径同口径）——后台只落仪式产物；误投包裹块=成员暂
              // 失解锁（条目仍密文在库，可再 rekey 纠正）
              if (!ctx->store->group_vault_rekey(
                      static_cast<std::uint64_t>(gid), salt,
                      static_cast<int>(iters), wrapped, now_ms())) {
                cb(err_resp(drogon::k500InternalServerError, "重置失败"));
                return;
              }
            }
            std::cout << "[MEMEX-ADMIN] group-vault.rekey gid=" << gid
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["gid"] = gid;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-vault/audit", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-vault.audit")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            for (const auto& a : ctx->store->vault_audit_list(gid, 200)) {
              Json::Value row;
              row["id"] = static_cast<Json::Int64>(a.id);
              row["entry_id"] = static_cast<Json::Int64>(a.entry_id);
              row["actor"] = a.actor;
              row["action"] = a.action; // reveal=查看 | copy=复制
              row["ts_ms"] = static_cast<Json::Int64>(a.ts_ms);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["rows"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-memos", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-memo.list")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            for (const auto& m : ctx->store->list_group_memos(gid)) {
              Json::Value row;
              row["id"] = m.id;
              row["title"] = m.title;
              row["content"] = m.content;
              row["author"] = m.author;
              row["created_ms"] = static_cast<Json::Int64>(m.created_ms);
              row["updated_ms"] = static_cast<Json::Int64>(m.updated_ms);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["memos"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-memo/history", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-memo.history")) {
              cb(std::move(*deny));
              return;
            }
            const std::string id_s = req->getParameter("id");
            if (id_s.empty() ||
                id_s.find_first_not_of("0123456789") != std::string::npos) {
              cb(err_resp(drogon::k400BadRequest, "id 须为数字"));
              return;
            }
            const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
            std::lock_guard<std::mutex> lk(ctx->mu);
            const auto row = ctx->store->group_memo_by_id(id);
            if (!row.has_value()) {
              cb(err_resp(drogon::k404NotFound, "备忘录不存在"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            for (const auto& r : ctx->store->group_memo_history(id)) {
              Json::Value rev;
              rev["id"] = r.id;
              rev["title"] = r.title;
              rev["content"] = r.content;
              rev["editor"] = r.editor;
              rev["ts_ms"] = static_cast<Json::Int64>(r.ts_ms);
              arr.append(rev);
            }
            Json::Value out = ok_json();
            out["id"] = id;
            out["gid"] = static_cast<Json::Int64>(row->group_id);
            out["revisions"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-memo/rollback", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-memo.rollback")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t id = int_or(*j, "id");
            const std::int64_t rev_id = int_or(*j, "revision_id");
            if (id <= 0 || rev_id <= 0) {
              cb(err_resp(drogon::k400BadRequest,
                          "缺少字段：id/revision_id"));
              return;
            }
            std::uint64_t gid = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              const auto row = ctx->store->group_memo_by_id(id);
              if (!row.has_value()) {
                cb(err_resp(drogon::k404NotFound, "备忘录不存在"));
                return;
              }
              gid = row->group_id;
              // 回滚即一次编辑：取目标笔全文写回并落新笔（editor=回滚
              // 者）——留痕链不断，与 files 面同径
              const auto hist = ctx->store->group_memo_history(id);
              bool found = false;
              for (const auto& r : hist) {
                if (r.id == rev_id) {
                  if (!ctx->store->update_group_memo(
                          id, r.title, r.content, session_account(req),
                          now_ms())) {
                    cb(err_resp(drogon::k500InternalServerError, "回滚失败"));
                    return;
                  }
                  found = true;
                  break;
                }
              }
              if (!found) {
                cb(err_resp(drogon::k404NotFound, "修订笔不存在"));
                return;
              }
            }
            std::cout << "[MEMEX-ADMIN] group-memo.rollback gid=" << gid
                      << " id=" << id << " to_revision=" << rev_id
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["id"] = id;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-polls", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-polls.list")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            const std::int64_t now = now_ms();
            Json::Value arr(Json::arrayValue);
            for (const auto& p : ctx->store->polls_list(gid)) {
              Json::Value row;
              row["id"] = p.id;
              row["topic"] = p.topic;
              Json::Value options(Json::arrayValue);
              for (const auto& o : p.options) options.append(o);
              row["options"] = options;
              row["deadline_ms"] = static_cast<Json::Int64>(p.deadline_ms);
              row["closed"] = p.closed;
              row["anonymous"] = p.anonymous;
              row["multi"] = p.multi;
              row["status"] =
                  p.closed || (p.deadline_ms > 0 && now >= p.deadline_ms)
                      ? "closed"
                      : "open"; // 到点现算（惰性判定，与 files 面同口径）
              row["created_by"] = p.created_by;
              row["created_ms"] = static_cast<Json::Int64>(p.created_ms);
              // 计数恒回；匿名票不回带 voter 身份（延续成员面匿名承诺，
              // 库内留 account 供改票与审计）
              const int n = static_cast<int>(p.options.size());
              Json::Value counts(Json::arrayValue);
              for (int i = 0; i < n; ++i) counts.append(0);
              Json::Value votes(Json::arrayValue);
              for (const auto& v : ctx->store->poll_votes(p.id)) {
                if (p.multi) {
                  if (v.choice >= 1 && v.choice < (1 << n)) {
                    for (int i = 0; i < n; ++i) {
                      if ((v.choice >> i) & 1) {
                        counts[i] = counts[i].asInt() + 1;
                      }
                    }
                  }
                } else if (v.choice >= 1 && v.choice <= n) {
                  counts[v.choice - 1] = counts[v.choice - 1].asInt() + 1;
                }
                if (!p.anonymous) {
                  Json::Value vote;
                  vote["account"] = v.account;
                  vote["choice"] = v.choice;
                  vote["ts_ms"] = static_cast<Json::Int64>(v.ts_ms);
                  votes.append(vote);
                }
              }
              row["counts"] = counts;
              row["votes"] = votes;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["polls"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-polls/close", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-polls.close")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t poll_id = int_or(*j, "poll_id");
            if (poll_id <= 0) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：poll_id"));
              return;
            }
            std::int64_t gid = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              const auto p = ctx->store->poll_by_id(poll_id);
              if (!p.has_value()) {
                cb(err_resp(drogon::k404NotFound, "投票不存在"));
                return;
              }
              gid = static_cast<std::int64_t>(p->group_id);
              // 幂等收口（已关仍 true，store 既有口径）
              ctx->store->poll_close(poll_id);
            }
            std::cout << "[MEMEX-ADMIN] group-polls.close gid=" << gid
                      << " poll=" << poll_id
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["poll_id"] = poll_id;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-tasks", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "group-tasks.list")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            for (const auto& t : ctx->store->gtasks_list(gid)) {
              Json::Value row;
              row["id"] = t.id;
              row["title"] = t.title;
              row["assignee"] = t.assignee;
              row["due_ms"] = static_cast<Json::Int64>(t.due_ms);
              row["claimed_ms"] = static_cast<Json::Int64>(t.claimed_ms);
              row["status"] = t.status;
              row["created_by"] = t.created_by;
              row["created_ms"] = static_cast<Json::Int64>(t.created_ms);
              row["done_by"] = t.done_by;
              row["done_ms"] = static_cast<Json::Int64>(t.done_ms);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["tasks"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-tools/credentials", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny =
                    require_manage(*ctx, req, "group-tools.credentials")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            // 掩码元数据面（工具/谁/何时）——sealed_hex 与明文永不出门
            //（store 只给元数据，密文取回是服务端代理调用内存内路径）
            Json::Value arr(Json::arrayValue);
            for (const auto& m : ctx->store->tool_cred_list(gid)) {
              Json::Value row;
              row["tool"] = m.tool;
              row["updated_by"] = m.updated_by;
              row["updated_ms"] = static_cast<Json::Int64>(m.updated_ms);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["credentials"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/group-tools/audit", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny =
                    require_manage(*ctx, req, "group-tools.audit")) {
              cb(std::move(*deny));
              return;
            }
            std::uint64_t gid = 0;
            if (!parse_gid_q(req->getParameter("gid"), &gid)) {
              cb(err_resp(drogon::k400BadRequest, "gid 须为正整数群号"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->group_info(gid)) {
              cb(err_resp(drogon::k404NotFound, "群不存在"));
              return;
            }
            // 工具动作留痕（谁/何时/何动作/参数/结果——铁律：点击可回溯
            // 到人）；admin 触发的 CI 也落这里
            Json::Value arr(Json::arrayValue);
            for (const auto& a : ctx->store->tool_audit_list(gid, 200)) {
              Json::Value row;
              row["id"] = static_cast<Json::Int64>(a.id);
              row["tool"] = a.tool;
              row["action"] = a.action;
              row["actor"] = a.actor;
              row["params_json"] = a.params_json;
              row["result_json"] = a.result_json;
              row["ts_ms"] = static_cast<Json::Int64>(a.ts_ms);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["gid"] = static_cast<Json::Int64>(gid);
            out["rows"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });
}

} // namespace memex::admin
