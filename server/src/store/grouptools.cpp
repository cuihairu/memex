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

// —— R25-1 群工具框架 ——

bool ServerStore::tool_set_actions(std::uint64_t group_id,
                                   const std::string& tool,
                                   const std::string& actions_json,
                                   const std::string& updated_by,
                                   std::int64_t ts_ms) {
  if (group_id == 0 || tool.empty() || actions_json.empty() ||
      updated_by.empty()) {
    return false;
  }
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_tools(group_id, tool, actions_json, updated_by,"
      " updated_ms) VALUES(?, ?, ?, ?, ?)"
      " ON CONFLICT(group_id, tool) DO UPDATE SET actions_json=excluded."
      "actions_json, updated_by=excluded.updated_by, updated_ms=excluded."
      "updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, actions_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::optional<ServerStore::GroupToolConfig> ServerStore::tool_config(
    std::uint64_t group_id, const std::string& tool) {
  const char* sql =
      "SELECT group_id, tool, actions_json, updated_by, updated_ms"
      " FROM group_tools WHERE group_id = ? AND tool = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<GroupToolConfig> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupToolConfig c;
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.tool = t ? t : "";
    c.actions_json = a ? a : "";
    c.updated_by = u ? u : "";
    c.updated_ms = sqlite3_column_int64(st, 4);
    out = std::move(c);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupToolConfig> ServerStore::tool_list(
    std::uint64_t group_id) {
  std::vector<GroupToolConfig> out;
  const char* sql =
      "SELECT group_id, tool, actions_json, updated_by, updated_ms"
      " FROM group_tools WHERE group_id = ? ORDER BY tool ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupToolConfig c;
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.tool = t ? t : "";
    c.actions_json = a ? a : "";
    c.updated_by = u ? u : "";
    c.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(c));
  }
  sqlite3_finalize(st);
  return out;
}

