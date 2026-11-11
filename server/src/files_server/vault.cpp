// 群密码箱（LAY-003 拆分）：R24-3 解锁门、初始化/重签/授权/存取
// 与 ACL、审计。

#include "files_server/conn.hpp"

namespace memex::server {

// —— R24-3 群密码箱 ——
// 解锁/读取判定（route 叠加，不自造 az 规则）：群成员（file:read 群
// 继承）且（owner/admin 恒可 ∨ 授权名单空=全成员 ∨ 在名单）
bool FileConn::vault_unlock_allowed(const std::string& account, std::uint64_t gid) {
  if (!impl_.az
           .authorize({account, "file:read", group_resource(gid),
                       "owner=" + account})
           .allowed) {
    return false; // 非群成员（退群即失）
  }
  if (impl_.az
          .authorize({account, "memo:write", group_resource(gid),
                      "owner=" + account})
          .allowed) {
    return true; // owner/admin 恒可（维护权即读取权）
  }
  const auto acl = impl_.store.vault_acl_list(gid);
  if (acl.empty()) return true; // 默认全成员（共享的本意）
  return std::find(acl.begin(), acl.end(), account) != acl.end();
}
// init/rekey 公共字段校验：gid 整数、盐/包裹块非空串、迭代数 ≥10000
bool FileConn::vault_wrap_fields(const json& j, std::uint64_t* gid,
                              std::string* salt, int* iters,
                              std::string* wrapped) {
  if (!j.is_object() || !j.contains("gid") ||
      !j["gid"].is_number_integer() || !j.contains("kdf_salt") ||
      !j["kdf_salt"].is_string() || !j.contains("kdf_iters") ||
      !j["kdf_iters"].is_number_integer() || !j.contains("wrapped_dek") ||
      !j["wrapped_dek"].is_string()) {
    return false;
  }
  *salt = j["kdf_salt"].get<std::string>();
  *wrapped = j["wrapped_dek"].get<std::string>();
  *iters = j["kdf_iters"].get<int>();
  if (salt->empty() || wrapped->empty() || *iters < 10000) return false;
  *gid = static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  return *gid > 0;
}
void FileConn::route_group_vault_info() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::uint64_t gid = 0;
  if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
    respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
    return;
  }
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  if (!vault_unlock_allowed(account, gid)) {
    respond_json(403, {{"ok", false},
                       {"error", "无权查看（非群成员或不在授权名单）"}});
    return;
  }
  const auto v = impl_.store.group_vault_info(gid);
  if (!v.has_value()) {
    respond_json(200, {{"ok", true}, {"exists", false}});
    return;
  }
  json acl = json::array();
  for (const auto& a : impl_.store.vault_acl_list(gid)) acl.push_back(a);
  respond_json(200, {{"ok", true},
                     {"exists", true},
                     {"gid", gid},
                     {"kdf_salt", v->kdf_salt},
                     {"kdf_iters", v->kdf_iters},
                     {"wrapped_dek", v->wrapped_dek},
                     {"acl", acl}});
}
void FileConn::route_group_vault_init(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  std::uint64_t gid = 0;
  std::string salt, wrapped;
  int iters = 0;
  if (!vault_wrap_fields(j, &gid, &salt, &iters, &wrapped)) {
    respond_json(400, {{"ok", false},
                       {"error",
                        "缺少字段：gid/kdf_salt/kdf_iters(≥10000)/"
                        "wrapped_dek（非空）"}});
    return;
  }
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:write", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权建箱（仅群主/管理员）"}});
    return;
  }
  if (impl_.store.group_vault_info(gid).has_value()) {
    respond_json(409, {{"ok", false}, {"error", "该群密码箱已初始化"}});
    return;
  }
  if (!impl_.store.group_vault_init(gid, salt, iters, wrapped, now_ms())) {
    respond_json(500, {{"ok", false}, {"error", "建箱失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-vault init account=" << account
            << " gid=" << gid << " iters=" << iters << std::endl;
  respond_json(200, {{"ok", true}, {"gid", gid}});
}
void FileConn::route_group_vault_rekey(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  std::uint64_t gid = 0;
  std::string salt, wrapped;
  int iters = 0;
  if (!vault_wrap_fields(j, &gid, &salt, &iters, &wrapped)) {
    respond_json(400, {{"ok", false},
                       {"error",
                        "缺少字段：gid/kdf_salt/kdf_iters(≥10000)/"
                        "wrapped_dek（非空）"}});
    return;
  }
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:write", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权重置（仅群主/管理员）"}});
    return;
  }
  if (!impl_.store.group_vault_rekey(gid, salt, iters, wrapped, now_ms())) {
    respond_json(404, {{"ok", false}, {"error", "密码箱不存在"}});
    return;
  }
  std::cout << "[MEMEX] files group-vault rekey account=" << account
            << " gid=" << gid << " iters=" << iters << std::endl;
  respond_json(200, {{"ok", true}, {"gid", gid}});
}
void FileConn::route_group_vault_list() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::uint64_t gid = 0;
  if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
    respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
    return;
  }
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  if (!vault_unlock_allowed(account, gid)) {
    respond_json(403, {{"ok", false},
                       {"error", "无权查看（非群成员或不在授权名单）"}});
    return;
  }
  json arr = json::array();
  for (const auto& e : impl_.store.vault_list_entries(gid)) {
    arr.push_back({{"id", e.id},
                   {"name", e.name},
                   {"account_name", e.account_name},
                   {"created_by", e.created_by},
                   {"created_ms", e.created_ms},
                   {"updated_ms", e.updated_ms}});
    // 掩码面：secret_* 不进列表（密文只经 access，每访留痕）
  }
  respond_json(200, {{"ok", true}, {"gid", gid}, {"entries", arr}});
}
void FileConn::route_group_vault_access(const std::string& body) {
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
      !j["id"].is_number_integer() || !j.contains("action") ||
      !j["action"].is_string()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/id/action"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::int64_t id = j["id"].get<std::int64_t>();
  const std::string action = j["action"].get<std::string>();
  if (action != "reveal" && action != "copy") {
    respond_json(400, {{"ok", false},
                       {"error", "action 须为 reveal（查看）或 copy（复制）"}});
    return;
  }
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  if (!vault_unlock_allowed(account, gid)) {
    respond_json(403, {{"ok", false},
                       {"error", "无权查看（非群成员或不在授权名单）"}});
    return;
  }
  const auto row = impl_.store.vault_entry_by_id(id);
  if (!row.has_value() || row->group_id != gid) {
    respond_json(404, {{"ok", false}, {"error", "条目不存在"}});
    return;
  }
  // 每次访问留痕（谁/何时/哪条/何动作——设计：查看留痕、复制显式+留痕）
  impl_.store.vault_audit_add(gid, id, account, action, now_ms());
  std::cout << "[MEMEX] files group-vault " << action
            << " account=" << account << " gid=" << gid << " id=" << id
            << std::endl;
  respond_json(200,
               {{"ok", true},
                {"id", row->id},
                {"name", row->name},
                {"account_name", row->account_name},
                {"secret_ct", row->secret_ct},
                {"secret_nonce", row->secret_nonce},
                {"updated_ms", row->updated_ms}});
}
void FileConn::route_group_vault_save(const std::string& body) {
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
      !j["name"].is_string() || !j.contains("account_name") ||
      !j["account_name"].is_string() || !j.contains("secret_ct") ||
      !j["secret_ct"].is_string() || !j.contains("secret_nonce") ||
      !j["secret_nonce"].is_string() ||
      j["name"].get<std::string>().empty() ||
      j["account_name"].get<std::string>().empty() ||
      j["secret_ct"].get<std::string>().empty() ||
      j["secret_nonce"].get<std::string>().empty()) {
    respond_json(400, {{"ok", false},
                       {"error",
                        "缺少字段：gid/name/account_name/secret_ct/"
                        "secret_nonce（非空）"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  const std::string name = j["name"].get<std::string>();
  const std::string account_name = j["account_name"].get<std::string>();
  const std::string secret_ct = j["secret_ct"].get<std::string>();
  const std::string secret_nonce = j["secret_nonce"].get<std::string>();
  // 管理员维护条目（memo:write 规则命中 owner/admin；不开放成员写）
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:write", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权维护条目（仅群主/管理员）"}});
    return;
  }
  if (j.contains("id") && !j["id"].is_null()) {
    if (!j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "id 须为整数"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.vault_entry_by_id(id);
    if (!row.has_value() || row->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "条目不存在"}});
      return;
    }
    if (!impl_.store.vault_update_entry(id, name, account_name, secret_ct,
                                        secret_nonce, account, now_ms())) {
      respond_json(500, {{"ok", false}, {"error", "条目更新失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-vault update account=" << account
              << " gid=" << gid << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}, {"id", id}});
    return;
  }
  const std::int64_t id = impl_.store.vault_create_entry(
      gid, name, account_name, secret_ct, secret_nonce, account, now_ms());
  if (id <= 0) {
    respond_json(500, {{"ok", false}, {"error", "条目落库失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-vault create account=" << account
            << " gid=" << gid << " id=" << id << std::endl;
  respond_json(200, {{"ok", true}, {"id", id}});
}
void FileConn::route_group_vault_delete(const std::string& body) {
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
  const auto row = impl_.store.vault_entry_by_id(id);
  if (!row.has_value() || row->group_id != gid) {
    respond_json(404, {{"ok", false}, {"error", "条目不存在"}});
    return;
  }
  // 删条目恒归管理员（memo:delete 规则命中 owner/admin）
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:delete", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false}, {"error", "无权删除（" + d.reason + "）"}});
    return;
  }
  if (!impl_.store.vault_delete_entry(id, gid)) {
    respond_json(500, {{"ok", false}, {"error", "条目删除失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-vault delete account=" << account
            << " gid=" << gid << " id=" << id << std::endl;
  respond_json(200, {{"ok", true}});
}
void FileConn::route_group_vault_acl(const std::string& body) {
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
      !j["gid"].is_number_integer() || !j.contains("accounts") ||
      !j["accounts"].is_array()) {
    respond_json(400, {{"ok", false},
                       {"error", "缺少字段：gid/accounts（数组，空=恢复全成员）"}});
    return;
  }
  const std::uint64_t gid =
      static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
  std::vector<std::string> accounts;
  for (const auto& a : j["accounts"]) {
    if (!a.is_string()) {
      respond_json(400, {{"ok", false}, {"error", "accounts 须为字符串数组"}});
      return;
    }
    accounts.push_back(a.get<std::string>());
  }
  // 授权名单仅群主可改（vault:config 由 group-owner 命中、group-admin
  // 显式排除——收窄群主的共享授权不归管理员）
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  const Decision d = impl_.az.authorize(
      {account, "vault:config", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权设置授权名单（仅群主）"}});
    return;
  }
  if (!impl_.store.vault_set_acl(gid, accounts)) {
    respond_json(500, {{"ok", false}, {"error", "名单写入失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-vault acl account=" << account
            << " gid=" << gid << " size=" << accounts.size() << std::endl;
  respond_json(200, {{"ok", true},
                     {"gid", gid},
                     {"size", static_cast<int>(accounts.size())}});
}
void FileConn::route_group_vault_audit() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::uint64_t gid = 0;
  if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
    respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
    return;
  }
  // 审计查询=管理面（memo:config 规则命中 owner/admin）
  if (!capability_gate(gid, "vault", "群密码箱")) return;
  const Decision d = impl_.az.authorize(
      {account, "memo:config", group_resource(gid), "owner=" + account});
  if (!d.allowed) {
    respond_json(403, {{"ok", false},
                       {"error", "无权查看审计（仅群主/管理员）"}});
    return;
  }
  json arr = json::array();
  for (const auto& a : impl_.store.vault_audit_list(gid)) {
    arr.push_back({{"id", a.id},
                   {"entry_id", a.entry_id},
                   {"actor", a.actor},
                   {"action", a.action},
                   {"ts_ms", a.ts_ms}});
  }
  respond_json(200, {{"ok", true}, {"gid", gid}, {"rows", arr}});
}

} // namespace memex::server
