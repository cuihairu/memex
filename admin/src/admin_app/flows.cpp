// 业务流程面、任务源拉取（两道闸默认关）路由组（LAY-004 自 admin_app.cpp 机械搬移，处理器体逐字保留）。
// 装配与 guarded 定义在 admin_app.cpp；共性面见 common.hpp/cpp。
#include "common.hpp"


namespace memex::admin {

void register_flow_routes(drogon::HttpAppFramework& app,
                          const std::shared_ptr<AdminContext>& ctx,
                          const Guarded& guarded) {
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
}

} // namespace memex::admin