void ServerStore::tool_audit_add(std::uint64_t group_id,
                                 const std::string& tool,
                                 const std::string& action,
                                 const std::string& actor,
                                 const std::string& params_json,
                                 const std::string& result_json,
                                 std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_tool_audit(group_id, tool, action, actor, params_json,"
      " result_json, ts_ms) VALUES (?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, params_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

std::vector<ServerStore::GroupToolAudit> ServerStore::tool_audit_list(
    std::uint64_t group_id, int limit) {
  std::vector<GroupToolAudit> out;
  const char* sql =
      "SELECT id, group_id, tool, action, actor, params_json, result_json,"
      " ts_ms FROM group_tool_audit WHERE group_id = ?"
      " ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int(st, 2, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupToolAudit a;
    a.id = sqlite3_column_int64(st, 0);
    a.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* ac = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* pj = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    a.tool = t ? t : "";
    a.action = ac ? ac : "";
    a.actor = actor ? actor : "";
    a.params_json = pj ? pj : "";
    a.result_json = rj ? rj : "";
    a.ts_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R25-2 CI/CD 工具 ——

bool ServerStore::ci_pipeline_upsert(std::uint64_t group_id,
                                     const std::string& name,
                                     const std::string& description,
                                     const std::string& updated_by,
                                     std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_ci_pipelines(group_id, name, description,"
      " updated_by, updated_ms) VALUES(?,?,?,?,?)"
      " ON CONFLICT(group_id, name) DO UPDATE SET"
      " description=excluded.description, updated_by=excluded.updated_by,"
      " updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, description.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::ci_pipeline_delete(std::uint64_t group_id,
                                     const std::string& name) {
  const char* sql =
      "DELETE FROM group_ci_pipelines WHERE group_id = ? AND name = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::CiPipeline> ServerStore::ci_pipeline_list(
    std::uint64_t group_id) {
  std::vector<CiPipeline> out;
  const char* sql =
      "SELECT group_id, name, description, updated_by, updated_ms"
      " FROM group_ci_pipelines WHERE group_id = ? ORDER BY name ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    CiPipeline p;
    p.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* d = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.name = n ? n : "";
    p.description = d ? d : "";
    p.updated_by = u ? u : "";
    p.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::ci_run_add(std::uint64_t group_id,
                                     const std::string& pipeline,
                                     const std::string& actor,
                                     const std::string& status,
                                     const std::string& params_json,
                                     const std::string& result_json,
                                     std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_ci_runs(group_id, pipeline, actor, status,"
      " params_json, result_json, ts_ms) VALUES(?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, pipeline.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, params_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::vector<ServerStore::CiRun> ServerStore::ci_run_list(
    std::uint64_t group_id, const std::string& pipeline, int limit) {
  std::vector<CiRun> out;
  const char* sql =
      "SELECT id, group_id, pipeline, actor, status, params_json,"
      " result_json, ts_ms FROM group_ci_runs WHERE group_id = ?"
      " AND (? = '' OR pipeline = ?) ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, pipeline.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, pipeline.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    CiRun r;
    r.id = sqlite3_column_int64(st, 0);
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* pj = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.pipeline = p ? p : "";
    r.actor = a ? a : "";
    r.status = s ? s : "";
    r.params_json = pj ? pj : "";
    r.result_json = rj ? rj : "";
    r.ts_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::CiRun> ServerStore::ci_status_list(
    std::uint64_t group_id) {
  std::vector<CiRun> out;
  // 每条流水线只取最近一笔（id 最大）——红绿灯面
  const char* sql =
      "SELECT id, group_id, pipeline, actor, status, params_json,"
      " result_json, ts_ms FROM group_ci_runs WHERE id IN"
      " (SELECT MAX(id) FROM group_ci_runs WHERE group_id = ?"
      "  GROUP BY pipeline) ORDER BY pipeline ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    CiRun r;
    r.id = sqlite3_column_int64(st, 0);
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* pj = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.pipeline = p ? p : "";
    r.actor = a ? a : "";
    r.status = s ? s : "";
    r.params_json = pj ? pj : "";
    r.result_json = rj ? rj : "";
    r.ts_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R25-3 打包工具 ——

bool ServerStore::pack_artifact_upsert(std::uint64_t group_id,
                                       const std::string& name,
                                       const std::string& version,
                                       const std::string& note,
                                       const std::string& created_by,
                                       std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_pack_artifacts(group_id, name, version, note,"
      " created_by, created_ms) VALUES(?,?,?,?,?,?)"
      " ON CONFLICT(group_id, name, version) DO UPDATE SET"
      " note=excluded.note, created_by=excluded.created_by,"
      " created_ms=excluded.created_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, note.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, created_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::PackArtifact> ServerStore::pack_artifact_list(
    std::uint64_t group_id) {
  std::vector<PackArtifact> out;
  const char* sql =
      "SELECT id, group_id, name, version, note, created_by, created_ms"
      " FROM group_pack_artifacts WHERE group_id = ?"
      " ORDER BY created_ms DESC, name ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    PackArtifact a;
    a.id = sqlite3_column_int64(st, 0);
    a.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* v = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* no = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    a.name = n ? n : "";
    a.version = v ? v : "";
    a.note = no ? no : "";
    a.created_by = c ? c : "";
    a.created_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::pack_artifact_delete(std::uint64_t group_id,
                                       const std::string& name,
                                       const std::string& version) {
  const char* sql =
      "DELETE FROM group_pack_artifacts WHERE group_id = ? AND name = ?"
      " AND version = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, version.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— R25-4 凭据面 ——

bool ServerStore::tool_cred_set(std::uint64_t group_id,
                                const std::string& tool,
                                const std::string& sealed_hex,
                                const std::string& updated_by,
                                std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return false;
  const char* sql =
      "INSERT INTO group_tool_credentials(group_id, tool, sealed_hex,"
      " updated_by, updated_ms) VALUES(?,?,?,?,?)"
      " ON CONFLICT(group_id, tool) DO UPDATE SET"
      " sealed_hex=excluded.sealed_hex, updated_by=excluded.updated_by,"
      " updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, sealed_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::tool_cred_delete(std::uint64_t group_id,
                                   const std::string& tool) {
  const char* sql =
      "DELETE FROM group_tool_credentials WHERE group_id = ? AND tool = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::optional<std::string> ServerStore::tool_cred_sealed(
    std::uint64_t group_id, const std::string& tool) {
  const char* sql =
      "SELECT sealed_hex FROM group_tool_credentials WHERE group_id = ?"
      " AND tool = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, tool.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<std::string> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out = h ? h : "";
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::ToolCredentialMeta> ServerStore::tool_cred_list(
    std::uint64_t group_id) {
  std::vector<ToolCredentialMeta> out;
  const char* sql =
      "SELECT group_id, tool, updated_by, updated_ms"
      " FROM group_tool_credentials WHERE group_id = ? ORDER BY tool ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ToolCredentialMeta m;
    m.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    m.tool = t ? t : "";
    m.updated_by = u ? u : "";
    m.updated_ms = sqlite3_column_int64(st, 3);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
