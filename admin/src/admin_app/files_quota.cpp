// 文件与配额路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"


namespace memex::admin {

void register_files_quota_routes(drogon::HttpAppFramework& app,
                                 const std::shared_ptr<AdminContext>& ctx,
                                 const Guarded& guarded) {
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
}

} // namespace memex::admin
