// 服务器 agent 与远程会话（LAY-003 拆分）：R26-1 注册/心跳/列表、
// R26-3 一次性短票会话、R26-4 服务器凭据。

#include "files_server/conn.hpp"

namespace memex::server {

// —— R26-1 服务器 agent 面 ——
// 登记：管理面（memo:config）。生成注册令牌（明文只此一次出现在回包，
// 库里只存 SHA-256 摘要）；同 gid+name 重登记=轮换令牌、id 稳定。
void FileConn::route_group_servers_enroll(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("gid") ||
      !j["gid"].is_number_integer() || !j.contains("name") ||
      !j["name"].is_string() || !j.contains("host") ||
      !j["host"].is_string()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/name/host"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::string name = j["name"].get<std::string>();
  const std::string host = j["host"].get<std::string>();
  if (gid == 0 || name.empty() || host.empty()) {
    respond_json(400,
                 {{"ok", false},
                  {"error", "gid 须为正整数、name/host 非空"}});
    return;
  }
  if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:config", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(
        403, {{"ok", false}, {"error", "无权登记服务器（仅群主/管理员）"}});
    return;
  }
  const std::string token = random_salt_hex(); // 16B 熵 → 32 hex
  const std::uint64_t id =
      impl_.store.server_enroll(gid, name, host, sha256_hex(token), account,
                                now_ms());
  if (id == 0) {
    respond_json(404, {{"ok", false}, {"error", "群不存在"}});
    return;
  }
  // 令牌不进日志（只进回包一次）
  std::cout << "[MEMEX] files group-servers enroll account=" << account
            << " gid=" << gid << " id=" << id << " name=" << name
            << " host=" << host << std::endl;
  respond_json(200, {{"ok", true},
                     {"gid", gid},
                     {"id", id},
                     {"name", name},
                     {"token", token}});
}
// 心跳：agent 令牌鉴权（非人会话——不走 Authorization 头）。指标整拍
// 覆盖写；令牌错/轮换掉的旧令牌一律 401。
void FileConn::route_group_servers_heartbeat(const std::string& body) {
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("token") || !j["token"].is_string() ||
      j["token"].get<std::string>().empty()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：token"}});
    return;
  }
  for (const char* k :
       {"cpu_percent", "mem_used_mb", "mem_total_mb", "disk_used_mb",
        "disk_total_mb", "load1"}) {
    if (!j.contains(k) || !j[k].is_number()) {
      respond_json(400,
                   {{"ok", false},
                    {"error", std::string("缺少数值字段：") + k}});
      return;
    }
  }
  const auto srv = impl_.store.server_by_token_hash(
      sha256_hex(j["token"].get<std::string>()));
  if (!srv.has_value()) {
    respond_json(401, {{"ok", false}, {"error", "注册令牌无效"}});
    return;
  }
  if (!capability_gate(srv->group_id, "server_tools", "群服务器工具")) return;
  impl_.store.server_heartbeat(
      srv->id, j["cpu_percent"].get<double>(),
      j["mem_used_mb"].get<double>(), j["mem_total_mb"].get<double>(),
      j["disk_used_mb"].get<double>(), j["disk_total_mb"].get<double>(),
      j["load1"].get<double>(), now_ms());
  // 邮箱·告警入箱（mailbox.md §I/P3：admin 事件源——agent 心跳磁盘
  // 超 85% 阈值入告警；受众=admin 账号集（§Ⅵ-2 默认：非管理员不看
  // 系统详情）；dedup=服务器位：持续超阈值不重复报，全员处理后复发
  // 可再报（store 层告警再入语义））
  const double disk_total = j["disk_total_mb"].get<double>();
  const double disk_pct =
      disk_total > 0 ? j["disk_used_mb"].get<double>() * 100.0 / disk_total
                     : 0.0;
  if (disk_pct >= 85.0) {
    (void)impl_.store.mailbox_add(
        "warning", "磁盘使用率超过 85%（" + srv->name + "）",
        "健康检查 · 持续未处理，红点不消失", "admin 事件源", "",
        "disk:" + std::to_string(srv->id), impl_.store.admin_accounts(),
        now_ms());
  }
  respond_json(200, {{"ok", true},
                     {"id", srv->id},
                     {"gid", srv->group_id},
                     {"interval_s", kAgentOnlineMs / 3000}});
}
// 列表：群成员（入群即授权）。带最近一拍指标＋在线红绿灯（last_seen
// 新鲜度）；令牌摘要永不出现。
void FileConn::route_group_servers_list() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::uint64_t gid = 0;
  if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
    respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
    return;
  }
  if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
  if (!tool_call_allowed(account, gid)) {
    respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
    return;
  }
  const std::int64_t now = now_ms();
  json arr = json::array();
  for (const auto& r : impl_.store.server_list(gid)) {
    // cred 掩码元数据：只言「是否已配置/谁/何时」——密文与明文永不出门
    arr.push_back(
        {{"id", r.id},
         {"name", r.name},
         {"host", r.host},
         {"enrolled_by", r.enrolled_by},
         {"created_ms", r.created_ms},
         {"last_seen_ms", r.last_seen_ms},
         {"online", now - r.last_seen_ms < kAgentOnlineMs},
         {"cpu_percent", r.cpu_percent},
         {"mem_used_mb", r.mem_used_mb},
         {"mem_total_mb", r.mem_total_mb},
         {"disk_used_mb", r.disk_used_mb},
         {"disk_total_mb", r.disk_total_mb},
         {"load1", r.load1},
         {"cred", !r.cred_updated_by.empty()},
         {"cred_updated_by", r.cred_updated_by},
         {"cred_updated_ms", r.cred_updated_ms}});
  }
  respond_json(200, {{"ok", true}, {"gid", gid}, {"servers", arr}});
}
// —— R26-3 远程会话（SSH 起步；memex 只做「看+连」，操作类归
// croupier）——
// 签发一次性短票（群成员；server_id 须属该群）。留痕即签发：行内记
// 谁/何时/连哪台/协议；短票明文只此一次出门，库里只存摘要。
void FileConn::route_group_servers_session_request(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("gid") ||
      !j["gid"].is_number_integer() || !j.contains("server_id") ||
      !j["server_id"].is_number_integer()) {
    respond_json(400,
                 {{"ok", false}, {"error", "缺少字段：gid/server_id"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::uint64_t sid =
      static_cast<std::uint64_t>(j["server_id"].get<std::int64_t>());
  const std::string protocol =
      j.contains("protocol") && j["protocol"].is_string()
          ? j["protocol"].get<std::string>()
          : "ssh";
  if (gid == 0 || sid == 0) {
    respond_json(400,
                 {{"ok", false},
                  {"error", "gid/server_id 须为正整数"}});
    return;
  }
  if (protocol != "ssh") {
    // RDP/VNC 随后（走同一短票/留痕面，到批再开）
    respond_json(400,
                 {{"ok", false},
                  {"error", "协议暂只支持 ssh（RDP/VNC 随后）"}});
    return;
  }
  if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
  if (!tool_call_allowed(account, gid)) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权发起会话（非群成员）"}});
    return;
  }
  const std::string ticket = random_salt_hex();
  const std::uint64_t id = impl_.store.server_session_open(
      gid, sid, account, protocol, sha256_hex(ticket), now_ms());
  if (id == 0) {
    respond_json(404, {{"ok", false}, {"error", "服务器不存在"}});
    return;
  }
  // 短票不进日志
  std::cout << "[MEMEX] files group-servers session request account="
            << account << " gid=" << gid << " server=" << sid
            << " session=" << id << " protocol=" << protocol << std::endl;
  respond_json(200, {{"ok", true},
                     {"gid", gid},
                     {"session_id", id},
                     {"protocol", protocol},
                     {"ticket", ticket},
                     {"expires_ms", now_ms() + kSessionTicketTtlMs}});
}
// 短票兑现（一次性；短票即凭据——无会话头）。客户端拉起本地 ssh 前
// 即时兑现；过期/错票 401、重放 409。
void FileConn::route_group_servers_session_redeem(const std::string& body) {
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("ticket") || !j["ticket"].is_string() ||
      j["ticket"].get<std::string>().empty()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：ticket"}});
    return;
  }
  const auto sess = impl_.store.server_session_by_ticket(
      sha256_hex(j["ticket"].get<std::string>()));
  if (!sess.has_value() ||
      now_ms() - sess->opened_ms > kSessionTicketTtlMs) {
    respond_json(401, {{"ok", false}, {"error", "短票无效或已过期"}});
    return;
  }
  if (!capability_gate(sess->group_id, "server_tools", "群服务器工具")) return;
  if (!impl_.store.server_session_mark_redeemed(sess->id, now_ms())) {
    respond_json(409, {{"ok", false}, {"error", "短票已使用"}});
    return;
  }
  std::cout << "[MEMEX] files group-servers session redeem session="
            << sess->id << " account=" << sess->actor << std::endl;
  respond_json(200, {{"ok", true},
                     {"session_id", sess->id},
                     {"gid", sess->group_id},
                     {"server_name", sess->server_name},
                     {"host", sess->host},
                     {"protocol", sess->protocol}});
}
// 收尾（本人、进行中才可）：closed_ms 落点＝时长可算（路由层算，
// 行内存原始两拍）
void FileConn::route_group_servers_session_close(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("gid") ||
      !j["gid"].is_number_integer() || !j.contains("session_id") ||
      !j["session_id"].is_number_integer()) {
    respond_json(400,
                 {{"ok", false}, {"error", "缺少字段：gid/session_id"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::uint64_t id =
      static_cast<std::uint64_t>(j["session_id"].get<std::int64_t>());
  if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
  if (!tool_call_allowed(account, gid)) {
    respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
    return;
  }
  if (!impl_.store.server_session_close(gid, id, account, now_ms())) {
    respond_json(409,
                 {{"ok", false},
                  {"error", "会话不存在/非本人/已结束"}});
    return;
  }
  std::cout << "[MEMEX] files group-servers session close session=" << id
            << " account=" << account << std::endl;
  respond_json(200, {{"ok", true}, {"gid", gid}, {"session_id", id}});
}
// 接入留痕列表（群成员）：谁/何时/连哪台/协议/是否兑现/时长；短票
// 摘要永不出现
void FileConn::route_group_servers_sessions() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::uint64_t gid = 0;
  if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
    respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
    return;
  }
  if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
  if (!tool_call_allowed(account, gid)) {
    respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
    return;
  }
  json arr = json::array();
  for (const auto& s : impl_.store.server_session_list(gid)) {
    arr.push_back(
        {{"id", s.id},
         {"server_id", s.server_id},
         {"server_name", s.server_name},
         {"host", s.host},
         {"actor", s.actor},
         {"protocol", s.protocol},
         {"opened_ms", s.opened_ms},
         {"redeemed", s.redeemed_ms > 0},
         {"open", s.closed_ms == 0},
         {"duration_ms",
          s.closed_ms > 0 ? s.closed_ms - s.opened_ms : 0}});
  }
  respond_json(200, {{"ok", true}, {"gid", gid}, {"sessions", arr}});
}
// —— R26-4 服务器凭据（管理面）：上/覆盖/删。密文落库，回包只见
// updated 旗标——value 明文与 sealed_hex 密文都只此一次进服务端 ——
void FileConn::route_group_servers_credential(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("gid") ||
      !j["gid"].is_number_integer() || !j.contains("server_id") ||
      !j["server_id"].is_number_integer()) {
    respond_json(400,
                 {{"ok", false}, {"error", "缺少字段：gid/server_id"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::uint64_t sid =
      static_cast<std::uint64_t>(j["server_id"].get<std::int64_t>());
  const bool is_delete = j.contains("op") && j["op"].is_string() &&
                         j["op"].get<std::string>() == "delete";
  std::string value;
  if (!is_delete) {
    if (!j.contains("value") || !j["value"].is_string() ||
        j["value"].get<std::string>().empty()) {
      respond_json(
          400, {{"ok", false},
                {"error", "缺少字段：value（非空字符串；删除传 op=\"delete\"）"}});
      return;
    }
    value = j["value"].get<std::string>();
  }
  if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:config", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权管理服务器凭据（仅群主/管理员）"}});
    return;
  }
  if (impl_.tool_cred_key.empty()) {
    respond_json(503, {{"ok", false},
                       {"error", "凭据面未启用（服务端未配置 --tool-cred-secret）"}});
    return;
  }
  if (is_delete) {
    if (!impl_.store.server_cred_delete(gid, sid)) {
      respond_json(404, {{"ok", false}, {"error", "凭据不存在或服务器不属该群"}});
      return;
    }
    std::cout << "[MEMEX] files group-servers credential delete account="
              << account << " gid=" << gid << " server=" << sid << std::endl;
    respond_json(200,
                 {{"ok", true}, {"gid", gid}, {"server_id", sid}, {"deleted", true}});
    return;
  }
  const std::string sealed = gcm_seal(impl_.tool_cred_key, value);
  if (sealed.empty()) {
    respond_json(500, {{"ok", false}, {"error", "凭据加密失败"}});
    return;
  }
  if (!impl_.store.server_cred_set(gid, sid, sealed, account, now_ms())) {
    respond_json(404, {{"ok", false}, {"error", "服务器不存在或不属该群"}});
    return;
  }
  // 留痕不含 value（明文/密文都不进日志）
  std::cout << "[MEMEX] files group-servers credential set account="
            << account << " gid=" << gid << " server=" << sid
            << " sealed_bytes=" << sealed.size() / 2 << std::endl;
  respond_json(200,
               {{"ok", true}, {"gid", gid}, {"server_id", sid}, {"updated", true}});
}

} // namespace memex::server
