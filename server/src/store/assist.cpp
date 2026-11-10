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

// —— 平台-11 远程协助（Security Domain 模型）——

namespace {
// 审计留痕（红线：require_audit 恒开）——每次状态迁移/策略变更必经此处
void assist_audit_log(sqlite3* db, const std::string& session_id,
                      const std::string& actor, const std::string& action,
                      const std::string& detail, std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO assist_audits(session_id, actor, action, detail, ts_ms)"
      " VALUES(?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, detail.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

bool assist_mask_valid(int m) {
  const int all = ServerStore::kAssistView | ServerStore::kAssistKeyboard |
                  ServerStore::kAssistMouse | ServerStore::kAssistClipboard |
                  ServerStore::kAssistFileTransfer;
  return m != 0 && (m & ~all) == 0;
}

// 会话行整体读出（列序同 SELECT 辅助）
ServerStore::AssistSession assist_row_read(sqlite3_stmt* st) {
  ServerStore::AssistSession s;
  s.id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  s.requester = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  s.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  s.requested_mask = sqlite3_column_int(st, 3);
  s.granted_mask = sqlite3_column_int(st, 4);
  s.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
  s.requested_ms = sqlite3_column_int64(st, 6);
  s.approved_ms = sqlite3_column_int64(st, 7);
  s.started_ms = sqlite3_column_int64(st, 8);
  s.ended_ms = sqlite3_column_int64(st, 9);
  s.end_actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 10));
  s.end_reason = reinterpret_cast<const char*>(sqlite3_column_text(st, 11));
  return s;
}

const char* kAssistSessionCols =
    "id, requester, target, requested_mask, granted_mask, status,"
    " requested_ms, approved_ms, started_ms, ended_ms, end_actor, end_reason";
} // namespace

bool ServerStore::assist_policy_set(bool allow,
                                    const std::string& department_path,
                                    const std::string& updated_by,
                                    std::int64_t ts_ms) {
  if (!department_path.empty()) {
    // 部门行须挂已存在部门（与 retention_set 同口径，不顺手建部门）
    bool found = false;
    for (const auto& [id, path] : department_list()) {
      (void)id;
      if (path == department_path) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  const char* sql =
      "INSERT INTO assist_policies(department_path, allow, updated_by,"
      " updated_ms) VALUES(?,?,?,?) ON CONFLICT(department_path) DO UPDATE"
      " SET allow = excluded.allow, updated_by = excluded.updated_by,"
      " updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, allow ? 1 : 0);
  sqlite3_bind_text(st, 3, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (ok) {
    // 策略面动作 session_id 记空（段注口径），部门路径随 detail 留痕
    assist_audit_log(db_, "", updated_by, "policy",
                     (department_path.empty() ? std::string("（全局）")
                                              : department_path) +
                         (allow ? " → 放行" : " → 禁止"),
                     ts_ms);
  }
  return ok;
}

std::vector<ServerStore::AssistPolicy> ServerStore::assist_policy_list() {
  std::vector<AssistPolicy> out;
  const char* sql =
      "SELECT rowid, department_path, allow, updated_by, updated_ms"
      " FROM assist_policies ORDER BY department_path = '' DESC,"
      " department_path ASC;"; // 全局行在前
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    AssistPolicy p;
    p.id = sqlite3_column_int64(st, 0);
    p.department_path = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    p.allow = sqlite3_column_int(st, 2) != 0;
    p.updated_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::assist_policy_resolve(const std::string& account) {
  // 生效口径：本人部门链逐级上溯（与 retention_resolve 同走法）→ 全局行
  // → 内置默认禁止（白名单口径，从严）
  std::map<std::string, bool> by_path;
  for (const auto& p : assist_policy_list())
    by_path[p.department_path] = p.allow;
  std::string path;
  if (const auto prof = member_profile(account)) path = prof->department_path;
  while (!path.empty()) {
    const auto it = by_path.find(path);
    if (it != by_path.end()) return it->second;
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) break;
    path.resize(pos);
  }
  const auto g = by_path.find("");
  if (g != by_path.end()) return g->second;
  return false; // 内置默认：禁止
}

std::string ServerStore::assist_request(const std::string& requester,
                                        const std::string& target, int mask,
                                        std::int64_t ts_ms) {
  // 发起面四闸：两账号存在、非同一人、双方部门均放行、权限集合法非零
  if (requester.empty() || target.empty() || requester == target) return "";
  if (!find_account(requester).has_value()) return "";
  if (!find_account(target).has_value()) return "";
  if (!assist_mask_valid(mask)) return "";
  if (!assist_policy_resolve(requester) || !assist_policy_resolve(target))
    return "";
  // 会话 id：ra-<sqlite PRNG hex16>（库内生成，不依赖外部熵源）
  std::string id;
  {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT lower(hex(randomblob(16)));", -1, &st,
                           nullptr) != SQLITE_OK)
      return "";
    if (sqlite3_step(st) == SQLITE_ROW)
      id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    if (id.empty()) return "";
    id.insert(0, "ra-");
  }
  const char* sql =
      "INSERT INTO assist_sessions(id, requester, target, requested_mask,"
      " granted_mask, status, requested_ms) VALUES(?,?,?,?,0,'requested',?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return "";
  sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, requester.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, target.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, mask);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return "";
  assist_audit_log(db_, id, requester, "request", "申请协助", ts_ms);
  return id;
}

