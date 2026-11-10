#include "store.hpp"

#include "cred.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <chrono>
#include <algorithm>
#include <map>
#include <set>

#include "store/common.hpp"

namespace memex::server {

// —— T4.1 群聊 ——

std::uint64_t ServerStore::create_group(
    const std::string& name, const std::string& owner,
    const std::vector<std::string>& members, const std::string& topic,
    const std::string& purpose) {
  if (name.empty() || owner.empty()) return 0;
  // 成员账号存在性校验（建群者一并校验）；去重
  std::vector<std::string> uniq;
  for (const auto& a : members) {
    if (std::find(uniq.begin(), uniq.end(), a) == uniq.end()) uniq.push_back(a);
  }
  for (const auto& a : uniq) {
    if (!find_account(a).has_value()) return 0;
  }
  if (std::find(uniq.begin(), uniq.end(), owner) == uniq.end()) {
    uniq.push_back(owner);
  }

  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "INSERT INTO groups(name, owner, announcement, topic, purpose,"
      " created_ms) VALUES(?, ?, '', ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, topic.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, purpose.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, now_ms());
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_finalize(st);
  const std::uint64_t gid =
      static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_));
  for (const auto& a : uniq) {
    sqlite3_stmt* ms = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT OR IGNORE INTO group_members(group_id,"
                           " account, joined_ms) VALUES(?, ?, ?);",
                           -1, &ms, nullptr) != SQLITE_OK) {
      continue;
    }
    sqlite3_bind_int64(ms, 1, static_cast<sqlite3_int64>(gid));
    sqlite3_bind_text(ms, 2, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ms, 3, now_ms());
    sqlite3_step(ms);
    sqlite3_finalize(ms);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return gid;
}

std::vector<std::string> ServerStore::group_members(std::uint64_t group_id) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT account FROM group_members WHERE group_id = ?"
      " ORDER BY joined_ms ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (p) out.emplace_back(p);
  }
  sqlite3_finalize(st);
  return out;
}

// 群成员明细（群组-2 导出「参与人」段）：account＋role＋joined_ms。
std::vector<GroupMemberRow> ServerStore::group_members_detail(
    std::uint64_t group_id) {
  std::vector<GroupMemberRow> out;
  const char* sql =
      "SELECT account, role, joined_ms FROM group_members WHERE group_id = ?"
      " ORDER BY joined_ms ASC, account ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupMemberRow row;
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const char* r = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    if (a) row.account = a;
    if (r) row.role = r;
    row.joined_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return out;
}

// —— 平台-12 群能力开关（权限模型「群能力管理员配全」）——

const std::vector<std::string>& ServerStore::group_capability_names() {
  static const std::vector<std::string> names = {
      "notice",       // 群公告
      "memo",         // 群备忘录
      "vault",        // 群密码箱
      "tools",        // 群工具（R25：打包/CI）
      "server_tools", // 群服务器工具（R26）
      "files",        // 群文件
      "uplink",       // 外网上传收件箱
  };
  return names;
}

