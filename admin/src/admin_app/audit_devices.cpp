// 归档检索与导出（auditor 证）、设备与登录台账路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"


namespace memex::admin {

void register_audit_device_routes(drogon::HttpAppFramework& app,
                                  const std::shared_ptr<AdminContext>& ctx,
                                  const Guarded& guarded) {
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
}

} // namespace memex::admin
