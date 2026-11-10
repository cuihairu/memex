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

// —— 二期·审批（请假起步）——

namespace {
ServerStore::ApprovalRow approval_row_read(sqlite3_stmt* st) {
  ServerStore::ApprovalRow t;
  t.id = sqlite3_column_int64(st, 0);
  t.applicant = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  t.type = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  t.leave_from = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
  t.leave_to = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
  t.reason = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
  t.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
  t.decider = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
  t.decision_note = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
  t.created_ms = sqlite3_column_int64(st, 9);
  t.decided_ms = sqlite3_column_int64(st, 10);
  return t;
}
const char* kApprovalCols =
    "id, applicant, type, leave_from, leave_to, reason, status, decider,"
    " decision_note, created_ms, decided_ms";
} // namespace

std::int64_t ServerStore::approval_create(const std::string& applicant,
                                          const std::string& type,
                                          const std::string& leave_from,
                                          const std::string& leave_to,
                                          const std::string& reason,
                                          std::int64_t created_ms) {
  if (applicant.empty() || type.empty()) return 0;
  if (!find_account(applicant)) return 0; // 幽灵账号不给建
  const char* sql =
      "INSERT INTO approvals(applicant, type, leave_from, leave_to, reason,"
      " status, created_ms) VALUES(?,?,?,?,?,'pending',?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, applicant.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, leave_from.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, leave_to.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, reason.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, created_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0;
}

