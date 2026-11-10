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

// —— R27-1 个人任务清单 ——

namespace {
ServerStore::TaskRow task_row_read(sqlite3_stmt* st) {
  ServerStore::TaskRow t;
  t.id = sqlite3_column_int64(st, 0);
  t.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  t.creator = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  t.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
  t.note = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
  t.due_ms = sqlite3_column_int64(st, 5);
  t.reminded_ms = sqlite3_column_int64(st, 6);
  t.done = sqlite3_column_int(st, 7) != 0;
  t.done_ms = sqlite3_column_int64(st, 8);
  t.created_ms = sqlite3_column_int64(st, 9);
  t.provider = reinterpret_cast<const char*>(sqlite3_column_text(st, 10));
  t.ext_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 11));
  t.start_ms = sqlite3_column_int64(st, 12);
  t.type = reinterpret_cast<const char*>(sqlite3_column_text(st, 13));
  t.status = reinterpret_cast<const char*>(sqlite3_column_text(st, 14));
  t.start_reminded_ms = sqlite3_column_int64(st, 15);
  return t;
}
const char* kTaskCols =
    "id, owner, creator, title, note, due_ms, reminded_ms, done, done_ms,"
    " created_ms, provider, ext_key, start_ms, type, status,"
    " start_reminded_ms";
} // namespace

std::int64_t ServerStore::task_create(const std::string& owner,
                                      const std::string& creator,
                                      const std::string& title,
                                      const std::string& note,
                                      std::int64_t due_ms,
                                      std::int64_t created_ms,
                                      const std::string& provider,
                                      const std::string& ext_key,
                                      const std::string& type,
                                      std::int64_t start_ms) {
  if (owner.empty() || creator.empty() || title.empty()) return 0;
  // 外部任务引用成对（路由层把守口径，库层再兜一道）
  if (provider.empty() != ext_key.empty()) return 0;
  // 双方都须为已建账号（幽灵账号不给建）
  if (!find_account(owner) || !find_account(creator)) return 0;
  const char* sql =
      "INSERT INTO tasks(owner, creator, title, note, due_ms, created_ms,"
      " provider, ext_key, type, start_ms) VALUES(?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, creator.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, due_ms);
  sqlite3_bind_int64(st, 6, created_ms);
  sqlite3_bind_text(st, 7, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 8, ext_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 9, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 10, start_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0;
}

std::vector<ServerStore::TaskRow> ServerStore::tasks_of(
    const std::string& owner) {
  std::vector<TaskRow> out;
  const std::string sql =
      std::string("SELECT ") + kTaskCols + " FROM tasks WHERE owner = ?"
      " ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(task_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::TaskRow> ServerStore::tasks_assigned_by(
    const std::string& creator) {
  std::vector<TaskRow> out;
  const std::string sql =
      std::string("SELECT ") + kTaskCols + " FROM tasks WHERE creator = ?"
      " AND owner != ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(st, 1, creator.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, creator.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(task_row_read(st));
  sqlite3_finalize(st);
  return out;
}

std::optional<ServerStore::TaskRow> ServerStore::task_by_id(std::int64_t id) {
  const std::string sql =
      std::string("SELECT ") + kTaskCols + " FROM tasks WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<TaskRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = task_row_read(st);
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::task_set_done(std::int64_t id, bool done,
                                std::int64_t done_ms) {
  // R27-6 双写过渡：完成/回退同步刷 status（读取以 status 为准）
  const char* sql =
      "UPDATE tasks SET done = ?, done_ms = ?, status = ? WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int(st, 1, done ? 1 : 0);
  sqlite3_bind_int64(st, 2, done ? done_ms : 0);
  sqlite3_bind_text(st, 3, done ? "完成" : "待办", -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_set_status(std::int64_t id, const std::string& status,
                                  std::int64_t done_ms) {
  // R27-6 状态切换（待办/进行中/完成）；完成↔done 双写，其余 done=0
  if (status != "待办" && status != "进行中" && status != "完成") {
    return false;
  }
  const char* sql =
      "UPDATE tasks SET status = ?, done = ?, done_ms = ? WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  const bool done = status == "完成";
  sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, done ? 1 : 0);
  sqlite3_bind_int64(st, 3, done ? done_ms : 0);
  sqlite3_bind_int64(st, 4, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_mark_reminded(std::int64_t id,
                                     std::int64_t reminded_ms) {
  // 只落一次：已有回执不回退（多端各提醒一次，时刻取最早）
  const char* sql =
      "UPDATE tasks SET reminded_ms = ? WHERE id = ? AND"
      " (reminded_ms = 0 OR reminded_ms > ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, reminded_ms);
  sqlite3_bind_int64(st, 2, id);
  sqlite3_bind_int64(st, 3, reminded_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_mark_start_reminded(std::int64_t id,
                                           std::int64_t reminded_ms) {
  // R27-6 开始到点提醒回执（口径同 deadline 回执，独立位互不抢占）
  const char* sql =
      "UPDATE tasks SET start_reminded_ms = ? WHERE id = ? AND"
      " (start_reminded_ms = 0 OR start_reminded_ms > ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, reminded_ms);
  sqlite3_bind_int64(st, 2, id);
  sqlite3_bind_int64(st, 3, reminded_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

// —— R27-6 任务类型清单（服务端配置） ——

std::vector<ServerStore::TaskType> ServerStore::task_types() {
  std::vector<TaskType> out;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT name, is_default FROM task_types"
                         " ORDER BY is_default DESC, name;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    TaskType t;
    t.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    t.is_default = sqlite3_column_int(st, 1) != 0;
    out.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  return out;
}

std::string ServerStore::task_type_default() {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT name FROM task_types WHERE is_default=1"
                         " LIMIT 1;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return "任务";
  }
  std::string out = "任务";
  if (sqlite3_step(st) == SQLITE_ROW) {
    out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::task_type_add(const std::string& name) {
  if (name.empty()) return false;
  const char* sql =
      "INSERT INTO task_types(name, is_default) VALUES(?, 0);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE; // 重名=约束失败=false
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_type_delete(const std::string& name) {
  if (name.empty()) return false;
  // 默认类型不删（建任务的兜底类型；删了新任务没落点）
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT is_default FROM task_types WHERE name=?;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  bool exists = false, is_default = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    exists = true;
    is_default = sqlite3_column_int(st, 0) != 0;
  }
  sqlite3_finalize(st);
  if (!exists || is_default) return false;
  // 有任务引用＝拒删（存量任务不悬空；删空类型才放行）
  if (sqlite3_prepare_v2(db_,
                         "SELECT 1 FROM tasks WHERE type=? LIMIT 1;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool in_use = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  if (in_use) return false;
  if (sqlite3_prepare_v2(db_, "DELETE FROM task_types WHERE name=?;", -1,
                         &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_delete(std::int64_t id) {
  const char* sql = "DELETE FROM tasks WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}


} // namespace memex::server
