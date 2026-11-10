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

// —— 模型网关：端点登记＋调用审计 ——

std::int64_t ServerStore::model_endpoint_add(const ModelEndpointRow& ep) {
  if (ep.name.empty() || ep.name.find(':') != std::string::npos ||
      ep.base_url.empty()) {
    return 0;
  }
  const char* sql =
      "INSERT INTO model_endpoints(name, base_url, api_key, model, is_local,"
      " enabled, created_by, created_ms) VALUES(?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, ep.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, ep.base_url.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, ep.api_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, ep.model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, ep.is_local ? 1 : 0);
  sqlite3_bind_int(st, 6, ep.enabled ? 1 : 0);
  sqlite3_bind_text(st, 7, ep.created_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, ep.created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

ModelEndpointRow model_endpoint_row_read(sqlite3_stmt* st) {
  ModelEndpointRow r;
  r.id = sqlite3_column_int64(st, 0);
  r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
  r.base_url = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
    r.api_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
  }
  if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
  }
  r.is_local = sqlite3_column_int(st, 5) != 0;
  r.enabled = sqlite3_column_int(st, 6) != 0;
  r.created_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
  r.created_ms = sqlite3_column_int64(st, 8);
  return r;
}

// 列清单（与 model_endpoint_row_read 读序对齐；两处相邻防漂移）
constexpr const char* kModelEndpointCols =
    "id, name, base_url, api_key, model, is_local, enabled, created_by,"
    " created_ms";

std::optional<ModelEndpointRow> ServerStore::model_endpoint_by_name(
    const std::string& name) {
  if (name.empty()) return std::nullopt;
  const std::string sql =
      std::string("SELECT ") + kModelEndpointCols +
      " FROM model_endpoints WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<ModelEndpointRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) out = model_endpoint_row_read(st);
  sqlite3_finalize(st);
  return out;
}

std::vector<ModelEndpointRow> ServerStore::model_endpoints_list() {
  std::vector<ModelEndpointRow> out;
  const std::string sql = std::string("SELECT ") + kModelEndpointCols +
                          " FROM model_endpoints ORDER BY id;"; // 注册序
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  while (sqlite3_step(st) == SQLITE_ROW) out.push_back(model_endpoint_row_read(st));
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::model_endpoint_remove(const std::string& name) {
  const char* sql = "DELETE FROM model_endpoints WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::model_endpoint_set_enabled(const std::string& name,
                                             bool enabled) {
  const char* sql = "UPDATE model_endpoints SET enabled=? WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, enabled ? 1 : 0);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::model_endpoint_set_local(const std::string& name,
                                           bool is_local) {
  const char* sql = "UPDATE model_endpoints SET is_local=? WHERE name=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, is_local ? 1 : 0);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::model_call_add(const ModelCallRow& call) {
  const char* sql =
      "INSERT INTO model_calls(caller, endpoint, model, archive_scope,"
      " prompt_chars, completion_chars, status, latency_ms, created_ms)"
      " VALUES(?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, call.caller.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, call.endpoint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, call.model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, call.archive_scope ? 1 : 0);
  sqlite3_bind_int(st, 5, call.prompt_chars);
  sqlite3_bind_int(st, 6, call.completion_chars);
  sqlite3_bind_int(st, 7, call.status);
  sqlite3_bind_int64(st, 8, call.latency_ms);
  sqlite3_bind_int64(st, 9, call.created_ms);
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  return id;
}

std::vector<ModelCallRow> ServerStore::model_calls_list(int limit) {
  std::vector<ModelCallRow> out;
  const char* sql =
      "SELECT id, caller, endpoint, model, archive_scope, prompt_chars,"
      " completion_chars, status, latency_ms, created_ms"
      " FROM model_calls ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit < 1 ? 50 : (limit > 1000 ? 1000 : limit));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ModelCallRow r;
    r.id = sqlite3_column_int64(st, 0);
    r.caller = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.endpoint = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.model = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.archive_scope = sqlite3_column_int(st, 4) != 0;
    r.prompt_chars = sqlite3_column_int(st, 5);
    r.completion_chars = sqlite3_column_int(st, 6);
    r.status = sqlite3_column_int(st, 7);
    r.latency_ms = sqlite3_column_int64(st, 8);
    r.created_ms = sqlite3_column_int64(st, 9);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
