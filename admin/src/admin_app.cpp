// memex 管理后台装配（LAY-004 拆分后）：配置解析（argv>env>默认）、
// 库/判权引擎装配、guarded 注册器定义、各域路由组装配调用（处理器在
// admin_app/ 目录，入口见 common.hpp）、静态页与会话/监听设置。
#include "admin_app.hpp"

#include "admin_app/common.hpp"

namespace memex::admin {

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

  // —— 统一注册小工具：登录态＋CSRF 闸后进业务体（形态见 common.hpp） ——
  const Guarded guarded = [&app](const char* path,
                                 const std::vector<
                                     drogon::internal::HttpConstraint>& methods,
                                 bool csrf, const Handler& body) {
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

  register_auth_routes(app, ctx, guarded);
  register_account_org_routes(app, ctx, guarded);
  register_audit_device_routes(app, ctx, guarded);
  register_notify_routes(app, ctx, guarded);
  register_files_quota_routes(app, ctx, guarded);
  register_group_routes(app, ctx, guarded);
  register_flow_routes(app, ctx, guarded);
  register_brand_health_routes(app, ctx, guarded);

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
