// 审批·信箱·日报（LAY-003 拆分）：R27-5 二期审批（请假起步）、
// 统一收件箱、撤回与个人日报台账。

#include "files_server/conn.hpp"

namespace memex::server {

// —— 二期·审批（请假起步，设计稿 docs/design/审批与日报周报.md §一）——
// 四态 pending|approved|rejected|withdrawn；审批人判定=az 规则
// approval-decide（直属上级现查；无上级=org-admin 兜底）；自建自审
// 不成立；全部动作留痕不删改
void FileConn::route_approval_create(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  // 一期固定模板「请假」：类型白名单（年假/事假/病假/调休）
  static const char* kTypes[] = {"年假", "事假", "病假", "调休"};
  if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
    respond_json(400, {{"ok", false},
                       {"error",
                        "缺少字段：type（年假/事假/病假/调休）；"
                        "from/to/reason 可选"}});
    return;
  }
  const std::string type = j["type"].get<std::string>();
  bool type_ok = false;
  for (const char* t : kTypes) {
    if (type == t) type_ok = true;
  }
  if (!type_ok) {
    respond_json(400, {{"ok", false},
                       {"error", "type 须为 年假/事假/病假/调休"}});
    return;
  }
  const std::string from = j.contains("from") && j["from"].is_string()
                               ? j["from"].get<std::string>()
                               : "";
  const std::string to = j.contains("to") && j["to"].is_string()
                             ? j["to"].get<std::string>()
                             : "";
  const std::string reason = j.contains("reason") && j["reason"].is_string()
                                 ? j["reason"].get<std::string>()
                                 : "";
  const std::int64_t id = impl_.store.approval_create(
      account, type, from, to, reason, now_ms());
  if (id <= 0) {
    respond_json(500, {{"ok", false}, {"error", "申请创建失败"}});
    return;
  }
  respond_json(200, {{"ok", true}, {"id", id}});
}
void FileConn::route_approvals_list() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json mine = json::array();
  for (const auto& a : impl_.store.approvals_of(account)) {
    mine.push_back({{"id", a.id},
                    {"type", a.type},
                    {"from", a.leave_from},
                    {"to", a.leave_to},
                    {"reason", a.reason},
                    {"status", a.status},
                    {"decider", a.decider},
                    {"decision_note", a.decision_note},
                    {"created_ms", a.created_ms},
                    {"decided_ms", a.decided_ms}});
  }
  // 待我决：全量 pending 逐行走 az approval:decide 现裁（不向无权者
  // 泄露他人申请的存在；判权不过的行直接不出现）
  json pending = json::array();
  for (const auto& a : impl_.store.approvals_pending()) {
    const Decision d = impl_.az.authorize(
        {account, "approval:decide", "user:" + a.applicant,
         "owner=" + a.applicant});
    if (!d.allowed) continue;
    pending.push_back({{"id", a.id},
                       {"applicant", a.applicant},
                       {"type", a.type},
                       {"from", a.leave_from},
                       {"to", a.leave_to},
                       {"reason", a.reason},
                       {"created_ms", a.created_ms}});
  }
  respond_json(200, {{"ok", true}, {"mine", mine}, {"pending", pending}});
}
void FileConn::route_approval_decide(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer() ||
      !j.contains("approved") || !j["approved"].is_boolean() ||
      (j.contains("note") && !j["note"].is_string())) {
    respond_json(400,
                 {{"ok", false}, {"error", "缺少字段：id/approved"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  const auto row = impl_.store.approval_by_id(id);
  if (!row.has_value()) {
    respond_json(404, {{"ok", false}, {"error", "申请不存在"}});
    return;
  }
  // 判权先于状态检查（对无权者与对不存在者同口径 403，不泄露状态）
  const Decision d = impl_.az.authorize(
      {account, "approval:decide", "user:" + row->applicant,
       "owner=" + row->applicant});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权决定该申请（" + d.reason +
                                 "；直属上级或无上级时 org-admin）"}});
    return;
  }
  if (row->status != "pending") {
    respond_json(409, {{"ok", false},
                       {"error", "该申请已决（" + row->status +
                                 "），不可重复决定"}});
    return;
  }
  const std::string note =
      j.contains("note") && j["note"].is_string()
          ? j["note"].get<std::string>()
          : "";
  if (!impl_.store.approval_decide(id, account, j["approved"].get<bool>(),
                                   note, now_ms())) {
    respond_json(409, {{"ok", false}, {"error", "决定落库失败（状态已变）"}});
    return;
  }
  std::cout << "[MEMEX] approval decide account=" << account
            << " id=" << id
            << " result=" << (j["approved"].get<bool>() ? "approved"
                                                        : "rejected")
            << std::endl;
  // 邮箱·通知入箱（mailbox.md §I：审批结果归通知类——落定才提醒申请
  // 人；状态行入箱即回执，正文仍在 approvals 台账，不走新推送链）
  {
    const auto dec = impl_.store.find_account(account);
    (void)impl_.store.mailbox_add(
        "notice",
        "你的「" + row->type + "」申请已" +
            (j["approved"].get<bool>() ? "通过" : "被驳回") + "（审批人 " +
            (dec ? dec->display_name : account) + "）",
        "审批结果 · 落定才提醒申请人", "审批结果", "approvals",
        "approval:" + std::to_string(id), {row->applicant}, now_ms());
  }
  respond_json(200, {{"ok", true}});
}
// —— 邮箱·统一收件箱（mailbox.md）：条目正文留在各自来源，本面只管
// 入箱/未读/已读/已处理/确认台账（按登录账号·按条目）；未读数以本面
// 为唯一来源（rail 徽章与收件箱页签同源同数）——
json FileConn::mailbox_row_json(const ServerStore::MailboxRow& r) {
  return json{{"id", r.id},
              {"kind", r.kind},
              {"title", r.title},
              {"sub", r.sub},
              {"source", r.source},
              {"ref", r.ref},
              {"time", r.created_ms},
              {"state", r.state},
              {"handledBy", r.handled_by},
              {"handledMs", r.handled_ms},
              {"confirmedMs", r.confirmed_ms},
              {"confirmTotal", r.confirm_total},
              {"confirmCount", r.confirm_count}};
}
void FileConn::route_mailbox_list(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::string kind;
  bool unread_only = false;
  std::string q;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    j = json::object(); // 空体=无条件全量（与任务列表同宽口径）
  }
  if (j.is_object()) {
    if (j.contains("type") && j["type"].is_string())
      kind = j["type"].get<std::string>();
    if (j.contains("unread") && j["unread"].is_boolean())
      unread_only = j["unread"].get<bool>();
    if (j.contains("q") && j["q"].is_string()) q = j["q"].get<std::string>();
  }
  if (!kind.empty() && kind != "warning" && kind != "notice" &&
      kind != "announce") {
    respond_json(400, {{"ok", false}, {"error", "type 须为空或 warning|notice|announce"}});
    return;
  }
  const auto rows = impl_.store.mailbox_of(account, kind, unread_only, q);
  const auto c = impl_.store.mailbox_counts(account);
  json items = json::array();
  for (const auto& r : rows) items.push_back(mailbox_row_json(r));
  respond_json(200,
               {{"ok", true},
                {"items", std::move(items)},
                {"counts", {{"all", c.all},
                            {"warning", c.warning},
                            {"notice", c.notice},
                            {"announce", c.announce}}}});
}
void FileConn::route_mailbox_unread() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const auto c = impl_.store.mailbox_counts(account);
  respond_json(200, {{"ok", true},
                     {"counts", {{"all", c.all},
                                 {"warning", c.warning},
                                 {"notice", c.notice},
                                 {"announce", c.announce}}}});
}
void FileConn::route_mailbox_read(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    j = json::object();
  }
  const bool all = j.is_object() && j.contains("all") &&
                   j["all"].is_boolean() && j["all"].get<bool>();
  if (all) {
    const int marked = impl_.store.mailbox_mark_all_read(account);
    respond_json(200, {{"ok", true}, {"marked", marked}});
    return;
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id（或 all=true）"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  const bool marked = impl_.store.mailbox_mark_read(id, account);
  respond_json(200, {{"ok", true}, {"marked", marked ? 1 : 0}});
}
void FileConn::route_mailbox_handle(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    j = json::object();
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  if (!impl_.store.mailbox_mark_handled(id, account, now_ms())) {
    respond_json(409, {{"ok", false},
                       {"error", "仅告警条目可标记已处理（或已处理过）"}});
    return;
  }
  std::cout << "[MEMEX] mailbox handle account=" << account << " id=" << id
            << std::endl;
  respond_json(200, {{"ok", true}});
}
void FileConn::route_mailbox_confirm(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    j = json::object();
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  if (!impl_.store.mailbox_confirm(id, account, now_ms())) {
    respond_json(409, {{"ok", false},
                       {"error", "仅公告条目可确认（或已确认过）"}});
    return;
  }
  std::cout << "[MEMEX] mailbox confirm account=" << account << " id=" << id
            << std::endl;
  respond_json(200, {{"ok", true}});
}
void FileConn::route_mailbox_confirmations(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    j = json::object();
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  // 台账只对本条目收件人开放（公告确认面＝收件人互见；判权=状态行在场）
  const auto rows = impl_.store.mailbox_of(account, "announce", false, "");
  const bool visible =
      std::any_of(rows.begin(), rows.end(),
                  [id](const ServerStore::MailboxRow& r) { return r.id == id; });
  if (!visible) {
    respond_json(403, {{"ok", false}, {"error", "仅公告收件人可查确认台账"}});
    return;
  }
  json out = json::array();
  for (const auto& r : impl_.store.mailbox_confirmations(id)) {
    out.push_back({{"account", r.account}, {"confirmedMs", r.confirmed_ms}});
  }
  respond_json(200, {{"ok", true}, {"rows", std::move(out)}});
}
void FileConn::route_approval_withdraw(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  const auto row = impl_.store.approval_by_id(id);
  if (!row.has_value()) {
    respond_json(404, {{"ok", false}, {"error", "申请不存在"}});
    return;
  }
  if (row->applicant != account) {
    respond_json(403, {{"ok", false}, {"error", "只有申请人能撤回"}});
    return;
  }
  if (row->status != "pending") {
    respond_json(409, {{"ok", false},
                       {"error", "该申请已决（" + row->status + "）"}});
    return;
  }
  if (!impl_.store.approval_withdraw(id, account)) {
    respond_json(409, {{"ok", false}, {"error", "撤回落库失败（状态已变）"}});
    return;
  }
  respond_json(200, {{"ok", true}});
}
// —— 日报周报（二期，设计稿 §二）：个人日报台账，当日重复=upsert
//    更新不留修订史；周报=按周聚合视图不单设表；直属上级可看下属
//   （az report:read，org-admin 不兜底）——
json FileConn::report_row_json(const ServerStore::ReportRow& r) {
  return {{"id", r.id},
          {"author", r.author},
          {"date", r.report_date},
          {"content", r.content},
          {"created_ms", r.created_ms},
          {"updated_ms", r.updated_ms}};
}
void FileConn::route_report_save(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("content") ||
      !j["content"].is_string() || j["content"].get<std::string>().empty()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：content"}});
    return;
  }
  const std::string date = j.contains("date") && j["date"].is_string()
                               ? j["date"].get<std::string>()
                               : "";
  if (!is_date_ymd(date)) {
    respond_json(400, {{"ok", false},
                       {"error", "date 须为 YYYY-MM-DD（日报归属日）"}});
    return;
  }
  const Decision d = impl_.az.authorize(
      {account, "report:write", "user:" + account, "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false}, {"error", "无权写入"}});
    return;
  }
  const std::int64_t id = impl_.store.report_upsert(
      account, date, j["content"].get<std::string>(), now_ms());
  if (id <= 0) {
    respond_json(500, {{"ok", false}, {"error", "日报落库失败"}});
    return;
  }
  std::cout << "[MEMEX] report save account=" << account
            << " date=" << date << std::endl;
  respond_json(200, {{"ok", true}, {"id", id}});
}
void FileConn::route_reports_mine() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json arr = json::array();
  for (const auto& r : impl_.store.reports_of(account)) {
    arr.push_back(report_row_json(r));
  }
  respond_json(200, {{"ok", true}, {"reports", std::move(arr)}});
}
void FileConn::route_reports_read() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::string author = query_param(query_, "author");
  if (author.empty()) {
    respond_json(400, {{"ok", false}, {"error", "缺少参数：author"}});
    return;
  }
  // 判权不过=403（幽灵账号同口径：链空且非本人——不泄露存在性）
  const Decision d = impl_.az.authorize(
      {account, "report:read", "user:" + author, "owner=" + author});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权查看该日报（直属上级可看下属）"}});
    return;
  }
  json arr = json::array();
  for (const auto& r : impl_.store.reports_of(author)) {
    arr.push_back(report_row_json(r));
  }
  respond_json(200, {{"ok", true},
                     {"author", author},
                     {"reports", std::move(arr)}});
}
void FileConn::route_reports_team() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  // 直接下属逐人聚合（org_reporting_lines 反查）；逐行再过 az 与单读
  // 同源（平台-3 现查现裁=转岗即时生效）
  json team = json::array();
  for (const auto& sub : impl_.store.direct_reports(account)) {
    const Decision d = impl_.az.authorize(
        {account, "report:read", "user:" + sub, "owner=" + sub});
    if (!d.allowed) continue;
    json rows = json::array();
    for (const auto& r : impl_.store.reports_of(sub)) {
      rows.push_back(report_row_json(r));
    }
    team.push_back({{"author", sub}, {"reports", std::move(rows)}});
  }
  respond_json(200, {{"ok", true}, {"team", std::move(team)}});
}

} // namespace memex::server