bool ServerStore::group_capability_set(std::uint64_t gid,
                                       const std::string& capability,
                                       bool enabled, const std::string& by,
                                       std::int64_t ts_ms) {
  if (!group_info(gid).has_value()) return false;
  bool known = false;
  for (const auto& n : group_capability_names())
    if (n == capability) {
      known = true;
      break;
    }
  if (!known) return false;
  const char* sql =
      "INSERT INTO group_capabilities(gid, capability, enabled, updated_by,"
      " updated_ms) VALUES(?,?,?,?,?) ON CONFLICT(gid, capability) DO UPDATE"
      " SET enabled = excluded.enabled, updated_by = excluded.updated_by,"
      " updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(gid));
  sqlite3_bind_text(st, 2, capability.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, enabled ? 1 : 0);
  sqlite3_bind_text(st, 4, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (ok) {
    // 权限模型「全程留痕」：能力开关变更落查阅台账（谁/何时/改了什么）
    AuditReadRow rec;
    rec.op_account = by;
    rec.action = "group.capability";
    rec.filters = "gid=" + std::to_string(gid) + " capability=" + capability +
                  " enabled=" + (enabled ? "on" : "off");
    rec.result_count = 0;
    rec.ts_ms = ts_ms;
    add_audit_read(rec);
  }
  return ok;
}

bool ServerStore::group_capability_enabled(std::uint64_t gid,
                                           const std::string& capability) {
  bool known = false;
  for (const auto& n : group_capability_names())
    if (n == capability) {
      known = true;
      break;
    }
  if (!known) return false; // 非法能力名不因为「没配置」而放行
  const char* sql =
      "SELECT enabled FROM group_capabilities WHERE gid = ? AND"
      " capability = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return true;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(gid));
  sqlite3_bind_text(st, 2, capability.c_str(), -1, SQLITE_TRANSIENT);
  bool enabled = true; // 未配置=现行口径（允许）
  if (sqlite3_step(st) == SQLITE_ROW) enabled = sqlite3_column_int(st, 0) != 0;
  sqlite3_finalize(st);
  return enabled;
}

std::vector<ServerStore::GroupCapability> ServerStore::group_capabilities_list(
    std::uint64_t gid) {
  std::vector<GroupCapability> out;
  const char* sql =
      "SELECT gid, capability, enabled, updated_by, updated_ms"
      " FROM group_capabilities WHERE gid = ? ORDER BY capability ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(gid));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupCapability c;
    c.gid = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    c.capability = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    c.enabled = sqlite3_column_int(st, 2) != 0;
    c.updated_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(c));
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<GroupInfo> ServerStore::group_info(std::uint64_t group_id) {
  const char* sql =
      "SELECT group_id, name, owner, announcement, topic, purpose, created_ms"
      " FROM groups WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  std::optional<GroupInfo> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupInfo g;
    g.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    g.name = text_of(1);
    g.owner = text_of(2);
    g.announcement = text_of(3);
    g.topic = text_of(4);
    g.purpose = text_of(5);
    g.created_ms = sqlite3_column_int64(st, 6);
    g.members = group_members(group_id);
    out = std::move(g);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<GroupInfo> ServerStore::groups_list() {
  std::vector<GroupInfo> out;
  const char* sql =
      "SELECT group_id, name, owner, announcement, topic, purpose, created_ms"
      " FROM groups ORDER BY group_id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupInfo g;
    g.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    g.name = text_of(1);
    g.owner = text_of(2);
    g.announcement = text_of(3);
    g.topic = text_of(4);
    g.purpose = text_of(5);
    g.created_ms = sqlite3_column_int64(st, 6);
    g.members = group_members(g.group_id); // 含群主
    out.push_back(std::move(g));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<GroupInfo> ServerStore::groups_of(const std::string& account) {
  std::vector<GroupInfo> out;
  const char* sql =
      "SELECT group_id FROM group_members WHERE account = ?"
      " ORDER BY group_id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<std::uint64_t> ids;
  while (sqlite3_step(st) == SQLITE_ROW) {
    ids.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(st, 0)));
  }
  sqlite3_finalize(st);
  for (const auto id : ids) {
    if (auto g = group_info(id)) out.push_back(std::move(*g));
  }
  return out;
}

bool ServerStore::is_group_member(std::uint64_t group_id,
                                  const std::string& account) {
  for (const auto& a : group_members(group_id)) {
    if (a == account) return true;
  }
  return false;
}

bool ServerStore::group_invite(std::uint64_t group_id,
                               const std::string& account) {
  if (!group_info(group_id).has_value()) return false;
  if (!find_account(account).has_value()) return false;
  if (is_group_member(group_id, account)) return false;
  const char* sql =
      "INSERT INTO group_members(group_id, account, joined_ms)"
      " VALUES(?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::group_leave(std::uint64_t group_id,
                              const std::string& account) {
  const auto info = group_info(group_id);
  if (!info.has_value() || !is_group_member(group_id, account)) return false;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "DELETE FROM group_members WHERE group_id = ?"
                         " AND account = ?;",
                         -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  if (info->owner == account) {
    // 群主退群＝解散：清完整张成员表（群号与历史归档保留，留痕纪律）
    if (sqlite3_prepare_v2(db_,
                           "DELETE FROM group_members WHERE group_id = ?;",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool ServerStore::group_announce(std::uint64_t group_id,
                                 const std::string& account,
                                 const std::string& announcement) {
  const auto info = group_info(group_id);
  if (!info.has_value()) return false;
  // R24-1 权限放宽：群主或管理员可设（设计「可写=群主/管理员」；
  // 群主身份在 groups.owner——role 列不含群主行）。
  if (info->owner != account && group_role(group_id, account) != "admin") {
    return false;
  }
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql = "UPDATE groups SET announcement = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_text(st, 1, announcement.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  // 编辑历史留痕（只附加）：清除也落一笔（content 空串=清除）
  const char* log_sql =
      "INSERT INTO group_announcement_log(group_id, editor, content, ts_ms)"
      " VALUES(?, ?, ?, ?);";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, log_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, announcement.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

// 群资料（群组-1 主题/目的）设置：权沿群公告先例（群主/管理员）；
// 两字段整组覆盖＝留空即清除（空串=「未填」态）。不入公告编辑历史
//（设计未要求资料编辑留痕，公告历史口径不动）。
bool ServerStore::group_set_profile(std::uint64_t group_id,
                                    const std::string& account,
                                    const std::string& topic,
                                    const std::string& purpose) {
  const auto info = group_info(group_id);
  if (!info.has_value()) return false;
  if (info->owner != account && group_role(group_id, account) != "admin") {
    return false;
  }
  const char* sql =
      "UPDATE groups SET topic = ?, purpose = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, topic.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, purpose.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::AnnouncementRevision>
ServerStore::announcement_history(std::uint64_t group_id, int limit) {
  std::vector<AnnouncementRevision> out;
  const char* sql =
      "SELECT id, editor, content, ts_ms FROM group_announcement_log"
      " WHERE group_id = ? ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int(st, 2, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    AnnouncementRevision r;
    r.id = sqlite3_column_int64(st, 0);
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.editor = e ? e : "";
    r.content = c ? c : "";
    r.ts_ms = sqlite3_column_int64(st, 3);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::string ServerStore::group_role(std::uint64_t group_id,
                                    const std::string& account) {
  if (account.empty()) return "";
  const char* sql =
      "SELECT role FROM group_members WHERE group_id=? AND account=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return "";
  sqlite3_bind_int64(st, 1, static_cast<std::int64_t>(group_id));
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  std::string role;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* r = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (r) role = r;
  }
  sqlite3_finalize(st);
  return role;
}

bool ServerStore::group_set_role(std::uint64_t group_id,
                                 const std::string& account,
                                 const std::string& role) {
  if (account.empty()) return false;
  if (role != "member" && role != "admin") return false;
  // 群主行拒改（owner 身份在 groups.owner，不在本列——任免/转让另走群面）
  const char* sql =
      "UPDATE group_members SET role=? WHERE group_id=? AND account=?"
      " AND account != (SELECT owner FROM groups WHERE group_id=?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, static_cast<std::int64_t>(group_id));
  sqlite3_bind_text(st, 3, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, static_cast<std::int64_t>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}


} // namespace memex::server
