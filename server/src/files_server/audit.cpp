// 会话审计与管理台归档检索（LAY-003 拆分）：auditor 闸、审计
// 上下文/撤回事件/导出与办公室位置图。

#include "files_server/conn.hpp"

namespace memex::server {

// —— 会话审计（二期）：CLI T3.2 查阅面同口径上移服务端——持 auditor
//    有效角色方可查（az audit:read；SecurityAuditor≠SystemAdmin）；
//    每次检索落查阅日志（谁/何时/条件摘要/命中几条）；被拒尝试也留痕
//   （audit.denied，审计自身被拒可对账）。查阅日志列表本身不落查阅
//    日志（台账自阅不自指，避免递归噪音）——
void FileConn::route_audit_search(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j = json::object();
  if (!body.empty()) {
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
  }
  if (!j.is_object()) {
    respond_json(400, {{"ok", false}, {"error", "请求体须为对象"}});
    return;
  }
  const std::string target =
      j.contains("account") && j["account"].is_string()
          ? j["account"].get<std::string>()
          : "";
  const std::string keyword =
      j.contains("keyword") && j["keyword"].is_string()
          ? j["keyword"].get<std::string>()
          : "";
  const std::int64_t since_ms =
      j.contains("since_ms") && j["since_ms"].is_number_integer()
          ? j["since_ms"].get<std::int64_t>()
          : 0;
  const std::int64_t until_ms =
      j.contains("until_ms") && j["until_ms"].is_number_integer()
          ? j["until_ms"].get<std::int64_t>()
          : 0;
  int limit = j.contains("limit") && j["limit"].is_number_integer()
                  ? static_cast<int>(j["limit"].get<std::int64_t>())
                  : 200;
  if (limit < 1) limit = 1;
  if (limit > 1000) limit = 1000;
  // 过滤条件摘要（进查阅日志；只记条件，不记消息内容——与 CLI 同款）
  std::string filters;
  const auto append_filter = [&filters](const std::string& kv) {
    if (!filters.empty()) filters += " ";
    filters += kv;
  };
  if (!target.empty()) append_filter("账号=" + target);
  if (!keyword.empty()) append_filter("关键词=" + keyword);
  if (since_ms > 0) append_filter("起=" + std::to_string(since_ms));
  if (until_ms > 0) append_filter("止=" + std::to_string(until_ms));
  const std::int64_t now = now_ms();
  const Decision d = impl_.az.authorize(
      {account, "audit:read", "audit", "owner=" + account});
  if (!d.allowed) {
    // 被拒尝试同样留痕（与 CLI --as 拒绝路径同口径）
    AuditReadRow denied;
    denied.op_account = account;
    denied.action = "audit.denied";
    denied.filters = filters;
    denied.ts_ms = now;
    impl_.store.add_audit_read(denied);
    respond_json(403, {{"ok", false},
                       {"error",
                        "无 auditor 有效角色（SystemAdmin 不自动可读消息，"
                        "须经授权授予 auditor）；被拒尝试已留痕"}});
    return;
  }
  MessageSearch q;
  q.account = target;
  q.keyword = keyword;
  q.since_ms = since_ms;
  q.until_ms = until_ms;
  q.limit = limit;
  const auto rows = impl_.store.search_messages(q);
  AuditReadRow rec;
  rec.op_account = account;
  rec.action = "audit.message.search";
  rec.filters = filters;
  rec.result_count = static_cast<int>(rows.size());
  rec.ts_ms = now;
  impl_.store.add_audit_read(rec);
  std::cout << "[MEMEX] audit search account=" << account
            << " hits=" << rows.size() << std::endl;
  json arr = json::array();
  for (const auto& m : rows) {
    arr.push_back({{"msg_id", m.msg_id},
                   {"from", m.from_account},
                   {"to", m.to_account},
                   {"type", memex::protocol::msg_type_name(
                                static_cast<memex::protocol::MsgType>(
                                    m.type))},
                   {"recalled", m.recalled},
                   {"ts_ms", m.ts_ms},
                   {"text", m.text}});
  }
  respond_json(200, {{"ok", true}, {"messages", std::move(arr)}});
}
void FileConn::route_audit_reads() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const Decision d = impl_.az.authorize(
      {account, "audit:read", "audit", "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error",
                        "无 auditor 有效角色（查阅日志同样持证查阅）"}});
    return;
  }
  int limit = 100;
  const std::string lim = query_param(query_, "limit");
  const int parsed = safe_int_param(lim, 0, 500);
  if (parsed > 0) limit = parsed;
  json arr = json::array();
  for (const auto& r : impl_.store.audit_reads(limit)) {
    arr.push_back({{"id", r.id},
                   {"op_account", r.op_account},
                   {"action", r.action},
                   {"filters", r.filters},
                   {"result_count", r.result_count},
                   {"ts_ms", r.ts_ms}});
  }
  respond_json(200, {{"ok", true}, {"reads", std::move(arr)}});
}
// —— 管理台归档检索（desktop-search）三面：与检索面同闸（auditor 有效
//    角色）同留痕（每次查阅落 audit_reads；被拒落 audit.denied）——
// audit 判权＋被拒留痕（三面共用；通过返回 true 并已应答 403）
bool FileConn::audit_gate_denied(const std::string& account,
                       const std::string& filters) {
  const Decision d = impl_.az.authorize(
      {account, "audit:read", "audit", "owner=" + account});
  if (d.allowed) return false;
  AuditReadRow denied;
  denied.op_account = account;
  denied.action = "audit.denied";
  denied.filters = filters;
  denied.ts_ms = now_ms();
  impl_.store.add_audit_read(denied);
  respond_json(403, {{"ok", false},
                     {"error",
                      "无 auditor 有效角色；被拒尝试已留痕"}});
  return true;
}
// GET /files/audit/context?msg_id=&span=：命中消息的会话窗口（升序，
// 命中行前后各 span 行，span 夹 1..50）
void FileConn::route_audit_context() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::string msg_id = query_param(query_, "msg_id");
  if (msg_id.empty()) {
    respond_json(400, {{"ok", false}, {"error", "msg_id 不能为空"}});
    return;
  }
  int span = 5;
  const std::string span_s = query_param(query_, "span");
  const int parsed = safe_int_param(span_s, 0, 500);
  if (parsed > 0) span = parsed;
  const std::string filters = "msg_id=" + msg_id.substr(0, 12);
  if (audit_gate_denied(account, filters)) return;
  const auto rows = impl_.store.context_around(msg_id, span);
  AuditReadRow rec;
  rec.op_account = account;
  rec.action = "audit.message.context";
  rec.filters = filters;
  rec.result_count = static_cast<int>(rows.size());
  rec.ts_ms = now_ms();
  impl_.store.add_audit_read(rec);
  std::cout << "[MEMEX] audit context account=" << account
            << " rows=" << rows.size() << std::endl;
  json arr = json::array();
  for (const auto& m : rows) {
    arr.push_back({{"msg_id", m.msg_id},
                   {"from", m.from_account},
                   {"to", m.to_account},
                   {"type", memex::protocol::msg_type_name(
                                static_cast<memex::protocol::MsgType>(
                                    m.type))},
                   {"recalled", m.recalled},
                   {"ts_ms", m.ts_ms},
                   {"text", m.text}});
  }
  respond_json(200, {{"ok", true}, {"msg_id", msg_id},
                     {"messages", std::move(arr)}});
}
// GET /files/audit/recall-event?msg_id=：该消息的撤回事件（撤回人＋时刻；
// 未撤回 exists=false）
void FileConn::route_audit_recall_event() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::string msg_id = query_param(query_, "msg_id");
  if (msg_id.empty()) {
    respond_json(400, {{"ok", false}, {"error", "msg_id 不能为空"}});
    return;
  }
  const std::string filters = "msg_id=" + msg_id.substr(0, 12);
  if (audit_gate_denied(account, filters)) return;
  const auto ev = impl_.store.last_recall_event(msg_id);
  AuditReadRow rec;
  rec.op_account = account;
  rec.action = "audit.recall.event";
  rec.filters = filters;
  rec.result_count = ev.has_value() ? 1 : 0;
  rec.ts_ms = now_ms();
  impl_.store.add_audit_read(rec);
  json body = {{"ok", true}, {"msg_id", msg_id}, {"exists", ev.has_value()}};
  if (ev.has_value()) {
    body["by_account"] = ev->by_account;
    body["ts_ms"] = ev->ts_ms;
  }
  respond_json(200, body);
}
// POST /files/audit/export：与检索同条件重跑＋落 audit.message.export 留痕
//（证据文件由客户端写盘——路径在操作者本机，服务端只回行与对账元数据）
void FileConn::route_audit_export(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j = json::object();
  if (!body.empty()) {
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
  }
  if (!j.is_object()) {
    respond_json(400, {{"ok", false}, {"error", "请求体须为对象"}});
    return;
  }
  const std::string target =
      j.contains("account") && j["account"].is_string()
          ? j["account"].get<std::string>()
          : "";
  const std::string keyword =
      j.contains("keyword") && j["keyword"].is_string()
          ? j["keyword"].get<std::string>()
          : "";
  const std::int64_t since_ms =
      j.contains("since_ms") && j["since_ms"].is_number_integer()
          ? j["since_ms"].get<std::int64_t>()
          : 0;
  const std::int64_t until_ms =
      j.contains("until_ms") && j["until_ms"].is_number_integer()
          ? j["until_ms"].get<std::int64_t>()
          : 0;
  int limit = j.contains("limit") && j["limit"].is_number_integer()
                  ? static_cast<int>(j["limit"].get<std::int64_t>())
                  : 200;
  if (limit < 1) limit = 1;
  if (limit > 1000) limit = 1000;
  std::string filters;
  const auto append_filter = [&filters](const std::string& kv) {
    if (!filters.empty()) filters += " ";
    filters += kv;
  };
  if (!target.empty()) append_filter("账号=" + target);
  if (!keyword.empty()) append_filter("关键词=" + keyword);
  if (since_ms > 0) append_filter("起=" + std::to_string(since_ms));
  if (until_ms > 0) append_filter("止=" + std::to_string(until_ms));
  if (audit_gate_denied(account, filters)) return;
  MessageSearch q;
  q.account = target;
  q.keyword = keyword;
  q.since_ms = since_ms;
  q.until_ms = until_ms;
  q.limit = limit;
  const auto rows = impl_.store.search_messages(q);
  const std::int64_t now = now_ms();
  AuditReadRow rec;
  rec.op_account = account;
  rec.action = "audit.message.export";
  rec.filters = filters;
  rec.result_count = static_cast<int>(rows.size());
  rec.ts_ms = now;
  impl_.store.add_audit_read(rec);
  std::cout << "[MEMEX] audit export account=" << account
            << " rows=" << rows.size() << std::endl;
  json arr = json::array();
  for (const auto& m : rows) {
    arr.push_back({{"msg_id", m.msg_id},
                   {"from", m.from_account},
                   {"to", m.to_account},
                   {"type", memex::protocol::msg_type_name(
                                static_cast<memex::protocol::MsgType>(
                                    m.type))},
                   {"recalled", m.recalled},
                   {"ts_ms", m.ts_ms},
                   {"text", m.text}});
  }
  respond_json(200, {{"ok", true},
                     {"messages", std::move(arr)},
                     {"operator", account},
                     {"exported_ms", now}});
}
// —— 办公室位置图（二期，设计稿 docs/design/办公室位置图.md）：
//     看自楼层（同层互见=位置图语义本体；无工位=空图不造楼层）；
//     编辑权 org-admin（az office:manage）；工位即楼层归属——
json FileConn::seat_row_json(const ServerStore::SeatRow& s) {
  return {{"id", s.id},
          {"floor", s.floor},
          {"label", s.label},
          {"x", s.x},
          {"y", s.y},
          {"account", s.account}};
}
bool FileConn::office_can_manage(const std::string& account) {
  const Decision d = impl_.az.authorize(
      {account, "office:manage", "office", "owner=" + account});
  return d.allowed;
}
// 品牌物料管理判权（同 office 口径：组织级面归 org-admin）
bool FileConn::branding_can_manage(const std::string& account) {
  const Decision d = impl_.az.authorize(
      {account, "branding:manage", "branding", "owner=" + account});
  return d.allowed;
}
void FileConn::route_office_map() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::string floor;
  if (office_can_manage(account)) {
    // org-admin 可跨层查看（?floor= 指定；缺省=自楼层）
    floor = query_param(query_, "floor");
  }
  if (floor.empty()) {
    const auto mine = impl_.store.seat_of(account);
    floor = mine.has_value() ? mine->floor : ""; // 无工位=空图不造楼层
  }
  json arr = json::array();
  if (!floor.empty()) {
    for (const auto& s : impl_.store.seats_on_floor(floor)) {
      arr.push_back(seat_row_json(s));
    }
  }
  respond_json(200, {{"ok", true},
                     {"floor", floor},
                     {"can_manage", office_can_manage(account)},
                     {"seats", std::move(arr)}});
}
void FileConn::route_office_seat(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  if (!office_can_manage(account)) {
    respond_json(403, {{"ok", false}, {"error", "工位编辑归 org-admin"}});
    return;
  }
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (j.is_object() && j.contains("remove") && j["remove"].is_boolean() &&
      j["remove"].get<bool>()) {
    if (!j.contains("id") || !j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
      return;
    }
    if (!impl_.store.seat_delete(j["id"].get<std::int64_t>())) {
      respond_json(404, {{"ok", false}, {"error", "工位不存在"}});
      return;
    }
    respond_json(200, {{"ok", true}});
    return;
  }
  if (!j.is_object() || !j.contains("floor") || !j["floor"].is_string() ||
      !j.contains("label") || !j["label"].is_string() ||
      !j.contains("x") || !j["x"].is_number() || !j.contains("y") ||
      !j["y"].is_number()) {
    respond_json(
        400, {{"ok", false}, {"error", "缺少字段：floor/label/x/y"}});
    return;
  }
  double x = j["x"].get<double>();
  double y = j["y"].get<double>();
  // 归一化坐标夹 0~1（越界=夹回不造假数据）
  x = std::max(0.0, std::min(1.0, x));
  y = std::max(0.0, std::min(1.0, y));
  const std::int64_t id = impl_.store.seat_upsert(
      j["floor"].get<std::string>(), j["label"].get<std::string>(), x, y,
      now_ms());
  if (id <= 0) {
    respond_json(500, {{"ok", false}, {"error", "工位落库失败"}});
    return;
  }
  respond_json(200, {{"ok", true}, {"id", id}});
}
void FileConn::route_office_bind(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  if (!office_can_manage(account)) {
    respond_json(403, {{"ok", false}, {"error", "工位绑定归 org-admin"}});
    return;
  }
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer() ||
      (j.contains("account") && !j["account"].is_string())) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
    return;
  }
  const std::string target =
      j.contains("account") ? j["account"].get<std::string>() : "";
  // 换座纪律：一人一工位（部分唯一索引守卫）——目标已占用他座=拒，
  // 先解绑再绑（不静默顶替他人座位）
  if (!impl_.store.seat_bind(j["id"].get<std::int64_t>(), target,
                             now_ms())) {
    respond_json(409, {{"ok", false},
                       {"error",
                        "绑定失败（工位不存在、账号不存在或该账号已占用"
                        "其他工位——换座须先解绑）"}});
    return;
  }
  std::cout << "[MEMEX] office seat " << (target.empty() ? "unbind"
                                                         : "bind")
            << " id=" << j["id"].get<std::int64_t>()
            << (target.empty() ? "" : " account=" + target)
            << " by=" << account << std::endl;
  respond_json(200, {{"ok", true}});
}

// —— 品牌物料（设计稿 docs/design/品牌物料.md）：读三免鉴权（登录窗
// 要在鉴权前显示，物料非敏感）；写三判权 branding-manage（org-admin）——
static constexpr std::size_t kBrandAssetMax = 512 * 1024; // 512KiB

} // namespace memex::server