std::vector<ServerStore::ApprovalRow> ServerStore::approvals_of(
    const std::string& applicant) {
  std::vector<ApprovalRow> out;
  const std::string sql =
      std::string("SELECT ") + kApprovalCols +
      " FROM approvals WHERE applicant = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(st, 1, applicant.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(approval_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::ApprovalRow> ServerStore::approvals_pending() {
  std::vector<ApprovalRow> out;
  const std::string sql =
      std::string("SELECT ") + kApprovalCols +
      " FROM approvals WHERE status = 'pending' ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(approval_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::optional<ServerStore::ApprovalRow> ServerStore::approval_by_id(
    std::int64_t id) {
  const std::string sql =
      std::string("SELECT ") + kApprovalCols +
      " FROM approvals WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<ApprovalRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = approval_row_read(st);
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::approval_decide(std::int64_t id, const std::string& decider,
                                  bool approved, const std::string& note,
                                  std::int64_t decided_ms) {
  if (decider.empty()) return false;
  const char* sql =
      "UPDATE approvals SET status = ?, decider = ?, decision_note = ?,"
      " decided_ms = ? WHERE id = ? AND status = 'pending';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, approved ? "approved" : "rejected", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, decider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, decided_ms);
  sqlite3_bind_int64(st, 5, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1; // 非 pending=0 行改动=拒
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::approval_withdraw(std::int64_t id,
                                    const std::string& applicant) {
  const char* sql =
      "UPDATE approvals SET status = 'withdrawn', decided_ms = ?"
      " WHERE id = ? AND applicant = ? AND status = 'pending';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, now_ms());
  sqlite3_bind_int64(st, 2, id);
  sqlite3_bind_text(st, 3, applicant.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1; // 他人/非 pending=0 行=拒
  sqlite3_finalize(st);
  return ok;
}

// —— 二期·日报周报（设计稿 §二）——

std::int64_t ServerStore::report_upsert(const std::string& author,
                                        const std::string& report_date,
                                        const std::string& content,
                                        std::int64_t ts_ms) {
  if (author.empty() || report_date.empty()) return 0;
  if (!find_account(author)) return 0; // 幽灵账号不给写
  const char* sql =
      "INSERT INTO reports(author, report_date, content, created_ms,"
      " updated_ms) VALUES(?,?,?,?,?)"
      " ON CONFLICT(author, report_date) DO UPDATE SET"
      " content = excluded.content, updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, author.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, report_date.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return 0;
  // upsert 两种路径统一按 (author, date) 回查 id（不依赖 last_insert_rowid
  // 在 DO UPDATE 路径的语义）
  const char* q = "SELECT id FROM reports WHERE author = ? AND report_date = ?;";
  st = nullptr;
  std::int64_t id = 0;
  if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, author.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, report_date.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return id;
}

std::vector<ServerStore::ReportRow> ServerStore::reports_of(
    const std::string& author) {
  std::vector<ReportRow> out;
  const char* sql =
      "SELECT id, author, report_date, content, created_ms, updated_ms"
      " FROM reports WHERE author = ? ORDER BY report_date DESC, id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, author.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    ReportRow t;
    t.id = sqlite3_column_int64(st, 0);
    t.author = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    t.report_date = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.content = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    t.created_ms = sqlite3_column_int64(st, 4);
    t.updated_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<std::string> ServerStore::direct_reports(
    const std::string& manager) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT DISTINCT account FROM org_reporting_lines"
      " WHERE manager_account = ? ORDER BY account;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, manager.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  }
  sqlite3_finalize(st);
  return out;
}

// —— 邮箱·统一收件箱（mailbox.md；条目行+按账号状态行两表，操作面
// 只动这两张表——边界②归档行一概不碰）——

std::int64_t ServerStore::mailbox_add(
    const std::string& kind, const std::string& title, const std::string& sub,
    const std::string& source, const std::string& ref,
    const std::string& dedup, const std::vector<std::string>& recipients,
    std::int64_t ts_ms) {
  if (recipients.empty()) return 0; // 无受众不入箱（如群只有设置者自己）
  // 幂等：同 dedup 已有条目即不再入。告警例外——须全部收件人已处理
  //（state=2）才允许复发再报（磁盘超阈值修复后复发的场景）。
  if (!dedup.empty()) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT i.id, (SELECT MIN(s.state) FROM"
                           " mailbox_status s WHERE s.item_id = i.id)"
                           " FROM mailbox_items i WHERE i.dedup = ?;",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, dedup.c_str(), -1, SQLITE_TRANSIENT);
      const bool exists = sqlite3_step(st) == SQLITE_ROW;
      const std::int64_t existing = exists ? sqlite3_column_int64(st, 0) : 0;
      const int min_state = exists ? sqlite3_column_int(st, 1) : -1;
      sqlite3_finalize(st);
      if (exists) {
        const bool warning_rearm = kind == "warning" && min_state >= 2;
        if (!warning_rearm) return existing; // 幂等回既有条目（不再入箱）
        // 告警复发（旧条目已全员处理）：旧条目让出幂等键，新条目再入箱
        sqlite3_stmt* u = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "UPDATE mailbox_items SET dedup = NULL"
                               " WHERE id = ?;",
                               -1, &u, nullptr) == SQLITE_OK) {
          sqlite3_bind_int64(u, 1, existing);
          sqlite3_step(u);
          sqlite3_finalize(u);
        }
      }
    }
  }
  const char* sql =
      "INSERT INTO mailbox_items(kind, title, sub, source, ref, dedup,"
      " created_ms) VALUES(?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, sub.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, source.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, ref.c_str(), -1, SQLITE_TRANSIENT);
  if (dedup.empty()) sqlite3_bind_null(st, 6);
  else sqlite3_bind_text(st, 6, dedup.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return 0;
  const std::int64_t id = sqlite3_last_insert_rowid(db_);
  // 收件人逐人建未读状态行（重复收件人去重；INSERT OR IGNORE 幂等）
  sqlite3_stmt* s2 = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT OR IGNORE INTO mailbox_status(item_id,"
                         " account, state) VALUES(?, ?, 0);",
                         -1, &s2, nullptr) != SQLITE_OK) return id;
  for (const auto& a : recipients) {
    sqlite3_reset(s2);
    sqlite3_clear_bindings(s2);
    sqlite3_bind_int64(s2, 1, id);
    sqlite3_bind_text(s2, 2, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s2);
  }
  sqlite3_finalize(s2);
  return id;
}

