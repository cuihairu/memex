// 备忘录与群备忘录（LAY-003 拆分）：R23-3 文本备忘、R24-2 群
// 维度共享知识（修订史留痕、回滚、编辑锁）。

#include "files_server/conn.hpp"

namespace memex::server {

// —— R23-3 备忘录（文件助手文本面）：判权走 AuthorizationService，
//    personal-owner 仅本人（resource "user:{owner}/memo:{id}" 前缀匹配
//    即中、不限 action），他人一律 default-deny——不自造权限规则 ——
std::string FileConn::memo_resource(const std::string& owner,
                                 std::int64_t id) {
  return "user:" + owner + "/memo:" + std::to_string(id);
}
void FileConn::route_memo_write(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("content") || !j["content"].is_string() ||
      j["content"].get<std::string>().empty()) {
    respond_json(400, {{"ok", false},
                       {"error", "缺少字段：content（非空字符串）"}});
    return;
  }
  const std::string content = j["content"].get<std::string>();
  if (j.contains("id")) {
    if (!j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "id 须为整数"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.memo_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
      return;
    }
    const Decision d = impl_.az.authorize(
        {account, "memo:update", memo_resource(row->owner, id),
         "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权编辑（" + d.reason + "）"}});
      return;
    }
    if (!impl_.store.update_memo(id, account, content, now_ms())) {
      respond_json(500, {{"ok", false}, {"error", "备忘录更新失败"}});
      return;
    }
    std::cout << "[MEMEX] files memo update account=" << account
              << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}, {"id", id}});
    return;
  }
  const Decision d = impl_.az.authorize(
      {account, "memo:create", "user:" + account, "owner=" + account});
  if (!d.allowed) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权新建（" + d.reason + "）"}});
    return;
  }
  const std::int64_t id = impl_.store.create_memo(account, content, now_ms());
  if (id <= 0) {
    respond_json(500, {{"ok", false}, {"error", "备忘录落库失败"}});
    return;
  }
  std::cout << "[MEMEX] files memo create account=" << account
            << " id=" << id << std::endl;
  respond_json(200, {{"ok", true}, {"id", id}});
}
void FileConn::route_memo() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::string id_s = query_param(query_, "id");
  if (!id_s.empty()) {
    if (id_s.find_first_not_of("0123456789") != std::string::npos) {
      respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
      return;
    }
    const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
    const auto row = impl_.store.memo_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
      return;
    }
    const Decision d = impl_.az.authorize(
        {account, "memo:read", memo_resource(row->owner, id),
         "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权读取（" + d.reason + "）"}});
      return;
    }
    respond_json(200,
                 {{"ok", true},
                  {"memo",
                   {{"id", row->id},
                    {"owner", row->owner},
                    {"content", row->content},
                    {"created_ms", row->created_ms},
                    {"updated_ms", row->updated_ms}}}});
    return;
  }
  const Decision d = impl_.az.authorize(
      {account, "memo:read", "user:" + account, "owner=" + account});
  if (!d.allowed) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权列表（" + d.reason + "）"}});
    return;
  }
  int limit = 200, offset = 0;
  const std::string lim = query_param(query_, "limit");
  const std::string off = query_param(query_, "offset");
  limit = safe_int_param(lim, 200, 1000);
  offset = safe_int_param(off, 0, 999999999);
  const auto rows = impl_.store.list_memos(account, limit, offset);
  json arr = json::array();
  for (const auto& m : rows) {
    arr.push_back({{"id", m.id},
                   {"owner", m.owner},
                   {"content", m.content},
                   {"created_ms", m.created_ms},
                   {"updated_ms", m.updated_ms}});
  }
  respond_json(200, {{"ok", true}, {"memos", arr}});
}
void FileConn::route_memo_delete() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::string id_s = query_param(query_, "id");
  if (id_s.empty() ||
      id_s.find_first_not_of("0123456789") != std::string::npos) {
    respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
    return;
  }
  const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
  const auto row = impl_.store.memo_by_id(id);
  if (!row.has_value()) {
    respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
    return;
  }
  const Decision d = impl_.az.authorize(
      {account, "memo:delete", memo_resource(row->owner, id),
       "owner=" + account});
  if (!d.allowed) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权删除（" + d.reason + "）"}});
    return;
  }
  if (!impl_.store.delete_memo(id, account)) {
    respond_json(500, {{"ok", false}, {"error", "备忘录删除失败"}});
    return;
  }
  std::cout << "[MEMEX] files memo delete account=" << account
            << " id=" << id << std::endl;
  respond_json(200, {{"ok", true}});
}
// —— R24-2 群备忘录（群维度共享知识：标题+正文；修订史逐笔留痕可回滚）——
// 判权映射（不自造规则）：管理写 memo:write / 删 memo:delete / 开关
// memo:config 由 group-owner/group-admin 规则命中；成员读 file:read /
// file:list 由 group-member 继承命中。开放编辑是产品开关（设计：
// 管理员维护或开放编辑），开着时对写/回滚叠加成员判定——route 层裁量。
std::string FileConn::group_resource(std::uint64_t gid) {
  return "group:" + std::to_string(gid);
}
std::string FileConn::group_memo_resource(std::uint64_t gid, std::int64_t id) {
  return group_resource(gid) + "/memo:" + std::to_string(id);
}
bool FileConn::parse_gid_param(const std::string& s, std::uint64_t* out) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  *out = std::strtoull(s.c_str(), nullptr, 10);
  return *out > 0;
}
bool FileConn::group_memo_write_allowed(const std::string& account,
                              std::uint64_t gid,
                              const std::string& resource) {
  if (impl_.az
          .authorize({account, "memo:write", resource, "owner=" + account})
          .allowed) {
    return true;
  }
  return impl_.store.group_memo_open_edit(gid) &&
         impl_.az
             .authorize({account, "file:read", group_resource(gid),
                         "owner=" + account})
             .allowed;
}
void FileConn::route_group_memo_list() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::uint64_t gid = 0;
  if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
    respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
    return;
  }
  if (!capability_gate(gid, "memo", "群备忘录")) return;
  const Decision d = impl_.az.authorize(
      {account, "file:list", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权查看（" + d.reason + "）"}});
    return;
  }
  int limit = 200, offset = 0;
  const std::string lim = query_param(query_, "limit");
  const std::string off = query_param(query_, "offset");
  limit = safe_int_param(lim, 200, 1000);
  offset = safe_int_param(off, 0, 999999999);
  const auto rows = impl_.store.list_group_memos(
      gid, query_param(query_, "q"), limit, offset);
  json arr = json::array();
  for (const auto& m : rows) {
    arr.push_back({{"id", m.id},
                   {"title", m.title},
                   {"content", m.content},
                   {"author", m.author},
                   {"created_ms", m.created_ms},
                   {"updated_ms", m.updated_ms}});
  }
  respond_json(200, {{"ok", true},
                     {"gid", gid},
                     {"open_edit", impl_.store.group_memo_open_edit(gid)},
                     {"memos", arr}});
}
void FileConn::route_group_memo_save(const std::string& body) {
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
      !j["gid"].is_number_integer() ||
      !j.contains("title") || !j["title"].is_string() ||
      !j.contains("content") || !j["content"].is_string() ||
      j["title"].get<std::string>().empty() ||
      j["content"].get<std::string>().empty()) {
    respond_json(400, {{"ok", false},
                       {"error", "缺少字段：gid/title/content（非空）"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::string title = j["title"].get<std::string>();
  const std::string content = j["content"].get<std::string>();
  // 新建/编辑两分支共此一门（门在分支外，防新建绕行）
  if (!capability_gate(gid, "memo", "群备忘录")) return;
  if (j.contains("id") && !j["id"].is_null()) {
    if (!j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "id 须为整数"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.group_memo_by_id(id);
    if (!row.has_value() || row->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
      return;
    }
    if (!group_memo_write_allowed(account, gid,
                                  group_memo_resource(gid, id))) {
      respond_json(403, {{"ok", false},
                         {"error", "无权编辑（管理员维护中；可请管理员开放编辑）"}});
      return;
    }
    if (!impl_.store.update_group_memo(id, title, content, account,
                                       now_ms())) {
      respond_json(500, {{"ok", false}, {"error", "备忘录更新失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-memo update account=" << account
              << " gid=" << gid << " id=" << id << std::endl;
    mailbox_note_memo_change(gid, id, title, content, account, false);
    respond_json(200, {{"ok", true}, {"id", id}});
    return;
  }
  if (!group_memo_write_allowed(account, gid, group_resource(gid))) {
    respond_json(403, {{"ok", false},
                       {"error", "无权新建（管理员维护中；可请管理员开放编辑）"}});
    return;
  }
  const std::int64_t id = impl_.store.create_group_memo(
      gid, title, content, account, now_ms());
  if (id <= 0) {
    respond_json(500, {{"ok", false}, {"error", "备忘录落库失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-memo create account=" << account
            << " gid=" << gid << " id=" << id << std::endl;
  mailbox_note_memo_change(gid, id, title, content, account, true);
  respond_json(200, {{"ok", true}, {"id", id}});
}
// 邮箱·通知入箱（mailbox.md §I：共享变更归通知类——备忘录新建/编辑
// 通知群成员（不含编辑者，镜像群消息扇出语义）；正文在群知识共享，
// 邮箱只承载状态。dedup=群+条目+内容摘要：同内容重复保存不重复入箱）
void FileConn::mailbox_note_memo_change(std::uint64_t gid, std::int64_t memo_id,
                              const std::string& title,
                              const std::string& content,
                              const std::string& editor, bool created) {
  const auto info = impl_.store.group_info(gid);
  if (!info.has_value()) return;
  std::vector<std::string> recipients;
  for (const auto& m : impl_.store.group_members(gid)) {
    if (m != editor) recipients.push_back(m);
  }
  (void)impl_.store.mailbox_add(
      "notice", "群「" + info->name + "」备忘录「" + title + "」" +
                    (created ? "已创建" : "有变更"),
      "共享变更 · 正文在群知识共享，邮箱只承载状态", "群知识共享",
      "group:" + std::to_string(gid),
      "memo:" + std::to_string(gid) + ":" + std::to_string(memo_id) + ":" +
          sha256_hex(content),
      std::move(recipients), now_ms());
}
void FileConn::route_group_memo_delete(const std::string& body) {
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
      !j["gid"].is_number_integer() || !j.contains("id") ||
      !j["id"].is_number_integer()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/id"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::int64_t id = j["id"].get<std::int64_t>();
  const auto row = impl_.store.group_memo_by_id(id);
  if (!row.has_value() || row->group_id != gid) {
    respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
    return;
  }
  // 删除恒归管理员（开放编辑开放的是写，不是删——防误删共享知识）
  if (!capability_gate(gid, "memo", "群备忘录")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:delete", group_memo_resource(gid, id),
       "owner=" + account});
  if (!d.allowed) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权删除（" + d.reason + "）"}});
    return;
  }
  if (!impl_.store.delete_group_memo(id, gid)) {
    respond_json(500, {{"ok", false}, {"error", "备忘录删除失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-memo delete account=" << account
            << " gid=" << gid << " id=" << id << std::endl;
  respond_json(200, {{"ok", true}});
}
void FileConn::route_group_memo_history() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::string id_s = query_param(query_, "id");
  if (id_s.empty() ||
      id_s.find_first_not_of("0123456789") != std::string::npos) {
    respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
    return;
  }
  const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
  const auto row = impl_.store.group_memo_by_id(id);
  if (!row.has_value()) {
    respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
    return;
  }
  if (!capability_gate(row->group_id, "memo", "群备忘录")) return;
  const Decision d = impl_.az.authorize(
      {account, "file:read", group_memo_resource(row->group_id, id),
       "owner=" + account});
  if (!d.allowed) {
    respond_json(403,
                 {{"ok", false}, {"error", "无权查看（" + d.reason + "）"}});
    return;
  }
  json arr = json::array();
  for (const auto& r : impl_.store.group_memo_history(id)) {
    arr.push_back({{"id", r.id},
                   {"title", r.title},
                   {"content", r.content},
                   {"editor", r.editor},
                   {"ts_ms", r.ts_ms}});
  }
  respond_json(200, {{"ok", true}, {"id", id}, {"revisions", arr}});
}
void FileConn::route_group_memo_rollback(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("id") ||
      !j["id"].is_number_integer() || !j.contains("revision_id") ||
      !j["revision_id"].is_number_integer()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id/revision_id"}});
    return;
  }
  const std::int64_t id = j["id"].get<std::int64_t>();
  const std::int64_t rev_id = j["revision_id"].get<std::int64_t>();
  const auto row = impl_.store.group_memo_by_id(id);
  if (!row.has_value()) {
    respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
    return;
  }
  if (!capability_gate(row->group_id, "memo", "群备忘录")) return;
  if (!group_memo_write_allowed(account, row->group_id,
                                group_memo_resource(row->group_id, id))) {
    respond_json(403, {{"ok", false},
                       {"error", "无权回滚（管理员维护中；可请管理员开放编辑）"}});
    return;
  }
  const auto hist = impl_.store.group_memo_history(id);
  const auto it = std::find_if(
      hist.begin(), hist.end(),
      [rev_id](const ServerStore::GroupMemoRevision& r) {
        return r.id == rev_id;
      });
  if (it == hist.end()) {
    respond_json(404, {{"ok", false}, {"error", "修订笔不存在"}});
    return;
  }
  // 回滚即一次编辑：取目标笔全文写回并落新笔（editor=回滚者）——留痕链不断
  if (!impl_.store.update_group_memo(id, it->title, it->content, account,
                                     now_ms())) {
    respond_json(500, {{"ok", false}, {"error", "回滚失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-memo rollback account=" << account
            << " gid=" << row->group_id << " id=" << id
            << " to_revision=" << rev_id << std::endl;
  respond_json(200, {{"ok", true}, {"id", id}});
}
void FileConn::route_group_memo_open_edit(const std::string& body) {
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
      !j["gid"].is_number_integer() || !j.contains("open") ||
      !j["open"].is_boolean()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/open（布尔）"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  if (!capability_gate(gid, "memo", "群备忘录")) return;
  const Decision d = impl_.az.authorize({account, "memo:config",
                                         group_resource(gid),
                                         "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权设置（仅群主/管理员）"}});
    return;
  }
  if (!impl_.store.set_group_memo_open_edit(gid, j["open"].get<bool>())) {
    respond_json(500, {{"ok", false}, {"error", "设置失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-memo open-edit account=" << account
            << " gid=" << gid
            << " open=" << (j["open"].get<bool>() ? 1 : 0) << std::endl;
  respond_json(200, {{"ok", true},
                     {"open_edit", j["open"].get<bool>()}});
}

} // namespace memex::server
