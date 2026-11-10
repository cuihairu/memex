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

// —— R27-5 服务端任务源集成 ——

bool ServerStore::task_pull_config_set(const TaskPullConfig& c) {
  const char* sql =
      "INSERT INTO task_pull_configs(owner, provider, repo, enabled,"
      " interval_s, last_pull_ms, updated_ms) VALUES(?,?,?,?,?,?,?)"
      " ON CONFLICT(owner, provider) DO UPDATE SET"
      " repo=excluded.repo, enabled=excluded.enabled,"
      " interval_s=excluded.interval_s, updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, c.owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, c.provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, c.repo.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, c.enabled ? 1 : 0);
  sqlite3_bind_int(st, 5, c.interval_s);
  sqlite3_bind_int64(st, 6, c.last_pull_ms);
  sqlite3_bind_int64(st, 7, c.updated_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::TaskPullConfig> ServerStore::task_pull_configs() {
  std::vector<TaskPullConfig> out;
  const char* sql =
      "SELECT owner, provider, repo, enabled, interval_s, last_pull_ms,"
      " updated_ms FROM task_pull_configs ORDER BY owner, provider;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    TaskPullConfig c;
    const char* o = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* r = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    c.owner = o ? o : "";
    c.provider = p ? p : "";
    c.repo = r ? r : "";
    c.enabled = sqlite3_column_int(st, 3) != 0;
    c.interval_s = sqlite3_column_int(st, 4);
    c.last_pull_ms = sqlite3_column_int64(st, 5);
    c.updated_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(c));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::task_pull_config_delete(const std::string& owner,
                                          const std::string& provider) {
  const char* sql =
      "DELETE FROM task_pull_configs WHERE owner = ? AND provider = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_pull_config_mark_pulled(const std::string& owner,
                                               const std::string& provider,
                                               std::int64_t ts_ms) {
  const char* sql =
      "UPDATE task_pull_configs SET last_pull_ms = ?"
      " WHERE owner = ? AND provider = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, provider.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_source_cred_set(const std::string& owner,
                                       const std::string& provider,
                                       const std::string& sealed_hex,
                                       std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO task_source_credentials(owner, provider, sealed_hex,"
      " updated_ms) VALUES(?,?,?,?)"
      " ON CONFLICT(owner, provider) DO UPDATE SET"
      " sealed_hex=excluded.sealed_hex, updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, sealed_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::task_source_cred_delete(const std::string& owner,
                                          const std::string& provider) {
  const char* sql =
      "DELETE FROM task_source_credentials WHERE owner = ? AND provider = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::optional<std::string> ServerStore::task_source_cred_sealed(
    const std::string& owner, const std::string& provider) {
  const char* sql =
      "SELECT sealed_hex FROM task_source_credentials"
      " WHERE owner = ? AND provider = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<std::string> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out = s ? s : "";
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<std::string> ServerStore::server_flag_get(
    const std::string& key) {
  const char* sql = "SELECT value FROM server_flags WHERE key = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<std::string> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out = s ? s : "";
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::server_flag_set(const std::string& key,
                                  const std::string& value,
                                  std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO server_flags(key, value, updated_ms) VALUES(?,?,?)"
      " ON CONFLICT(key) DO UPDATE SET value=excluded.value,"
      " updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

ServerStore::ExternalTaskUpsert ServerStore::task_upsert_external(
    const std::string& owner, const std::string& title,
    const std::string& note, const std::string& provider,
    const std::string& ext_key, std::int64_t created_ms) {
  ExternalTaskUpsert r;
  // 定位既有行（provider/ext_key 成对必带——调用方保证非空）
  const char* find_sql =
      "SELECT id, title, note FROM tasks"
      " WHERE owner = ? AND provider = ? AND ext_key = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, find_sql, -1, &st, nullptr) != SQLITE_OK) return r;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, ext_key.c_str(), -1, SQLITE_TRANSIENT);
  std::int64_t existing_id = 0;
  std::string old_title, old_note;
  if (sqlite3_step(st) == SQLITE_ROW) {
    existing_id = sqlite3_column_int64(st, 0);
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    old_title = t ? t : "";
    old_note = n ? n : "";
  }
  sqlite3_finalize(st);
  if (existing_id > 0) {
    r.id = existing_id;
    if (old_title == title && old_note == note) return r; // 无变化不动
    const char* upd =
        "UPDATE tasks SET title = ?, note = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(db_, upd, -1, &st, nullptr) != SQLITE_OK) return r;
    sqlite3_bind_text(st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, existing_id);
    r.changed = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return r;
  }
  const char* ins =
      "INSERT INTO tasks(owner, creator, title, note, due_ms, created_ms,"
      " provider, ext_key) VALUES(?,?,?,?,0,?,?,?);";
  if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) return r;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, owner.c_str(), -1, SQLITE_TRANSIENT); // 自建语义
  sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, created_ms);
  sqlite3_bind_text(st, 6, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, ext_key.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_DONE) {
    r.id = static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
    r.created = true;
    r.changed = true;
  }
  sqlite3_finalize(st);
  return r;
}


} // namespace memex::server