std::vector<ServerStore::MailboxRow> ServerStore::mailbox_of(
    const std::string& account, const std::string& kind, bool unread_only,
    const std::string& q) {
  std::vector<MailboxRow> out;
  std::string sql =
      "SELECT i.id, i.kind, i.title, i.sub, i.source, i.ref, i.created_ms,"
      " s.state, s.handled_by, s.handled_ms, s.confirmed_ms,"
      " (SELECT COUNT(*) FROM mailbox_status t WHERE t.item_id = i.id),"
      " (SELECT COUNT(*) FROM mailbox_status t WHERE t.item_id = i.id"
      "  AND t.confirmed_ms > 0)"
      " FROM mailbox_items i JOIN mailbox_status s ON s.item_id = i.id"
      " WHERE s.account = ?";
  if (!kind.empty()) sql += " AND i.kind = ?";
  if (unread_only) sql += " AND s.state = 0";
  if (!q.empty()) sql += " AND (instr(lower(i.title), lower(?)) > 0 OR"
                         " instr(lower(i.source), lower(?)) > 0)";
  sql += " ORDER BY i.id DESC LIMIT 200;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  int col = 1;
  sqlite3_bind_text(st, col++, account.c_str(), -1, SQLITE_TRANSIENT);
  if (!kind.empty())
    sqlite3_bind_text(st, col++, kind.c_str(), -1, SQLITE_TRANSIENT);
  if (!q.empty()) {
    sqlite3_bind_text(st, col++, q.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, col++, q.c_str(), -1, SQLITE_TRANSIENT);
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    MailboxRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.sub = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.source = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.ref = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    r.created_ms = sqlite3_column_int64(st, 6);
    r.state = sqlite3_column_int(st, 7);
    r.handled_by = sqlite3_column_type(st, 8) == SQLITE_TEXT
                       ? reinterpret_cast<const char*>(
                             sqlite3_column_text(st, 8))
                       : "";
    r.handled_ms = sqlite3_column_int64(st, 9);
    r.confirmed_ms = sqlite3_column_int64(st, 10);
    r.confirm_total = sqlite3_column_int(st, 11);
    r.confirm_count = sqlite3_column_int(st, 12);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

ServerStore::MailboxCounts ServerStore::mailbox_counts(
    const std::string& account) {
  MailboxCounts c;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT i.kind, COUNT(*) FROM mailbox_items i"
                         " JOIN mailbox_status s ON s.item_id = i.id"
                         " WHERE s.account = ? AND s.state = 0"
                         " GROUP BY i.kind;",
                         -1, &st, nullptr) != SQLITE_OK)
    return c;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const std::string k =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const int n = sqlite3_column_int(st, 1);
    if (k == "warning") c.warning = n;
    else if (k == "notice") c.notice = n;
    else if (k == "announce") c.announce = n;
    c.all += n;
  }
  sqlite3_finalize(st);
  return c;
}

bool ServerStore::mailbox_mark_read(std::int64_t id,
                                    const std::string& account) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "UPDATE mailbox_status SET state = 1"
                         " WHERE item_id = ? AND account = ? AND state = 0;",
                         -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  const bool changed = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return changed;
}

int ServerStore::mailbox_mark_all_read(const std::string& account) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "UPDATE mailbox_status SET state = 1"
                         " WHERE account = ? AND state = 0;",
                         -1, &st, nullptr) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(st);
  const int changed = sqlite3_changes(db_);
  sqlite3_finalize(st);
  return changed;
}

bool ServerStore::mailbox_mark_handled(std::int64_t id,
                                       const std::string& account,
                                       std::int64_t ts_ms) {
  // 仅告警条目可标记已处理（kind 闸在 UPDATE 子查询里；处理人+时间留痕）
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "UPDATE mailbox_status SET state = 2, handled_by = ?,"
                         " handled_ms = ? WHERE item_id = ? AND account = ?"
                         " AND state != 2"
                         " AND (SELECT kind FROM mailbox_items"
                         "      WHERE id = ?) = 'warning';",
                         -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_int64(st, 3, id);
  sqlite3_bind_text(st, 4, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, id);
  sqlite3_step(st);
  const bool changed = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return changed;
}

bool ServerStore::mailbox_confirm(std::int64_t id,
                                  const std::string& account,
                                  std::int64_t ts_ms) {
  // 仅公告条目可确认（kind 闸同上；确认时刻留痕=已读位同置）
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "UPDATE mailbox_status SET confirmed_ms = ?,"
                         " state = 1 WHERE item_id = ? AND account = ?"
                         " AND confirmed_ms = 0"
                         " AND (SELECT kind FROM mailbox_items"
                         "      WHERE id = ?) = 'announce';",
                         -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_int64(st, 2, id);
  sqlite3_bind_text(st, 3, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, id);
  sqlite3_step(st);
  const bool changed = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return changed;
}

std::vector<ServerStore::MailboxConfirmRow>
ServerStore::mailbox_confirmations(std::int64_t id) {
  std::vector<MailboxConfirmRow> out;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT account, confirmed_ms FROM mailbox_status"
                         " WHERE item_id = ?"
                         " ORDER BY confirmed_ms = 0, confirmed_ms DESC,"
                         " account;",
                         -1, &st, nullptr) != SQLITE_OK)
    return out;
  sqlite3_bind_int64(st, 1, id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    MailboxConfirmRow r;
    r.account =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.confirmed_ms = sqlite3_column_int64(st, 1);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
