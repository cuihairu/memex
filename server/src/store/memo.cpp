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

// ---- R23-3 文件助手：备忘录（本人收件箱文本面） ----
// 自备忘录不留修订史（修订史属 R24-2）；owner 过滤即权限边界，server 层再判权。

std::int64_t ServerStore::create_memo(const std::string& owner,
                                      const std::string& content,
                                      std::int64_t ts_ms) {
  if (owner.empty() || content.empty()) return 0;
  const char* sql =
      "INSERT INTO memos (owner, content, created_ms, updated_ms)"
      " VALUES (?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  sqlite3_bind_int64(st, 4, ts_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::MemoRow> ServerStore::memo_by_id(std::int64_t id) {
  const char* sql =
      "SELECT id, owner, content, created_ms, updated_ms"
      " FROM memos WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<MemoRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    MemoRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.content = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.updated_ms = sqlite3_column_int64(st, 4);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::MemoRow> ServerStore::list_memos(
    const std::string& owner, int limit, int offset) {
  std::vector<MemoRow> out;
  if (owner.empty()) return out;
  const char* sql =
      "SELECT id, owner, content, created_ms, updated_ms"
      " FROM memos WHERE owner=?"
      " ORDER BY updated_ms DESC, id DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, limit);
  sqlite3_bind_int(st, 3, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    MemoRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.content = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.created_ms = sqlite3_column_int64(st, 3);
    r.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::update_memo(std::int64_t id, const std::string& owner,
                              const std::string& content,
                              std::int64_t ts_ms) {
  if (owner.empty() || content.empty()) return false;
  // WHERE owner= 二次校验：仅本人可改自己的行。
  const char* sql =
      "UPDATE memos SET content=?, updated_ms=? WHERE id=? AND owner=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_int64(st, 3, id);
  sqlite3_bind_text(st, 4, owner.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::delete_memo(std::int64_t id, const std::string& owner) {
  if (owner.empty()) return false;
  const char* sql = "DELETE FROM memos WHERE id=? AND owner=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_text(st, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— R24-2 群备忘录 ——

std::int64_t ServerStore::create_group_memo(std::uint64_t group_id,
                                            const std::string& title,
                                            const std::string& content,
                                            const std::string& author,
                                            std::int64_t ts_ms) {
  if (group_id == 0 || title.empty() || content.empty() || author.empty()) {
    return 0;
  }
  if (!group_info(group_id).has_value()) return 0;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "INSERT INTO group_memos(group_id, title, content, author, created_ms,"
      " updated_ms) VALUES(?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, author.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_bind_int64(st, 6, ts_ms);
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_finalize(st);
  const std::int64_t id = sqlite3_last_insert_rowid(db_);
  // 首建即落首笔修订（editor=作者）——历史从此完整
  const char* rev_sql =
      "INSERT INTO group_memo_revisions(memo_id, title, content, editor,"
      " ts_ms) VALUES(?, ?, ?, ?, ?);";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, rev_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, author.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ts_ms);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return id;
}

std::optional<ServerStore::GroupMemo> ServerStore::group_memo_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, title, content, author, created_ms, updated_ms"
      " FROM group_memos WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupMemo> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupMemo m;
    m.id = sqlite3_column_int64(st, 0);
    m.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.title = t ? t : "";
    m.content = c ? c : "";
    m.author = a ? a : "";
    m.created_ms = sqlite3_column_int64(st, 5);
    m.updated_ms = sqlite3_column_int64(st, 6);
    out = std::move(m);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupMemo> ServerStore::list_group_memos(
    std::uint64_t group_id, const std::string& keyword, int limit,
    int offset) {
  std::vector<GroupMemo> out;
  std::string sql =
      "SELECT id, group_id, title, content, author, created_ms, updated_ms"
      " FROM group_memos WHERE group_id = ?";
  std::string like;
  if (!keyword.empty()) {
    for (const char c : keyword) {
      if (c == '%' || c == '_' || c == '\\') like += '\\';
      like += c;
    }
    sql += " AND (title LIKE ? ESCAPE '\\' OR content LIKE ? ESCAPE '\\')";
  }
  sql += " ORDER BY updated_ms DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  sqlite3_bind_int64(st, idx++, static_cast<sqlite3_int64>(group_id));
  if (!like.empty()) {
    const std::string pat = "%" + like + "%";
    sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(st, idx++, limit);
  sqlite3_bind_int(st, idx, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupMemo m;
    m.id = sqlite3_column_int64(st, 0);
    m.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.title = t ? t : "";
    m.content = c ? c : "";
    m.author = a ? a : "";
    m.created_ms = sqlite3_column_int64(st, 5);
    m.updated_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::update_group_memo(std::int64_t id, const std::string& title,
                                    const std::string& content,
                                    const std::string& editor,
                                    std::int64_t ts_ms) {
  if (title.empty() || content.empty() || editor.empty()) return false;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "UPDATE group_memos SET title = ?, content = ?, updated_ms = ?"
      " WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_text(st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  sqlite3_bind_int64(st, 4, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  // 修订笔与条目更新同事务——留痕不落半截
  const char* rev_sql =
      "INSERT INTO group_memo_revisions(memo_id, title, content, editor,"
      " ts_ms) VALUES(?, ?, ?, ?, ?);";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, rev_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, editor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, ts_ms);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool ServerStore::delete_group_memo(std::int64_t id, std::uint64_t group_id) {
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "DELETE FROM group_memos WHERE id = ? AND group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  if (!ok) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  // 条目删除连带修订史（不留孤儿；历史属条目的一部分）
  const char* rev_sql = "DELETE FROM group_memo_revisions WHERE memo_id = ?;";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, rev_sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

std::vector<ServerStore::GroupMemoRevision> ServerStore::group_memo_history(
    std::int64_t memo_id) {
  std::vector<GroupMemoRevision> out;
  const char* sql =
      "SELECT id, memo_id, title, content, editor, ts_ms"
      " FROM group_memo_revisions WHERE memo_id = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, memo_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupMemoRevision r;
    r.id = sqlite3_column_int64(st, 0);
    r.memo_id = sqlite3_column_int64(st, 1);
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.title = t ? t : "";
    r.content = c ? c : "";
    r.editor = e ? e : "";
    r.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::set_group_memo_open_edit(std::uint64_t group_id,
                                           bool open) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "UPDATE groups SET open_memo_edit = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, open ? 1 : 0);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::group_memo_open_edit(std::uint64_t group_id) {
  const char* sql =
      "SELECT open_memo_edit FROM groups WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  bool open = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    open = sqlite3_column_int(st, 0) != 0;
  }
  sqlite3_finalize(st);
  return open;
}



} // namespace memex::server
