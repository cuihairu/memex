// 品牌与外观、健康状态路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"


namespace memex::admin {

void register_brand_health_routes(drogon::HttpAppFramework& app,
                                  const std::shared_ptr<AdminContext>& ctx,
                                  const Guarded& guarded) {
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
}

} // namespace memex::admin