std::optional<ServerStore::AssistSession> ServerStore::assist_session(
    const std::string& id) {
  const std::string sql =
      std::string("SELECT ") + kAssistSessionCols +
      " FROM assist_sessions WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return std::nullopt;
  sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<AssistSession> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = assist_row_read(st);
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::AssistSession> ServerStore::assist_sessions(
    const std::string& account) {
  std::vector<AssistSession> out;
  std::string sql = std::string("SELECT ") + kAssistSessionCols +
                    " FROM assist_sessions";
  if (!account.empty()) sql += " WHERE requester = ? OR target = ?";
  sql += " ORDER BY requested_ms DESC, id ASC LIMIT 200;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!account.empty()) {
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  }
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(assist_row_read(st));
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::assist_approve(const std::string& id, const std::string& by,
                                 int granted_mask, std::int64_t ts_ms) {
  // consent 红线：只有受控方本人可批；实批集 ⊆ 申请集且非零（可缩不可扩）
  const auto s = assist_session(id);
  if (!s || s->status != "requested" || by.empty() || by != s->target)
    return false;
  if (!assist_mask_valid(granted_mask) ||
      (granted_mask & ~s->requested_mask) != 0)
    return false;
  const char* sql =
      "UPDATE assist_sessions SET granted_mask = ?, status = 'approved',"
      " approved_ms = ? WHERE id = ? AND status = 'requested';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, granted_mask);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_text(st, 3, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok)
    assist_audit_log(db_, id, by, "approve", "批准协助（授出权限）", ts_ms);
  return ok;
}

bool ServerStore::assist_deny(const std::string& id, const std::string& by,
                              std::int64_t ts_ms) {
  // consent 红线：拒绝同样只属受控方本人；requested→denied 终态
  const auto s = assist_session(id);
  if (!s || s->status != "requested" || by.empty() || by != s->target)
    return false;
  const char* sql =
      "UPDATE assist_sessions SET status = 'denied', ended_ms = ?,"
      " end_actor = ?, end_reason = 'denied'"
      " WHERE id = ? AND status = 'requested';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok) assist_audit_log(db_, id, by, "deny", "拒绝协助", ts_ms);
  return ok;
}

bool ServerStore::assist_start(const std::string& id, const std::string& by,
                               std::int64_t ts_ms) {
  const auto s = assist_session(id);
  if (!s || s->status != "approved" ||
      (by != s->requester && by != s->target))
    return false;
  const char* sql =
      "UPDATE assist_sessions SET status = 'active', started_ms = ?"
      " WHERE id = ? AND status = 'approved';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok) assist_audit_log(db_, id, by, "start", "开始协助", ts_ms);
  return ok;
}

bool ServerStore::assist_end(const std::string& id, const std::string& by,
                             const std::string& reason, std::int64_t ts_ms) {
  // active（协助中受控方=撤权即时生效）或 approved（批了没用上）都可结；
  // 终态 closed/denied 再动拒
  const auto s = assist_session(id);
  if (!s || (s->status != "active" && s->status != "approved") ||
      by.empty() || (by != s->requester && by != s->target))
    return false;
  const char* sql =
      "UPDATE assist_sessions SET status = 'closed', ended_ms = ?,"
      " end_actor = ?, end_reason = ?"
      " WHERE id = ? AND status IN ('active','approved');";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, reason.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  if (ok)
    assist_audit_log(db_, id, by,
                     by == s->target ? "end(revoke)" : "end",
                     reason, ts_ms);
  return ok;
}

std::vector<ServerStore::AssistAuditRow> ServerStore::assist_audits(
    const std::string& session_id) {
  std::vector<AssistAuditRow> out;
  std::string sql =
      "SELECT id, session_id, actor, action, detail, ts_ms"
      " FROM assist_audits";
  if (!session_id.empty()) sql += " WHERE session_id = ?";
  sql += " ORDER BY id ASC LIMIT 500;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!session_id.empty())
    sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    AssistAuditRow a;
    a.id = sqlite3_column_int64(st, 0);
    a.session_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    a.actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    a.action = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    a.detail = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    a.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
