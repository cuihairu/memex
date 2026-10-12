#include "admin_app.hpp"

#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#include <cred.hpp>
#include <authz.hpp>
#include <store.hpp>

namespace memex::admin {
namespace {

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// —— JSON 应答与请求体小工具 ——

drogon::HttpResponsePtr json_resp(drogon::HttpStatusCode code,
                                  const Json::Value& j) {
  auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
  resp->setStatusCode(code);
  return resp;
}

Json::Value ok_json() {
  Json::Value j;
  j["ok"] = true;
  return j;
}

drogon::HttpResponsePtr err_resp(drogon::HttpStatusCode code,
                                 const std::string& msg) {
  Json::Value j;
  j["ok"] = false;
  j["error"] = msg;
  return json_resp(code, j);
}

// 请求体解析（坏体＝null 返回，调用方 400）
std::shared_ptr<Json::Value> body_of(const drogon::HttpRequestPtr& req) {
  return req->getJsonObject();
}

std::string str_or(const Json::Value& j, const char* key,
                   const std::string& dflt = {}) {
  return j.isMember(key) && j[key].isString() ? j[key].asString() : dflt;
}

std::int64_t int_or(const Json::Value& j, const char* key,
                    std::int64_t dflt = 0) {
  if (!j.isMember(key)) return dflt;
  if (j[key].isInt64()) return j[key].asInt64(); // 时间戳级数值超 int32
  return j[key].isConvertibleTo(Json::intValue) ? j[key].asInt64() : dflt;
}

bool bool_or(const Json::Value& j, const char* key, bool dflt = false) {
  return j.isMember(key) && j[key].isBool() ? j[key].asBool() : dflt;
}

// 群号解析（与 files 面 parse_gid_param 同口径：非空纯数字且 >0）
bool parse_gid_q(const std::string& s, std::uint64_t* out) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  *out = std::strtoull(s.c_str(), nullptr, 10);
  return *out > 0;
}

// 远程协助权限位 → 逗号串（view/keyboard/mouse/clipboard/file 顺序固定，
// 与 files 面 assist_perm_mask 同词汇表）
std::string assist_mask_names(int mask) {
  static const struct {
    int bit;
    const char* name;
  } kBits[] = {{memex::server::ServerStore::kAssistView, "view"},
               {memex::server::ServerStore::kAssistKeyboard, "keyboard"},
               {memex::server::ServerStore::kAssistMouse, "mouse"},
               {memex::server::ServerStore::kAssistClipboard, "clipboard"},
               {memex::server::ServerStore::kAssistFileTransfer, "file"}};
  std::string out;
  for (const auto& b : kBits) {
    if ((mask & b.bit) == 0) continue;
    if (!out.empty()) out += ",";
    out += b.name;
  }
  return out;
}

// 品牌素材 PNG 校验（与 files 面 png_asset_check 同口径：magic 8 字节＋
// ≤512KiB＋IHDR 尺寸 ≤2048×2048；IHDR 宽/高在固定偏移 16/20 大端）。
// 回 {0,""}=过，否则 {status,error}。该校验在 files_server 是文件内 static，
// 管理面直连库、不复用服务端路由层——同口径重写一次（不动既有服务端）。
std::pair<int, std::string> png_asset_check(const std::string& b) {
  static const unsigned char kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
  if (b.size() < 8 || std::memcmp(b.data(), kPngMagic, 8) != 0) {
    return {415, "素材须为 PNG 格式"};
  }
  if (b.size() > 512 * 1024) {
    return {413, "素材超大小上限（512KiB）"};
  }
  if (b.size() >= 24) {
    const auto be32 = [&](std::size_t off) {
      return (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(b[off]))
              << 24) |
             (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(b[off + 1]))
              << 16) |
             (static_cast<std::uint32_t>(
                  static_cast<unsigned char>(b[off + 2]))
              << 8) |
             static_cast<std::uint32_t>(
                 static_cast<unsigned char>(b[off + 3]));
    };
    const std::uint32_t w = be32(16);
    const std::uint32_t h = be32(20);
    if (w == 0 || h == 0 || w > 2048 || h > 2048) {
      return {413, "素材尺寸超限（≤2048×2048）"};
    }
  }
  return {0, ""};
}

// 会话取当前账号（未登录＝空串）
std::string session_account(const drogon::HttpRequestPtr& req) {
  const auto acct = req->session()->getOptional<std::string>("account");
  return acct.value_or(std::string{}); // {} 推不出 value_or 模板参（CI GCC 拒，GCC 15 收）
}

// —— CSRF 轻闸：改写类请求须带自定义头（跨站表单无法携带，CORS 预检
//    也过不来）——部署面反代到公网时这是最低限度的防跨站写。如实口径：
//    不是完整 CSRF 方案，同站 XSS 不设防（后台工具先保内网可用）。
bool csrf_ok(const drogon::HttpRequestPtr& req) {
  return req->getHeader("X-Memex-Admin") == "1";
}

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

// 判权门：org-admin 面。拒绝留痕（与 files_server 同款 std::cout）。
std::optional<drogon::HttpResponsePtr> require_manage(
    AdminContext& ctx, const drogon::HttpRequestPtr& req,
    const std::string& action_detail) {
  const std::string account = session_account(req);
  const memex::server::Decision d = ctx.az.authorize(
      {account, "admin:manage", "admin", "admin-web:" + action_detail, ""});
  if (d.allowed) return std::nullopt;
  std::cout << "[MEMEX-ADMIN] admin.denied account=" << account
            << " action=" << action_detail << " reason=" << d.reason << "\n";
  return err_resp(drogon::k403Forbidden, "无权（" + d.reason + "）");
}

// 判权门：auditor 面（归档检索/导出/查阅日志）。拒绝同样留痕。
std::optional<drogon::HttpResponsePtr> require_audit(
    AdminContext& ctx, const drogon::HttpRequestPtr& req) {
  const std::string account = session_account(req);
  const memex::server::Decision d =
      ctx.az.authorize({account, "audit:read", "audit", "admin-web", ""});
  if (d.allowed) return std::nullopt;
  // 被拒尝试落查阅日志（action=audit.denied，与 CLI/服务端同口径对账）
  memex::server::AuditReadRow denied;
  denied.op_account = account;
  denied.action = "audit.denied";
  denied.filters = "admin-web";
  denied.ts_ms = now_ms();
  {
    std::lock_guard<std::mutex> lk(ctx.mu);
    ctx.store->add_audit_read(denied);
  }
  std::cout << "[MEMEX-ADMIN] audit.denied account=" << account
            << " reason=" << d.reason << "\n";
  return err_resp(drogon::k403Forbidden,
                  "无 auditor 有效角色（org-admin 不自动可读消息，须经"
                  "授权授予 auditor）；被拒尝试已留痕");
}

} // namespace

