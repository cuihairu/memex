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

std::int64_t ServerStore::webhook_create(const std::string& token_hash,
                                          const std::string& target,
                                          const std::string& name,
                                          std::int64_t created_ms) {
  if (token_hash.empty() || target.empty()) return 0;
  const char* sql =
      "INSERT INTO webhooks(token_hash, target, name, created_ms)"
      " VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, target.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::optional<WebhookRow> ServerStore::webhook_by_token(
    const std::string& token_hash) {
  if (token_hash.empty()) return std::nullopt;
  const char* sql =
      "SELECT id, token_hash, target, name, created_ms, revoked"
      " FROM webhooks WHERE token_hash=? AND revoked=0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<WebhookRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    WebhookRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
      r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    }
    r.created_ms = sqlite3_column_int64(st, 4);
    r.revoked = sqlite3_column_int(st, 5) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<WebhookRow> ServerStore::webhook_list() {
  std::vector<WebhookRow> out;
  const char* sql =
      "SELECT id, token_hash, target, name, created_ms, revoked"
      " FROM webhooks ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    WebhookRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
      r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    }
    r.created_ms = sqlite3_column_int64(st, 4);
    r.revoked = sqlite3_column_int(st, 5) != 0;
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::webhook_revoke(std::int64_t id) {
  const char* sql = "UPDATE webhooks SET revoked=1 WHERE id=? AND revoked=0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— 机器人（bot）台账 ——

std::int64_t ServerStore::bot_add(const std::string& name,
                                  const std::string& token_hash,
                                  const std::string& created_by,
                                  std::int64_t created_ms) {
  if (name.empty() || name.find(':') != std::string::npos ||
      token_hash.empty()) {
    return 0;
  }
  const char* sql =
      "INSERT INTO bots(name, token_hash, created_by, created_ms)"
      " VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, created_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::optional<BotRow> ServerStore::bot_by_token(
    const std::string& token_hash) {
  if (token_hash.empty()) return std::nullopt;
  const char* sql =
      "SELECT name, token_hash, created_by, created_ms, disabled"
      " FROM bots WHERE token_hash=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<BotRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    BotRow r;
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.disabled = sqlite3_column_int(st, 4) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<BotRow> ServerStore::bot_list() {
  std::vector<BotRow> out;
  const char* sql =
      "SELECT name, token_hash, created_by, created_ms, disabled"
      " FROM bots ORDER BY created_ms, name;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    BotRow r;
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.disabled = sqlite3_column_int(st, 4) != 0;
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<BotRow> ServerStore::bot_by_name(const std::string& name) {
  if (name.empty()) return std::nullopt;
  const char* sql =
      "SELECT name, token_hash, created_by, created_ms, disabled"
      " FROM bots WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<BotRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    BotRow r;
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.token_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.disabled = sqlite3_column_int(st, 4) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::bot_remove(const std::string& name) {
  // 连带清全部群成员行（伪账号退群由本方法一并兜底）。
  const char* del_member =
      "DELETE FROM group_members WHERE account=?;";
  const char* del_bot = "DELETE FROM bots WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, del_member, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, ("bot:" + name).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  // 连带清白名单两表（无外键，手动清——同名重建不残留旧名单）。
  for (const char* sql :
       {"DELETE FROM bot_group_allow WHERE name=?;",
        "DELETE FROM bot_dept_allow WHERE name=?;"}) {
    sqlite3_stmt* d = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &d, nullptr) != SQLITE_OK) {
      return false;
    }
    sqlite3_bind_text(d, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(d);
    sqlite3_finalize(d);
  }
  if (sqlite3_prepare_v2(db_, del_bot, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::bot_set_disabled(const std::string& name, bool disabled) {
  const char* sql = "UPDATE bots SET disabled=? WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, disabled ? 1 : 0);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::bot_join_group(const std::string& name,
                                 std::uint64_t group_id,
                                 std::int64_t joined_ms) {
  if (!bot_by_name(name) || !group_info(group_id)) return false;
  // 白名单守卫：名单非空且群不在名单 → 拒（口径见 bot_allow_group 注）。
  if (!bot_group_allowed(name, group_id)) return false;
  const char* sql =
      "INSERT OR IGNORE INTO group_members(group_id, account, joined_ms)"
      " VALUES(?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, ("bot:" + name).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, joined_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return true;
}

bool ServerStore::bot_leave_group(const std::string& name,
                                  std::uint64_t group_id) {
  if (!bot_by_name(name)) return false;
  const char* sql =
      "DELETE FROM group_members WHERE group_id=? AND account=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, ("bot:" + name).c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— bot 可用范围白名单 ——（口径见 store.hpp 注）

// 名单收紧的统一扫尾：名单非空时退掉名单外的全部群（不变式＝成员资格
// ⊆ 名单）；名单空＝不设限，不动成员资格。EXISTS 守卫即「非空」判定。
// ?1=伪账号（group_members.account）、?2=bot 名（bot_group_allow.name）——
// 两列取值域不同，错绑会让 EXISTS 恒假＝一个群都裁不掉。
void ServerStore::bot_prune_group_membership(const std::string& name) {
  const char* sql =
      "DELETE FROM group_members WHERE account=?1"
      " AND EXISTS(SELECT 1 FROM bot_group_allow WHERE name=?2)"
      " AND group_id NOT IN"
      " (SELECT group_id FROM bot_group_allow WHERE name=?2);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(st, 1, ("bot:" + name).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

bool ServerStore::bot_allow_group(const std::string& name,
                                  std::uint64_t group_id,
                                  std::int64_t created_ms) {
  if (!bot_by_name(name) || !group_info(group_id)) return false;
  const char* sql =
      "INSERT OR IGNORE INTO bot_group_allow(name, group_id, created_ms)"
      " VALUES(?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int64(st, 3, created_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
  bot_prune_group_membership(name);
  return true;
}

bool ServerStore::bot_deny_group(const std::string& name,
                                 std::uint64_t group_id) {
  if (!bot_by_name(name)) return false;
  // 移出名单即退出该群（连带退群，与 bot_remove 同款先例）：名单复空时
  // 「空＝不设限」不扫尾，不显式退群 bot 会滞留群内还能收发。
  bot_leave_group(name, group_id);
  const char* sql =
      "DELETE FROM bot_group_allow WHERE name=? AND group_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  sqlite3_step(st);
  sqlite3_finalize(st);
  // 名单仍非空时把名单外其余群一并收口（复空则 no-op）
  bot_prune_group_membership(name);
  return true;
}

bool ServerStore::bot_allow_dept(const std::string& name,
                                 const std::string& department_path,
                                 std::int64_t created_ms) {
  if (!bot_by_name(name) || department_path.empty()) return false;
  // 路径存在性校验（只读走树，不隐式建部门——与 ensure_department_path
  // 的建树语义区分开）。
  int parent_id = -1;
  std::size_t start = 0;
  while (start <= department_path.size()) {
    const std::size_t slash = department_path.find('/', start);
    const std::string seg = department_path.substr(
        start, slash == std::string::npos ? std::string::npos
                                          : slash - start);
    if (seg.empty()) return false; // 连续斜杠／尾斜杠等非法路径
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id FROM departments WHERE name = ? AND"
                      " ((parent_id IS NULL AND ? = -1) OR parent_id = ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
      return false;
    sqlite3_bind_text(st, 1, seg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, parent_id);
    sqlite3_bind_int(st, 3, parent_id);
    int id = -1;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (id < 0) return false; // 该段不存在
    parent_id = id;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  const char* ins =
      "INSERT OR IGNORE INTO bot_dept_allow(name, department_path, created_ms)"
      " VALUES(?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, created_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return true;
}

bool ServerStore::bot_deny_dept(const std::string& name,
                                const std::string& department_path) {
  if (!bot_by_name(name)) return false;
  const char* sql =
      "DELETE FROM bot_dept_allow WHERE name=? AND department_path=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  sqlite3_finalize(st);
  return true;
}

std::vector<std::uint64_t> ServerStore::bot_groups_allowed(
    const std::string& name) {
  std::vector<std::uint64_t> out;
  const char* sql =
      "SELECT group_id FROM bot_group_allow WHERE name=? ORDER BY group_id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(st, 0)));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<std::string> ServerStore::bot_depts_allowed(const std::string& name) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT department_path FROM bot_dept_allow WHERE name=?"
      " ORDER BY department_path;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::bot_group_allowed(const std::string& name,
                                    std::uint64_t group_id) {
  // 名单空（或无此 bot——判定面宽松放行，401/404 由鉴权与交付层裁）→ 放行
  const char* sql =
      "SELECT NOT EXISTS(SELECT 1 FROM bot_group_allow WHERE name=?)"
      " OR EXISTS(SELECT 1 FROM bot_group_allow WHERE name=? AND group_id=?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return true;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_ROW &&
                  sqlite3_column_int(st, 0) != 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::bot_account_allowed(const std::string& name,
                                      const std::string& account) {
  // 名单空 → 放行；账号不存在 → 放行（白名单不抢 404 语义，交
  // deliver_notice）；未分配部门从严（跨部门文件同款先例）；名单部门为
  // 目标部门的祖先或自身（段边界前缀，防「研发」误吞「研发部」）才放行。
  const char* sql = "SELECT COUNT(*) FROM bot_dept_allow WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return true;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  bool any = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0;
  sqlite3_finalize(st);
  if (!any) return true;
  if (!find_account(account)) return true; // 幽灵账号不在白名单管辖
  const auto profile = member_profile(account);
  const std::string path =
      profile ? profile->department_path : std::string{};
  if (path.empty()) return false; // 未分配部门从严
  for (const auto& allow : bot_depts_allowed(name)) {
    if (path == allow || path.rfind(allow + "/", 0) == 0) return true;
  }
  return false;
}


} // namespace memex::server
