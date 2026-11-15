// 账号与组织、策略下发、留存策略（purge 双人审批）路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"


namespace memex::admin {

void register_account_org_routes(drogon::HttpAppFramework& app,
                                 const std::shared_ptr<AdminContext>& ctx,
                                 const Guarded& guarded) {
  // ============ 账号与组织（org-admin） ============
  guarded("/api/admin/accounts", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "accounts.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& [acct, label] : ctx->store->account_list()) {
              Json::Value row;
              row["account"] = acct;
              row["label"] = label;
              Json::Value roles(Json::arrayValue);
              for (const auto& r : ctx->store->effective_roles(acct, now_ms()))
                roles.append(r);
              row["roles"] = roles;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["accounts"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/accounts", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "accounts.create")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string account = str_or(*j, "account");
            const std::string password = str_or(*j, "password");
            std::string name = str_or(*j, "name", account);
            const std::string role =
                str_or(*j, "role", "member");
            if (account.empty() || password.empty()) {
              cb(err_resp(drogon::k400BadRequest, "账号与口令不能为空"));
              return;
            }
            if (role != "admin" && role != "member") {
              cb(err_resp(drogon::k400BadRequest,
                          "role 只接受 admin|member（基础角色）；org-admin/"
                          "auditor 等管理角色走「角色授权」追加"));
              return;
            }
            bool created = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              created = ctx->store->create_account(account, password, name, role);
            }
            if (!created) {
              cb(err_resp(drogon::k409Conflict,
                          "建号失败（账号已存在或写库失败）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] account.create account=" << account
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/departments", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr&,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& [id, path] : ctx->store->department_list()) {
              Json::Value row;
              row["id"] = id;
              row["path"] = path;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["departments"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/departments", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "departments.create")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string path = str_or(*j, "path");
            if (path.empty()) {
              cb(err_resp(drogon::k400BadRequest, "path 不能为空"));
              return;
            }
            int id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              id = ctx->store->ensure_department_path(path);
            }
            Json::Value out = ok_json();
            out["id"] = id;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/membership", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "membership.set")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string account = str_or(*j, "account");
            const std::int64_t dept_id = int_or(*j, "department_id");
            const bool add = str_or(*j, "op", "add") == "add";
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = add ? ctx->store->membership_add(account,
                                                 static_cast<int>(dept_id),
                                                 now_ms())
                         : ctx->store->membership_remove(
                               account, static_cast<int>(dept_id));
            }
            if (!done) {
              cb(err_resp(drogon::k409Conflict,
                          add ? "加部门失败（账号/部门不存在或已关联）"
                              : "移除失败（关联不存在）"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/profile", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "profile.set")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string account = str_or(*j, "account");
            const std::int64_t dept_id = int_or(*j, "department_id");
            const std::string title = str_or(*j, "title");
            const std::string manager = str_or(*j, "manager");
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = ctx->store->set_member_profile(
                  account, static_cast<int>(dept_id), title, manager);
            }
            if (!done) {
              cb(err_resp(drogon::k409Conflict,
                          "建档失败（账号不存在/自为上级/构成汇报环）"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // 角色授权台账（含查询与授予/吊销）
  guarded("/api/admin/roles", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "roles.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string account = req->getParameter("account");
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& r : ctx->store->role_assignments(account)) {
              Json::Value row;
              row["id"] = r.id;
              row["account"] = r.account;
              row["role"] = r.role;
              row["scope"] = r.scope;
              row["valid_from_ms"] = r.valid_from_ms;
              row["valid_until_ms"] = r.valid_until_ms;
              row["granted_by"] = r.granted_by;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["assignments"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/roles", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "roles.grant")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string account = str_or(*j, "account");
            const std::string role = str_or(*j, "role");
            if (account.empty() || role.empty()) {
              cb(err_resp(drogon::k400BadRequest, "account 与 role 必填"));
              return;
            }
            std::int64_t id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              id = ctx->store->role_grant(account, role, str_or(*j, "scope"),
                                     int_or(*j, "valid_from_ms"),
                                     int_or(*j, "valid_until_ms"),
                                     session_account(req), now_ms());
            }
            if (id == 0) {
              cb(err_resp(drogon::k409Conflict,
                          "授权失败（账号不存在或时间窗倒置）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] role.grant account=" << account
                      << " role=" << role
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["id"] = id;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/roles/revoke", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "roles.revoke")) {
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
              done = ctx->store->role_revoke(int_or(*j, "id"));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "授权行不存在"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/reporting", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "reporting.set")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string account = str_or(*j, "account");
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = bool_or(*j, "clear", false)
                         ? ctx->store->reporting_clear(account)
                         : ctx->store->reporting_set(account,
                                                str_or(*j, "manager"),
                                                now_ms());
            }
            if (!done) {
              cb(err_resp(drogon::k409Conflict,
                          "汇报线设置失败（账号不存在/自为上级/成环）"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/visibility", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "visibility.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& v : ctx->store->visibility_list()) {
              Json::Value row;
              row["scope"] = v.scope;
              row["key"] = v.key;
              row["hidden"] = v.hidden;
              row["restrict_scope"] = v.restrict_scope;
              row["hide_fields"] = v.hide_fields;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["rows"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/visibility", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "visibility.set")) {
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
              done = ctx->store->set_visibility(str_or(*j, "scope"),
                                           str_or(*j, "key"),
                                           bool_or(*j, "hidden"),
                                           bool_or(*j, "restrict_scope"),
                                           str_or(*j, "hide_fields"));
            }
            if (!done) {
              cb(err_resp(drogon::k409Conflict,
                          "写失败（scope 非 member/dept 或目标不存在）"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // ============ 策略下发（org-admin） ============
  guarded("/api/admin/policy", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "policy.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& p : ctx->store->policy_list()) {
              Json::Value row;
              row["department_path"] = p.department_path;
              row["allow_anonymous"] = p.allow_anonymous;
              row["allow_cross_state"] = p.allow_cross_state;
              row["new_device_approval"] = p.new_device_approval;
              row["allow_cross_dept_file"] = p.allow_cross_dept_file;
              row["allow_forward_file"] = p.allow_forward_file;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["rows"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/policy", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "policy.set")) {
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
              done = ctx->store->set_policy(
                  str_or(*j, "department_path"),
                  bool_or(*j, "allow_anonymous", true),
                  bool_or(*j, "allow_cross_state", true),
                  bool_or(*j, "new_device_approval", false),
                  bool_or(*j, "allow_cross_dept_file", false),
                  bool_or(*j, "allow_forward_file", true));
            }
            if (!done) {
              cb(err_resp(drogon::k409Conflict,
                          "写失败（部门路径不存在）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] policy.set path="
                      << str_or(*j, "department_path")
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // ============ 留存策略（org-admin；purge 双人审批红线） ============
  guarded("/api/admin/retention", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "retention.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value rows(Json::arrayValue);
            Json::Value purges(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& p : ctx->store->retention_list()) {
              Json::Value row;
              row["department_path"] = p.department_path;
              row["retention_days"] = p.retention_days;
              row["updated_by"] = p.updated_by;
              row["updated_ms"] = p.updated_ms;
              rows.append(row);
            }
            for (const auto& g : ctx->store->retention_purges()) {
              Json::Value row;
              row["id"] = g.id;
              row["purged_by"] = g.purged_by;
              row["approved_by"] = g.approved_by;
              row["reason"] = g.reason;
              row["msg_count"] = g.msg_count;
              row["before_ms"] = g.before_ms;
              row["purged_ms"] = g.purged_ms;
              purges.append(row);
            }
            Json::Value out = ok_json();
            out["rows"] = rows;
            out["purges"] = purges;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/retention", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "retention.set")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const int days = static_cast<int>(int_or(*j, "days"));
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = ctx->store->retention_set(days, str_or(*j, "department_path"),
                                          session_account(req), now_ms());
            }
            if (!done) {
              cb(err_resp(drogon::k409Conflict,
                          "写失败（days 须 0 或 30/180/365/1095；部门路径"
                          "须存在）"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/retention/purge", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "retention.purge")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string reason = str_or(*j, "reason");
            const std::string approved_by = str_or(*j, "approved_by");
            const std::string purged_by = session_account(req);
            if (reason.empty() || approved_by.empty()) {
              cb(err_resp(drogon::k400BadRequest,
                          "理由与审批人必填（双人审批红线：approved_by≠"
                          "purged_by）"));
              return;
            }
            std::int64_t ledger_id = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              ledger_id = ctx->store->retention_purge(
                  int_or(*j, "before_ms"), reason, purged_by, approved_by,
                  static_cast<int>(int_or(*j, "policy_days")), now_ms());
            }
            if (ledger_id == 0) {
              cb(err_resp(drogon::k409Conflict,
                          "清理被拒（双人审批口径：审批人≠执行人且双方须"
                          "已建账号；理由必填）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] retention.purge by=" << purged_by
                      << " approved_by=" << approved_by
                      << " ledger=" << ledger_id << "\n";
            Json::Value out = ok_json();
            out["id"] = ledger_id;
            cb(json_resp(drogon::k200OK, out));
          });
}

} // namespace memex::admin