int setup(int argc, char** argv) {
  // —— 配置：argv > env > 默认 ——
  int port = 8089;
  std::string db_path = "/var/lib/memex/memex.db";
  std::string www_dir = "./www";
  std::string files_url;
  std::string storage_url;
  int session_ttl = 1800;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (a == "--db" && i + 1 < argc) db_path = argv[++i];
    else if (a == "--www" && i + 1 < argc) www_dir = argv[++i];
    else if (a == "--files-url" && i + 1 < argc) files_url = argv[++i];
    else if (a == "--storage-url" && i + 1 < argc) storage_url = argv[++i];
    else if (a == "--session-ttl" && i + 1 < argc) session_ttl = std::atoi(argv[++i]);
  }
  if (const char* e = std::getenv("MEMEX_ADMIN_PORT")) port = std::atoi(e);
  if (const char* e = std::getenv("MEMEX_ADMIN_DB")) db_path = e;
  if (const char* e = std::getenv("MEMEX_ADMIN_WWW")) www_dir = e;
  if (const char* e = std::getenv("MEMEX_FILES_URL")) files_url = e;
  if (const char* e = std::getenv("MEMEX_STORAGE_URL")) storage_url = e;
  if (const char* e = std::getenv("MEMEX_ADMIN_SESSION_TTL"))
    session_ttl = std::atoi(e);
  if (port <= 0 || port > 65535 || db_path.empty()) {
    std::cerr << "[MEMEX-ADMIN] 配置不合法：port=" << port
              << " db=" << db_path << "\n";
    return 2;
  }

  // —— 库：直连 ServerStore（与 CLI 同径）。ServerStore 非线程安全，
  //    后台进程单 IO 线程（setThreadNum(1)）串行化全部处理，再加一把
  //    互斥锁兜底未来多线程化——管理面流量极小，不构成吞吐问题。
  //    状态全进堆上 AdminContext（setup 返回后处理器仍长命，见上文）。
  auto ctx = std::make_shared<AdminContext>();
  ctx->store = std::make_shared<memex::server::ServerStore>();
  if (!ctx->store->open(db_path)) {
    std::cerr << "[MEMEX-ADMIN] 本地库打开失败：" << db_path << "\n";
    return 1;
  }
  ctx->started_ms = now_ms();
  ctx->files_url = files_url;
  ctx->storage_url = storage_url;

  // —— az：服务端同一判权引擎，规则按 files_server 既有语义镜像注册
  //    （角色集与冲突规则都是服务端口径，未命中 default-deny 不自造）。
  //    - admin-manage：组织级管理动作归 org-admin 有效角色（基础∪窗内
  //      授权，effective_roles 现查——吊销即时生效，不缓存进会话）；
  //    - audit-read：镜像 files_server「audit-read」逐字口径——持
  //      auditor 有效角色（平台-6：org-admin 不自动可读消息）。
  ctx->az.add_rule(memex::server::RuleEffect::ExplicitAllow, "admin-manage",
                   [store = ctx->store](const memex::server::AuthzQuery& q) {
                     if (q.action != "admin:manage") return false;
                     const auto roles =
                         store->effective_roles(q.subject, now_ms());
                     for (const auto& r : roles) {
                       if (r == "org-admin") return true;
                     }
                     return false;
                   });
  ctx->az.add_rule(memex::server::RuleEffect::ExplicitAllow, "audit-read",
                   [store = ctx->store](const memex::server::AuthzQuery& q) {
                     if (q.action != "audit:read") return false;
                     const auto roles =
                         store->effective_roles(q.subject, now_ms());
                     for (const auto& r : roles) {
                       if (r == "auditor") return true;
                     }
                     return false;
                   });

  auto& app = drogon::app();

  // —— 统一注册小工具：登录态＋CSRF 闸后进业务体 ——
  using Handler = std::function<void(const drogon::HttpRequestPtr&,
                                     std::function<void(
                                         const drogon::HttpResponsePtr&)>&&)>;
  const auto guarded = [&app](const char* path,
                           const std::vector<
                               drogon::internal::HttpConstraint>& methods,
                           bool csrf,
                           const std::function<
                               void(const drogon::HttpRequestPtr&,
                                    std::function<void(
                                        const drogon::HttpResponsePtr&)>&&)>&
                               body) {
    app.registerHandler(
        path,
        [csrf, body](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
          if (csrf && !csrf_ok(req)) {
            cb(err_resp(drogon::k403Forbidden, "缺少 X-Memex-Admin 头"));
            return;
          }
          if (session_account(req).empty()) {
            cb(err_resp(drogon::k401Unauthorized, "未登录或会话已过期"));
            return;
          }
          body(req, std::move(cb));
        },
        methods);
  };

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

  // ============ 归档检索与导出（auditor 证，平台-6 红线） ============
  // 条件解析共用：全部可空——空=不过滤（与 CLI messages 同径）。
  const auto parse_search = [](const Json::Value& j) {
    memex::server::MessageSearch q;
    q.account = str_or(j, "account");
    q.keyword = str_or(j, "keyword");
    q.since_ms = int_or(j, "since_ms");
    q.until_ms = int_or(j, "until_ms");
    q.limit = static_cast<int>(int_or(j, "limit", 200));
    if (q.limit <= 0 || q.limit > 5000) q.limit = 200;
    q.peer = str_or(j, "peer");
    return q;
  };
  const auto filters_summary = [](const Json::Value& j, int count) {
    // 摘要不含消息内容（与 CLI 同口径）：条件与命中数
    return "account=" + str_or(j, "account") + " keyword=" +
           (str_or(j, "keyword").empty() ? "-" : "*") +
           " since=" + std::to_string(int_or(j, "since_ms")) +
           " until=" + std::to_string(int_or(j, "until_ms")) +
           " peer=" + str_or(j, "peer") + " count=" + std::to_string(count);
  };

  guarded("/api/admin/messages/search", {drogon::Post}, true,
          [ctx, parse_search, filters_summary](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_audit(*ctx, req)) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const auto q = parse_search(*j);
            std::vector<memex::server::ArchivedMessage> rows;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              rows = ctx->store->search_messages(q);
              memex::server::AuditReadRow rec;
              rec.op_account = session_account(req);
              rec.action = "audit.message.search";
              rec.filters = filters_summary(*j,
                                            static_cast<int>(rows.size()));
              rec.result_count = static_cast<int>(rows.size());
              rec.ts_ms = now_ms();
              ctx->store->add_audit_read(rec);
            }
            Json::Value arr(Json::arrayValue);
            for (const auto& m : rows) {
              Json::Value row;
              row["msg_id"] = m.msg_id;
              row["from"] = m.from_account;
              row["to"] = m.to_account;
              row["type"] = m.type;
              row["text"] = m.text;
              row["ts_ms"] = m.ts_ms;
              row["recalled"] = m.recalled;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["messages"] = arr;
            out["count"] = static_cast<Json::Int64>(rows.size());
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/messages/export", {drogon::Post}, true,
          [ctx, parse_search, filters_summary](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_audit(*ctx, req)) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const auto q = parse_search(*j);
            std::vector<memex::server::ArchivedMessage> rows;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              rows = ctx->store->search_messages(q);
              memex::server::AuditReadRow rec;
              rec.op_account = session_account(req);
              rec.action = "audit.message.export";
              rec.filters = filters_summary(*j,
                                            static_cast<int>(rows.size()));
              rec.result_count = static_cast<int>(rows.size());
              rec.ts_ms = now_ms();
              ctx->store->add_audit_read(rec);
            }
            // 导出＝JSONL 附件（浏览器下载留证）；逐条一行完整记录
            std::string body_text;
            for (const auto& m : rows) {
              Json::Value row;
              row["msg_id"] = m.msg_id;
              row["from"] = m.from_account;
              row["to"] = m.to_account;
              row["type"] = m.type;
              row["text"] = m.text;
              row["ts_ms"] = m.ts_ms;
              row["recalled"] = m.recalled;
              body_text += Json::writeString(Json::StreamWriterBuilder(), row);
              body_text += "\n";
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
            resp->addHeader("Content-Disposition",
                            "attachment; filename=memex-export.jsonl");
            resp->setBody(std::move(body_text));
            cb(resp);
          });

  guarded("/api/admin/audit-reads", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_audit(*ctx, req)) {
              cb(std::move(*deny));
              return;
            }
            const std::string limit_s = req->getParameter("limit");
            const int limit = std::clamp(
                limit_s.empty() ? 100 : atoi(limit_s.c_str()), 1, 1000);
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& r : ctx->store->audit_reads(limit)) {
              Json::Value row;
              row["id"] = r.id;
              row["op_account"] = r.op_account;
              row["action"] = r.action;
              row["filters"] = r.filters;
              row["result_count"] = r.result_count;
              row["ts_ms"] = r.ts_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["reads"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  // ============ 设备与登录台账（org-admin） ============
  guarded("/api/admin/devices", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "devices.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& d : ctx->store->device_list()) {
              Json::Value row;
              row["fingerprint"] = d.fingerprint;
              row["kind"] = d.kind;
              row["name"] = d.name;
              row["owner_account"] = d.owner_account;
              row["enabled"] = d.enabled;
              row["first_seen_ms"] = d.first_seen_ms;
              row["last_seen_ms"] = d.last_seen_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["devices"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/devices/enabled", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "devices.enabled")) {
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
              done = ctx->store->set_device_enabled(str_or(*j, "fingerprint"),
                                               bool_or(*j, "enabled", true));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "设备不存在"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/devices/owner", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "devices.owner")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string fp = str_or(*j, "fingerprint");
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              done = bool_or(*j, "clear", false)
                         ? ctx->store->unbind_device(fp)
                         : ctx->store->set_device_owner(fp,
                                                   str_or(*j, "owner"));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound,
                          "设备不存在或责任人设置被拒"));
              return;
            }
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/logins", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "logins.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string account = req->getParameter("account");
            const std::string fp = req->getParameter("device");
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& r : ctx->store->login_records(account, fp)) {
              Json::Value row;
              row["id"] = r.id;
              row["account"] = r.account;
              row["result"] = r.result;
              row["kind"] = r.kind;
              row["name"] = r.name;
              row["fingerprint"] = r.fingerprint.substr(0, 8);
              row["source_ip"] = r.source_ip;
              row["version"] = r.version;
              row["ts_ms"] = r.ts_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["logins"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

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

  // ============ 文件与配额（org-admin；波次 2 第 2 条） ============
  // 文件台账/删除/置顶＋两级配额（群+个人）＋存储状态只读探活（CLI
  // files/storage 同径直连库）。边界如实：admin 进程不持 S3 凭据——删除
  // 只落台账与配额退费（与 files 面 handle_delete 同账），引用归零的对象
  // 字节留待过期清理面对账（files 面既有 v1 注记同口径）；存储面只探活，
  // up/down/compose 编排在宿主 compose 栈由 CLI 代管，后台不代启停。

  guarded("/api/admin/files", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "files.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string limit_s = req->getParameter("limit");
            const std::string offset_s = req->getParameter("offset");
            const int limit = std::clamp(
                limit_s.empty() ? 200 : atoi(limit_s.c_str()), 1, 500);
            const int offset = std::clamp(
                offset_s.empty() ? 0 : atoi(offset_s.c_str()), 0, 1000000);
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            // 空 gid/uid＝不过滤（全量台账，与 store list_files 口径一致）
            for (const auto& f :
                 ctx->store->list_files("", "", limit, offset, -1)) {
              Json::Value row;
              row["id"] = f.id;
              row["owner"] = f.owner;
              row["belong_gid"] = f.belong_gid;
              row["belong_uid"] = f.belong_uid;
              row["file_name"] = f.file_name;
              row["file_size"] = f.file_size;
              row["file_hash"] = f.file_hash;
              row["source"] = static_cast<int>(f.source);
              row["upload_ts"] = f.upload_ts;
              row["status"] = static_cast<int>(f.status);
              row["pin"] = f.pin;
              row["kind"] = static_cast<int>(f.kind);
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["files"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/files/pin", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "files.pin")) {
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
              done = ctx->store->set_file_pin(int_or(*j, "id"),
                                              bool_or(*j, "pin", true));
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "文件不存在"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] files.pin id=" << int_or(*j, "id")
                      << " pin=" << (bool_or(*j, "pin", true) ? "1" : "0")
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["pin"] = bool_or(*j, "pin", true);
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/files/delete", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "files.delete")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t id = int_or(*j, "id");
            std::string name;
            std::int64_t size = 0;
            std::string owner;
            std::string gid;
            bool done = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              // 台账与配额退费与 files 面 handle_delete 同账：群文件退群
              // 配额，属主配额恒退（字节面本进程无 S3 凭据不做——引用
              // 归零对象留待过期清理对账，见本节头注）。
              if (const auto meta = ctx->store->file_by_id(id)) {
                name = meta->file_name;
                size = meta->file_size;
                owner = meta->owner;
                gid = meta->belong_gid;
                done = ctx->store->delete_file_meta(id);
                if (done) {
                  if (!gid.empty())
                    ctx->store->add_group_quota_used(
                        gid, -size);
                  ctx->store->add_user_quota_used(owner, -size);
                }
              }
            }
            if (!done) {
              cb(err_resp(drogon::k404NotFound, "文件不存在"));
              return;
            }
            // 删除留痕（stdout；台账效果＝行删除＋双面配额退费可对账）
            std::cout << "[MEMEX-ADMIN] files.delete id=" << id
                      << " name=" << name << " size=" << size
                      << " owner=" << owner
                      << (gid.empty() ? "" : " group=" + gid)
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // 两级配额一览：群级（groups_list 逐群）＋个人级（account_list 逐号）。
  // 无配额行＝used/limit 双 0（显示「不限」），store read_quota 既有口径。
  guarded("/api/admin/quotas", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "quotas.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value groups(Json::arrayValue);
            Json::Value users(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& g : ctx->store->groups_list()) {
              const auto q = ctx->store->get_group_quota(
                  std::to_string(g.group_id));
              Json::Value row;
              row["scope"] = "group";
              row["target"] = std::to_string(g.group_id);
              row["name"] = g.name;
              row["used_bytes"] = q ? q->used_bytes : 0;
              row["limit_bytes"] = q ? q->limit_bytes : 0;
              groups.append(row);
            }
            for (const auto& [acct, label] : ctx->store->account_list()) {
              const auto q = ctx->store->get_user_quota(acct);
              Json::Value row;
              row["scope"] = "user";
              row["target"] = acct;
              row["name"] = label;
              row["used_bytes"] = q ? q->used_bytes : 0;
              row["limit_bytes"] = q ? q->limit_bytes : 0;
              users.append(row);
            }
            Json::Value out = ok_json();
            out["groups"] = groups;
            out["users"] = users;
            cb(json_resp(drogon::k200OK, out));
          });

  // 配额写入（仅 org-admin；CLI set quota 同径 store upsert）。
  guarded("/api/admin/quotas", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "quotas.set")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string scope = str_or(*j, "scope");
            const std::string target = str_or(*j, "target");
            const std::int64_t limit = int_or(*j, "limit_bytes");
            if (scope != "group" && scope != "user") {
              cb(err_resp(drogon::k400BadRequest,
                          "scope 只接受 group|user（两级配额）"));
              return;
            }
            if (target.empty()) {
              cb(err_resp(drogon::k400BadRequest,
                          "target 必填（群号 或 账号）"));
              return;
            }
            if (limit < 0) {
              cb(err_resp(drogon::k400BadRequest, "limit_bytes 不可为负"));
              return;
            }
            bool done = false;
            bool target_ok = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              if (scope == "group") {
                const auto gid = static_cast<std::uint64_t>(
                    std::strtoull(target.c_str(), nullptr, 10));
                if (ctx->store->group_info(gid)) {
                  target_ok = true;
                  done = ctx->store->set_group_quota_limit(target, limit);
                }
              } else {
                if (ctx->store->find_account(target).has_value()) {
                  target_ok = true;
                  done = ctx->store->set_user_quota_limit(target, limit);
                }
              }
            }
            if (!target_ok) {
              cb(err_resp(drogon::k404NotFound,
                          scope == "group" ? "群不存在" : "账号不存在"));
              return;
            }
            if (!done) {
              cb(err_resp(drogon::k409Conflict, "配额写入失败"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] quota.set scope=" << scope
                      << " target=" << target << " limit=" << limit
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // 存储状态只读探活：文件面（/files/health）＋对象存储（/minio/
  // health/live；任一 HTTP 应答都算 up＝端点活着，连接失败/超时才 down
  // ——与 CLI storage health 严格 /minio/health/live 判定的口径差异如实
  // 注记）。up/down/compose 编排在宿主栈，后台只读不代启停。
  guarded("/api/admin/storage/status", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "storage.status")) {
              cb(std::move(*deny));
              return;
            }
            const auto note_json = [] {
              Json::Value out = ok_json();
              out["note"] =
                  "只读探活：up/down/compose 编排在宿主 docker compose 栈"
                  "（CLI storage up/down 代管），后台不代启停";
              return out;
            };
            const std::string files_url = ctx->files_url;
            const std::string storage_url = ctx->storage_url;
            if (files_url.empty()) {
              // 文件面未配置：对象存储探活照走（两项独立；回调捕获
              // client 保活——悬垂＝在途回包解引用已析构对象）
              if (storage_url.empty()) {
                Json::Value out = note_json();
                out["files"] = "未配置（MEMEX_FILES_URL）";
                out["storage"] = "未配置（MEMEX_STORAGE_URL）";
                cb(json_resp(drogon::k200OK, out));
                return;
              }
              auto sclient = drogon::HttpClient::newHttpClient(
                  storage_url, drogon::app().getLoop());
              auto sreq = drogon::HttpRequest::newHttpRequest();
              sreq->setMethod(drogon::Get);
              sreq->setPath("/minio/health/live");
              sclient->sendRequest(
                  sreq,
                  [sclient, cb, note_json](
                      drogon::ReqResult sr,
                      const drogon::HttpResponsePtr& sresp) {
                    Json::Value out = note_json();
                    out["files"] = "未配置（MEMEX_FILES_URL）";
                    // 任一 HTTP 应答＝端点活着；连接失败/超时＝down
                    out["storage"] =
                        (sr == drogon::ReqResult::Ok && sresp != nullptr)
                            ? "up"
                            : "down";
                    cb(json_resp(drogon::k200OK, out));
                  },
                  3.0);
              return;
            }
            // 先探文件面，回调里再探对象存储（串行两腿；回调捕获 client
            // 保活——悬垂＝在途回包解引用已析构对象）
            auto client = drogon::HttpClient::newHttpClient(
                files_url, drogon::app().getLoop());
            auto freq = drogon::HttpRequest::newHttpRequest();
            freq->setMethod(drogon::Get);
            freq->setPath("/files/health");
            client->sendRequest(
                freq,
                [client, cb, note_json, storage_url](
                    drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
                  const bool files_up =
                      r == drogon::ReqResult::Ok && resp &&
                      resp->getStatusCode() == drogon::k200OK;
                  if (storage_url.empty()) {
                    Json::Value out = note_json();
                    out["files"] = files_up ? "up" : "down";
                    out["storage"] = "未配置（MEMEX_STORAGE_URL）";
                    cb(json_resp(drogon::k200OK, out));
                    return;
                  }
                  auto sclient = drogon::HttpClient::newHttpClient(
                      storage_url, drogon::app().getLoop());
                  auto sreq = drogon::HttpRequest::newHttpRequest();
                  sreq->setMethod(drogon::Get);
                  sreq->setPath("/minio/health/live");
                  sclient->sendRequest(
                      sreq,
                      [sclient, cb, note_json, files_up](
                          drogon::ReqResult sr,
                          const drogon::HttpResponsePtr& sresp) {
                        Json::Value out = note_json();
                        out["files"] = files_up ? "up" : "down";
                        out["storage"] =
                            (sr == drogon::ReqResult::Ok && sresp != nullptr)
                                ? "up"
                                : "down";
                        cb(json_resp(drogon::k200OK, out));
                      },
                      3.0);
                },
                3.0);
          });

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

  // ============ 业务流程面（org-admin；波次 2 第 4 条） ============
  // 审批（台账＋决定）、日报团队聚合、办公室位置图（工位建改／绑定）、
  // 远程协助（部门放行策略＋会话台账＋动作审计）、个人任务与备忘录台账。
  // 口径如实（与群组管理面同口径）：org-admin=组织级管理面，直连库即 CLI
  // 同权，不过成员面的关系闸——成员面的 report:read 只对直属上级（org-admin
  // 不兜底）、approval:decide 只对直属上级或无上级时 org-admin 兜底，本面按
  // 组织级视图呈现全量台账；审批按申请人逐查，无「已决全量」接口（store 只
  // 有 pending 全量＋逐申请人）。成员红线动作一律只读：撤回只属申请人、
  // 批拒只属受控方（consent 红线）、完成与提醒回执只属清单主人、个人日报由
  // 员工自助提交，后台不代写不代发；assist 会话生命周期（request/respond/
  // start/end）是当事方短票流程，后台只有台账与策略。
  // 个人备忘录为员工私人文本——本面按组织级合规审阅呈现（直连库同权），
  // 与平台-6 消息内容红线不同口径，如实注记在页面说明与交付记录。

  guarded("/api/admin/approvals", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "approvals.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string applicant = req->getParameter("applicant");
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            const bool per_person = !applicant.empty();
            if (per_person && !ctx->store->find_account(applicant)) {
              cb(err_resp(drogon::k404NotFound, "账号不存在"));
              return;
            }
            // 无申请人=全部待决（跨全组织盘点）；指定申请人=该人全部
            // （含已决——逐申请人视角，成员面 approvals_of 同径）
            for (const auto& a :
                 per_person ? ctx->store->approvals_of(applicant)
                            : ctx->store->approvals_pending()) {
              Json::Value row;
              row["id"] = a.id;
              row["applicant"] = a.applicant;
              row["type"] = a.type;
              row["from"] = a.leave_from;
              row["to"] = a.leave_to;
              row["reason"] = a.reason;
              row["status"] = a.status;
              row["decider"] = a.decider;
              row["decision_note"] = a.decision_note;
              row["created_ms"] = a.created_ms;
              row["decided_ms"] = a.decided_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["scope"] = per_person ? "applicant" : "pending";
            out["applicant"] = applicant;
            out["approvals"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/approvals/decide", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "approvals.decide")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            if (!j->isMember("id") || !j->isMember("approved") ||
                !(*j)["approved"].isBool() ||
                (j->isMember("note") && !(*j)["note"].isString())) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：id/approved"));
              return;
            }
            const std::int64_t id = int_or(*j, "id");
            if (id <= 0) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：id/approved"));
              return;
            }
            const bool approved = (*j)["approved"].asBool();
            const std::string note = str_or(*j, "note");
            const std::string actor = session_account(req);
            std::lock_guard<std::mutex> lk(ctx->mu);
            const auto row = ctx->store->approval_by_id(id);
            if (!row.has_value()) {
              cb(err_resp(drogon::k404NotFound, "申请不存在"));
              return;
            }
            // 自审不成立（成员面 deny 层口径延续：申请人本人无决定权）
            if (row->applicant == actor) {
              cb(err_resp(drogon::k400BadRequest,
                          "自审不成立（申请人本人不可决定）"));
              return;
            }
            if (row->status != "pending") {
              cb(err_resp(drogon::k409Conflict,
                          "该申请已决（" + row->status + "），不可重复决定"));
              return;
            }
            // 决定不删改：UPDATE ... WHERE status='pending' 守卫（store 层）
            if (!ctx->store->approval_decide(id, actor, approved, note,
                                             now_ms())) {
              cb(err_resp(drogon::k409Conflict, "决定落库失败（状态已变）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] approvals.decide id=" << id
                      << " applicant=" << row->applicant
                      << " result=" << (approved ? "approved" : "rejected")
                      << " by=" << actor << "\n";
            Json::Value out = ok_json();
            out["id"] = id;
            out["status"] = approved ? "approved" : "rejected";
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/reports/team", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "reports.team")) {
              cb(std::move(*deny));
              return;
            }
            // 缺省=会话账号自己的直属下属聚合（成员面 reports/team 同径）；
            // ?manager= 指定任意员工的直接下属聚合（组织级视图）
            const std::string manager = req->getParameter("manager");
            const std::string scope = manager.empty() ? session_account(req)
                                                      : manager;
            Json::Value team(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->find_account(scope)) {
              cb(err_resp(drogon::k404NotFound, "账号不存在"));
              return;
            }
            for (const auto& sub : ctx->store->direct_reports(scope)) {
              Json::Value rows(Json::arrayValue);
              for (const auto& r : ctx->store->reports_of(sub)) {
                Json::Value row;
                row["id"] = r.id;
                row["author"] = r.author;
                row["report_date"] = r.report_date;
                row["content"] = r.content;
                row["created_ms"] = r.created_ms;
                row["updated_ms"] = r.updated_ms;
                rows.append(row);
              }
              Json::Value grp;
              grp["author"] = sub;
              grp["reports"] = rows;
              team.append(grp);
            }
            Json::Value out = ok_json();
            out["manager"] = scope;
            out["team"] = team;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/reports", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "reports.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string author = req->getParameter("author");
            if (author.empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少参数：author"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->find_account(author)) {
              cb(err_resp(drogon::k404NotFound, "账号不存在"));
              return;
            }
            for (const auto& r : ctx->store->reports_of(author)) {
              Json::Value row;
              row["id"] = r.id;
              row["author"] = r.author;
              row["report_date"] = r.report_date;
              row["content"] = r.content;
              row["created_ms"] = r.created_ms;
              row["updated_ms"] = r.updated_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["author"] = author;
            out["reports"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/office-map", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "office-map.list")) {
              cb(std::move(*deny));
              return;
            }
            // 跨层盘点：楼层须显式指定（成员面缺省回自楼层，本面不代裁）
            const std::string floor = req->getParameter("floor");
            if (floor.empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少参数：floor"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& s : ctx->store->seats_on_floor(floor)) {
              Json::Value row;
              row["id"] = s.id;
              row["floor"] = s.floor;
              row["label"] = s.label;
              row["x"] = s.x;
              row["y"] = s.y;
              row["account"] = s.account;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["floor"] = floor;
            out["seats"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/office-map/seat", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "office-map.seat")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::string floor = str_or(*j, "floor");
            const std::string label = str_or(*j, "label");
            if (floor.empty() || label.empty() || !j->isMember("x") ||
                !j->isMember("y") || !(*j)["x"].isNumeric() ||
                !(*j)["y"].isNumeric()) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：floor/label/x/y"));
              return;
            }
            // 归一化坐标夹 0~1（越界夹回不造假数据，与成员面同口径）
            const double x = std::max(0.0, std::min(1.0, (*j)["x"].asDouble()));
            const double y = std::max(0.0, std::min(1.0, (*j)["y"].asDouble()));
            const std::int64_t id = ctx->store->seat_upsert(floor, label, x,
                                                             y, now_ms());
            if (id <= 0) {
              cb(err_resp(drogon::k500InternalServerError, "工位落库失败"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] office-map.seat floor=" << floor
                      << " label=" << label << " id=" << id
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["id"] = id;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/office-map/remove", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "office-map.remove")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t id = int_or(*j, "id");
            if (id <= 0) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：id"));
              return;
            }
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->seat_delete(id)) {
              cb(err_resp(drogon::k404NotFound, "工位不存在"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] office-map.remove id=" << id
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/office-map/bind", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "office-map.bind")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            const std::int64_t id = int_or(*j, "id");
            if (id <= 0) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：id"));
              return;
            }
            const std::string target = str_or(*j, "account");
            std::lock_guard<std::mutex> lk(ctx->mu);
            // 换座纪律（成员面同口径）：一人一工位，目标已占他座=拒，
            // 须先解绑再绑（不静默顶替他人座位）
            if (!ctx->store->seat_bind(id, target, now_ms())) {
              cb(err_resp(drogon::k409Conflict,
                          "绑定失败（工位不存在、账号不存在或该账号已占用"
                          "其他工位——换座须先解绑）"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] office-map."
                      << (target.empty() ? "unbind" : "bind")
                      << " id=" << id
                      << (target.empty() ? "" : " account=" + target)
                      << " by=" << session_account(req) << "\n";
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/assist/policy", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "assist.policy.list")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            for (const auto& p : ctx->store->assist_policy_list()) {
              Json::Value row;
              row["scope"] = p.department_path.empty() ? "（全局）"
                                                       : p.department_path;
              row["department_path"] = p.department_path;
              row["allow"] = p.allow;
              row["updated_by"] = p.updated_by;
              row["updated_ms"] = p.updated_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["policy"] = arr;
            // 白名单口径：未配置部门沿链上溯至全局行，再默认禁止
            out["default"] = "禁止（未配置默认禁止，从严）";
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/assist/policy", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "assist.policy.set")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            // allow 须显式给出布尔（开关语义：不猜默认）
            if (!j->isMember("allow") || !(*j)["allow"].isBool()) {
              cb(err_resp(drogon::k400BadRequest,
                          "缺少字段：allow（须为 true/false）"));
              return;
            }
            const bool allow = (*j)["allow"].asBool();
            const std::string dept = str_or(*j, "department_path");
            std::lock_guard<std::mutex> lk(ctx->mu);
            // 部门行须挂已存在部门（与 CLI assist policy set 同口径，
            // 不顺手建部门）
            if (!dept.empty()) {
              bool found = false;
              for (const auto& [id, path] : ctx->store->department_list()) {
                (void)id;
                if (path == dept) {
                  found = true;
                  break;
                }
              }
              if (!found) {
                cb(err_resp(drogon::k404NotFound, "部门不存在"));
                return;
              }
            }
            // 策略面动作留痕（assist_audits 中 session_id 为空行）
            if (!ctx->store->assist_policy_set(allow, dept,
                                               session_account(req), now_ms())) {
              cb(err_resp(drogon::k500InternalServerError, "写入失败"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] assist.policy scope="
                      << (dept.empty() ? "（全局）" : dept)
                      << " allow=" << (allow ? "1" : "0")
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["department_path"] = dept;
            out["allow"] = allow;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/assist/sessions", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "assist.sessions")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            // account 空=全部台账（store 层上限 200，发起时刻倒序）
            for (const auto& s : ctx->store->assist_sessions("")) {
              Json::Value row;
              row["id"] = s.id;
              row["requester"] = s.requester;
              row["target"] = s.target;
              row["requested_mask"] = s.requested_mask;
              row["requested_perms"] = assist_mask_names(s.requested_mask);
              row["granted_mask"] = s.granted_mask;
              row["granted_perms"] = assist_mask_names(s.granted_mask);
              row["status"] = s.status;
              row["requested_ms"] = s.requested_ms;
              row["approved_ms"] = s.approved_ms;
              row["started_ms"] = s.started_ms;
              row["ended_ms"] = s.ended_ms;
              row["end_actor"] = s.end_actor;
              row["end_reason"] = s.end_reason;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["sessions"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/assist/audit", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "assist.audit")) {
              cb(std::move(*deny));
              return;
            }
            const std::string sid = req->getParameter("id");
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            // id 空=含策略面动作的近期记录（store 层上限 500，发生序）
            for (const auto& a : ctx->store->assist_audits(sid)) {
              Json::Value row;
              row["id"] = a.id;
              row["session_id"] = a.session_id;
              row["actor"] = a.actor;
              row["action"] = a.action;
              row["detail"] = a.detail;
              row["ts_ms"] = a.ts_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["id"] = sid;
            out["audits"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/tasks", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "tasks.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string account = req->getParameter("account");
            if (account.empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少参数：account"));
              return;
            }
            Json::Value mine(Json::arrayValue);
            Json::Value assigned(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->find_account(account)) {
              cb(err_resp(drogon::k404NotFound, "账号不存在"));
              return;
            }
            for (const auto& t : ctx->store->tasks_of(account)) {
              Json::Value row;
              row["id"] = t.id;
              row["creator"] = t.creator;
              row["title"] = t.title;
              row["note"] = t.note;
              row["due_ms"] = t.due_ms;
              row["reminded_ms"] = t.reminded_ms;
              row["done"] = t.done;
              row["done_ms"] = t.done_ms;
              row["created_ms"] = t.created_ms;
              row["provider"] = t.provider;
              row["ext_key"] = t.ext_key;
              mine.append(row);
            }
            for (const auto& t : ctx->store->tasks_assigned_by(account)) {
              Json::Value row;
              row["id"] = t.id;
              row["owner"] = t.owner;
              row["title"] = t.title;
              row["note"] = t.note;
              row["due_ms"] = t.due_ms;
              row["done"] = t.done;
              row["done_ms"] = t.done_ms;
              row["created_ms"] = t.created_ms;
              row["provider"] = t.provider;
              row["ext_key"] = t.ext_key;
              assigned.append(row);
            }
            Json::Value out = ok_json();
            out["account"] = account;
            out["tasks"] = mine;
            out["assigned_by_me"] = assigned;
            cb(json_resp(drogon::k200OK, out));
          });

  // ============ 任务源拉取（R27-5：数据外发默认关硬口径——本面即默认关
  // 闸的显式开启位；拉取另需主密钥在配，两道闸才开） ============
  guarded("/api/admin/task-pull", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "task_pull.view")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value out = ok_json();
            std::lock_guard<std::mutex> lk(ctx->mu);
            const auto flag = ctx->store->server_flag_get("task_pull_enabled");
            const bool enabled = flag.has_value() && *flag == "1";
            out["enabled"] = enabled;
            Json::Value arr(Json::arrayValue);
            for (const auto& c : ctx->store->task_pull_configs()) {
              const auto sealed =
                  ctx->store->task_source_cred_sealed(c.owner, c.provider);
              Json::Value row;
              row["owner"] = c.owner;
              row["provider"] = c.provider;
              row["repo"] = c.repo;
              row["enabled"] = c.enabled;
              row["interval_s"] = c.interval_s;
              row["last_pull_ms"] = c.last_pull_ms;
              row["cred_configured"] = sealed.has_value() && !sealed->empty();
              arr.append(row);
            }
            out["sources"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/task-pull", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "task_pull.config")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j || !j->isMember("enabled") || !(*j)["enabled"].isBool()) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：enabled（布尔）"));
              return;
            }
            const bool on = (*j)["enabled"].asBool();
            const std::string account = session_account(req);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->server_flag_set("task_pull_enabled", on ? "1" : "0",
                                             now_ms())) {
              cb(err_resp(drogon::k500InternalServerError, "开关落库失败"));
              return;
            }
            // 开启即明示拉取范围（谁/何时/范围＝审计面留痕；拉取器逐拍
            // 只读本键与配置行，别处不动）
            Json::Value scope(Json::arrayValue);
            for (const auto& c : ctx->store->task_pull_configs()) {
              if (!c.enabled) continue;
              scope.append(c.owner + "/" + c.provider + "/" + c.repo);
            }
            std::cout << "[MEMEX-ADMIN] task-pull enabled=" << (on ? "on" : "off")
                      << " by=" << account << " scope=["
                      << (on ? scope.toStyledString() : "—") << "]" << std::endl;
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  // —— R27-6 任务类型清单（服务端配置；初始四类 任务/需求/缺陷/其他，
  //     默认「任务」；admin 可增删改——桌面/手机从 /files/task-types 拉取） ——
  guarded("/api/admin/task-types", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "task_types.view")) {
              cb(std::move(*deny));
              return;
            }
            Json::Value out = ok_json();
            std::lock_guard<std::mutex> lk(ctx->mu);
            Json::Value arr(Json::arrayValue);
            for (const auto& t : ctx->store->task_types()) {
              Json::Value row;
              row["name"] = t.name;
              row["is_default"] = t.is_default;
              arr.append(row);
            }
            out["types"] = arr;
            out["default"] = ctx->store->task_type_default();
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/task-types", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "task_types.manage")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j || !j->isMember("name") || !(*j)["name"].isString() ||
                (*j)["name"].asString().empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：name（非空）"));
              return;
            }
            const std::string name = (*j)["name"].asString();
            const std::string account = session_account(req);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->task_type_add(name)) {
              cb(err_resp(drogon::k400BadRequest, "类型已存在或非法"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] task-type add name=" << name
                      << " by=" << account << std::endl;
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/task-types/delete", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "task_types.manage")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j || !j->isMember("name") || !(*j)["name"].isString() ||
                (*j)["name"].asString().empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少字段：name（非空）"));
              return;
            }
            const std::string name = (*j)["name"].asString();
            const std::string account = session_account(req);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->task_type_delete(name)) {
              // 默认类型/有任务引用/不存在——一律拒（存量任务不悬空）
              cb(err_resp(drogon::k400BadRequest,
                          "拒删：默认类型、有任务引用或不存在"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] task-type delete name=" << name
                      << " by=" << account << std::endl;
            cb(json_resp(drogon::k200OK, ok_json()));
          });

  guarded("/api/admin/memos", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "memos.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string account = req->getParameter("account");
            if (account.empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少参数：account"));
              return;
            }
            const std::string limit_s = req->getParameter("limit");
            const std::string offset_s = req->getParameter("offset");
            const int limit = std::clamp(
                limit_s.empty() ? 200 : atoi(limit_s.c_str()), 1, 500);
            const int offset = std::clamp(
                offset_s.empty() ? 0 : atoi(offset_s.c_str()), 0, 1000000);
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->find_account(account)) {
              cb(err_resp(drogon::k404NotFound, "账号不存在"));
              return;
            }
            for (const auto& m : ctx->store->list_memos(account, limit,
                                                         offset)) {
              Json::Value row;
              row["id"] = m.id;
              row["owner"] = m.owner;
              row["content"] = m.content;
              row["created_ms"] = m.created_ms;
              row["updated_ms"] = m.updated_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["account"] = account;
            out["limit"] = limit;
            out["offset"] = offset;
            out["memos"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  // ============ 品牌与外观（org-admin；波次 2 第 5 条） ============
  // 品牌（branding_*）与品牌素材（logo/splash PNG 上传/清除）：组织级配置，
  // 写权归 org-admin（files 面 branding_can_manage 同口径）。
  // 口径如实（与前几面同口径：org-admin 不过成员面的能力闸）：
  // - 表情包（emoji_*）：个人素材，仅本人读写（emoji_bytes/emoji_delete 均
  //   属主裁决）。本面只按账号出元数据清点（名称/大小/时间，组织级合规
  //   清点，与个人备忘录同先例）；不代取字节、不代删——字节与删除仍是
  //   属主能力闸，后台不过。
  // - 头像（avatar_*）：上传/删除仅本人（成员面能力闸，后台不过）；读面
  //   在 files 面本就组织内互见（?account= 任意成员），故本面代读不新增
  //   任何可见性——与表情包字节的属主封锁不同口径，如实注记。

  // 品牌设置读面：文本三件＋素材有无/大小/版本
  guarded("/api/admin/branding", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "branding.get")) {
              cb(std::move(*deny));
              return;
            }
            const auto b = ctx->store->branding_get();
            Json::Value out = ok_json();
            out["company_name"] = b.company_name;
            out["accent"] = b.accent;
            out["slogan"] = b.slogan;
            out["has_logo"] = !b.logo.empty();
            out["has_splash"] = !b.splash.empty();
            out["logo_size"] = static_cast<Json::Int64>(b.logo.size());
            out["splash_size"] = static_cast<Json::Int64>(b.splash.size());
            out["version"] = b.version;
            out["updated_ms"] = b.updated_ms;
            cb(json_resp(drogon::k200OK, out));
          });

  // 品牌写面：文本三件（缺省不动/空串清空）＋素材清除布尔；与 files 面
  // route_branding_set 同校验（accent #rrggbb 形态门；无可写字段 400）
  guarded("/api/admin/branding", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "branding.set")) {
              cb(std::move(*deny));
              return;
            }
            const auto j = body_of(req);
            if (!j) {
              cb(err_resp(drogon::k400BadRequest, "请求体不是合法 JSON"));
              return;
            }
            if ((j->isMember("company_name") &&
                 !(*j)["company_name"].isString()) ||
                (j->isMember("accent") && !(*j)["accent"].isString()) ||
                (j->isMember("slogan") && !(*j)["slogan"].isString())) {
              cb(err_resp(drogon::k400BadRequest,
                          "company_name/accent/slogan 须为字符串"));
              return;
            }
            if ((j->isMember("clear_logo") && !(*j)["clear_logo"].isBool()) ||
                (j->isMember("clear_splash") && !(*j)["clear_splash"].isBool())) {
              cb(err_resp(drogon::k400BadRequest,
                          "clear_logo/clear_splash 须为布尔"));
              return;
            }
            const bool clear_logo = bool_or(*j, "clear_logo");
            const bool clear_splash = bool_or(*j, "clear_splash");
            // 字段缺省=nullopt（不动）；显式空串=清空
            const auto pick = [&](const char* key) -> std::optional<std::string> {
              return j->isMember(key)
                         ? std::optional<std::string>((*j)[key].asString())
                         : std::nullopt;
            };
            const std::optional<std::string> name = pick("company_name");
            const std::optional<std::string> accent = pick("accent");
            const std::optional<std::string> slogan = pick("slogan");
            // accent 形态门：#rrggbb（显式空串=清空放行）
            if (accent.has_value() && !accent->empty()) {
              bool ok = accent->size() == 7 && (*accent)[0] == '#';
              for (std::size_t i = 1; ok && i < 7; ++i) {
                const char c = (*accent)[i];
                ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
              }
              if (!ok) {
                cb(err_resp(drogon::k400BadRequest, "accent 须为 #rrggbb 形态"));
                return;
              }
            }
            std::int64_t version = 0;
            bool wrote = false;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              if (name.has_value() || accent.has_value() || slogan.has_value()) {
                version =
                    ctx->store->branding_set(name, accent, slogan, now_ms());
                if (version < 0) {
                  cb(err_resp(drogon::k500InternalServerError, "品牌配置写入失败"));
                  return;
                }
                wrote = true;
              }
              if (clear_logo) {
                version = ctx->store->branding_clear_logo();
                if (version < 0) {
                  cb(err_resp(drogon::k500InternalServerError, "品牌配置写入失败"));
                  return;
                }
                wrote = true;
              }
              if (clear_splash) {
                version = ctx->store->branding_clear_splash();
                if (version < 0) {
                  cb(err_resp(drogon::k500InternalServerError, "品牌配置写入失败"));
                  return;
                }
                wrote = true;
              }
            }
            if (!wrote) {
              cb(err_resp(drogon::k400BadRequest, "无可写品牌字段"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] branding.set version=" << version
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["version"] = version;
            cb(json_resp(drogon::k200OK, out));
          });

  // 品牌素材字节读面（素材非敏感：files 面本就登录前可读，这里回 PNG）
  guarded("/api/admin/branding/logo", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "branding.logo.get")) {
              cb(std::move(*deny));
              return;
            }
            const auto b = ctx->store->branding_get();
            if (b.logo.empty()) {
              cb(err_resp(drogon::k404NotFound, "素材未配置"));
              return;
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeString("image/png");
            resp->addHeader("Cache-Control", "no-cache");
            resp->setBody(std::string(b.logo.begin(), b.logo.end()));
            cb(resp);
          });

  guarded("/api/admin/branding/splash", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "branding.splash.get")) {
              cb(std::move(*deny));
              return;
            }
            const auto b = ctx->store->branding_get();
            if (b.splash.empty()) {
              cb(err_resp(drogon::k404NotFound, "素材未配置"));
              return;
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeString("image/png");
            resp->addHeader("Cache-Control", "no-cache");
            resp->setBody(std::string(b.splash.begin(), b.splash.end()));
            cb(resp);
          });

  // 品牌素材上传：原始字节体（非 JSON）→ PNG 校验（与 files 面
  // png_asset_check 同口径）→ 落库回新 version
  guarded("/api/admin/branding/logo", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "branding.logo.set")) {
              cb(std::move(*deny));
              return;
            }
            const std::string body = std::string(req->body());
            if (body.empty()) {
              cb(err_resp(drogon::k400BadRequest, "素材字节不可空"));
              return;
            }
            const auto [status, err] = png_asset_check(body);
            if (status != 0) {
              cb(err_resp(static_cast<drogon::HttpStatusCode>(status), err));
              return;
            }
            const std::vector<unsigned char> bytes(body.begin(), body.end());
            std::int64_t version = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              version = ctx->store->branding_set_logo(bytes, now_ms());
            }
            if (version < 0) {
              cb(err_resp(drogon::k500InternalServerError, "素材写入失败"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] branding.logo.upload bytes="
                      << body.size() << " version=" << version
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["version"] = version;
            cb(json_resp(drogon::k200OK, out));
          });

  guarded("/api/admin/branding/splash", {drogon::Post}, true,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "branding.splash.set")) {
              cb(std::move(*deny));
              return;
            }
            const std::string body = std::string(req->body());
            if (body.empty()) {
              cb(err_resp(drogon::k400BadRequest, "素材字节不可空"));
              return;
            }
            const auto [status, err] = png_asset_check(body);
            if (status != 0) {
              cb(err_resp(static_cast<drogon::HttpStatusCode>(status), err));
              return;
            }
            const std::vector<unsigned char> bytes(body.begin(), body.end());
            std::int64_t version = 0;
            {
              std::lock_guard<std::mutex> lk(ctx->mu);
              version = ctx->store->branding_set_splash(bytes, now_ms());
            }
            if (version < 0) {
              cb(err_resp(drogon::k500InternalServerError, "素材写入失败"));
              return;
            }
            std::cout << "[MEMEX-ADMIN] branding.splash.upload bytes="
                      << body.size() << " version=" << version
                      << " by=" << session_account(req) << "\n";
            Json::Value out = ok_json();
            out["version"] = version;
            cb(json_resp(drogon::k200OK, out));
          });

  // 表情包元数据清点（仅元数据：名称/大小/时间；字节与删除仅属主，见块头）
  guarded("/api/admin/emoji", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "emoji.list")) {
              cb(std::move(*deny));
              return;
            }
            const std::string account = req->getParameter("account");
            if (account.empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少参数：account"));
              return;
            }
            Json::Value arr(Json::arrayValue);
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->find_account(account)) {
              cb(err_resp(drogon::k404NotFound, "账号不存在"));
              return;
            }
            for (const auto& e : ctx->store->emoji_list(account)) {
              Json::Value row;
              row["id"] = e.id;
              row["name"] = e.name;
              row["size"] = e.size;
              row["ts_ms"] = e.ts_ms;
              arr.append(row);
            }
            Json::Value out = ok_json();
            out["account"] = account;
            out["emoji"] = arr;
            cb(json_resp(drogon::k200OK, out));
          });

  // 头像代读（组织内互见口径，files 面 ?account= 同径；上传/删除仅属主）
  guarded("/api/admin/avatar", {drogon::Get}, false,
          [ctx](const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            if (auto deny = require_manage(*ctx, req, "avatar.get")) {
              cb(std::move(*deny));
              return;
            }
            const std::string account = req->getParameter("account");
            if (account.empty()) {
              cb(err_resp(drogon::k400BadRequest, "缺少参数：account"));
              return;
            }
            const int size = atoi(req->getParameter("size").c_str());
            if (!ctx->store->avatar_size_valid(size)) {
              cb(err_resp(drogon::k400BadRequest, "档位仅支持 32/64/128/256"));
              return;
            }
            std::vector<unsigned char> bytes;
            std::string mime;
            std::lock_guard<std::mutex> lk(ctx->mu);
            if (!ctx->store->find_account(account)) {
              cb(err_resp(drogon::k404NotFound, "账号不存在"));
              return;
            }
            if (!ctx->store->avatar_bytes(account, size, bytes, mime)) {
              cb(err_resp(drogon::k404NotFound, "该账号未设置头像"));
              return;
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeString(mime.empty() ? "image/png" : mime);
            resp->addHeader("Cache-Control", "no-cache");
            resp->setBody(std::string(bytes.begin(), bytes.end()));
            cb(resp);
          });

  // ============ 健康状态 ============
  // admin 自身 + 库 + （可选）文件面探活（无鉴权 GET /files/health）。
  app.registerHandler(
      "/api/admin/health",
      [ctx](const drogon::HttpRequestPtr& req,
          std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (session_account(req).empty()) {
          cb(err_resp(drogon::k401Unauthorized, "未登录或会话已过期"));
          return;
        }
        const auto health_json = [ctx] {
          Json::Value out = ok_json();
          out["db"] = ctx->store->is_open();
          out["uptime_ms"] =
              static_cast<Json::Int64>(now_ms() - ctx->started_ms);
          return out;
        };
        if (ctx->files_url.empty()) {
          Json::Value out = health_json();
          out["files"] = "未配置（MEMEX_FILES_URL）";
          cb(json_resp(drogon::k200OK, out));
          return;
        }
        // 文件面探活：异步回调（探活失败＝files:"down"，admin 自身仍 ok）。
        // client 由回调捕获保活（悬垂＝在途回包解引用已析构对象）。
        auto client = drogon::HttpClient::newHttpClient(ctx->files_url,
                                                        drogon::app().getLoop());
        auto freq = drogon::HttpRequest::newHttpRequest();
        freq->setMethod(drogon::Get);
        freq->setPath("/files/health");
        client->sendRequest(
            freq,
            [client, cb, health_json](
                drogon::ReqResult r, const drogon::HttpResponsePtr& resp) {
              Json::Value out = health_json();
              out["files"] =
                  (r == drogon::ReqResult::Ok && resp &&
                   resp->getStatusCode() == drogon::k200OK)
                      ? "up"
                      : "down";
              cb(json_resp(drogon::k200OK, out));
            },
            3.0);
      },
      {drogon::Get});

  // —— 静态页（www/index.html＋JS/CSS）与会话 ——
  app.setDocumentRoot(www_dir);
  app.setHomePage("index.html");
  app.enableSession(static_cast<size_t>(session_ttl),
                    drogon::Cookie::SameSite::kLax);
  app.setLogLevel(trantor::Logger::kInfo);
  app.addListener("0.0.0.0", static_cast<uint16_t>(port));
  app.setThreadNum(1); // ServerStore 非线程安全：单 IO 线程串行化（有锁兜底）

  std::cout << "[MEMEX-ADMIN] admin 后台路由就绪：http://0.0.0.0:" << port
            << " db=" << db_path
            << (ctx->files_url.empty() ? "" : " files=" + ctx->files_url)
            << (ctx->storage_url.empty() ? "" : " storage=" + ctx->storage_url)
            << "\n";
  return 0;
}

int run(int argc, char** argv) {
  const int rc = setup(argc, argv);
  if (rc != 0) return rc;
  drogon::app().run();
  return 0;
}

} // namespace memex::admin
