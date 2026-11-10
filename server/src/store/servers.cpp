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

// —— R26-1 服务器 agent 面 ——

std::uint64_t ServerStore::server_enroll(std::uint64_t group_id,
                                         const std::string& name,
                                         const std::string& host,
                                         const std::string& token_hash,
                                         const std::string& enrolled_by,
                                         std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return 0;
  // 同 gid+name 复用一行（UNIQUE 闸）：重登记=轮换令牌，id 稳定
  const char* sql =
      "INSERT INTO group_servers(group_id, name, host, token_hash,"
      " enrolled_by, created_ms) VALUES(?,?,?,?,?,?)"
      " ON CONFLICT(group_id, name) DO UPDATE SET host=excluded.host,"
      " token_hash=excluded.token_hash,"
      " enrolled_by=excluded.enrolled_by, created_ms=excluded.created_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, host.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, enrolled_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::uint64_t id =
      ok ? static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_)) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupServerRow>
ServerStore::server_by_token_hash(const std::string& token_hash) {
  const char* sql =
      "SELECT id, group_id, name, host, enrolled_by, created_ms,"
      " last_seen_ms, cpu_percent, mem_used_mb, mem_total_mb,"
      " disk_used_mb, disk_total_mb, load1"
      " FROM group_servers WHERE token_hash = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<GroupServerRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupServerRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.name = n ? n : "";
    r.host = h ? h : "";
    r.enrolled_by = e ? e : "";
    r.created_ms = sqlite3_column_int64(st, 5);
    r.last_seen_ms = sqlite3_column_int64(st, 6);
    r.cpu_percent = sqlite3_column_double(st, 7);
    r.mem_used_mb = sqlite3_column_double(st, 8);
    r.mem_total_mb = sqlite3_column_double(st, 9);
    r.disk_used_mb = sqlite3_column_double(st, 10);
    r.disk_total_mb = sqlite3_column_double(st, 11);
    r.load1 = sqlite3_column_double(st, 12);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::server_heartbeat(std::uint64_t id, double cpu_percent,
                                   double mem_used_mb, double mem_total_mb,
                                   double disk_used_mb, double disk_total_mb,
                                   double load1, std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_servers SET last_seen_ms=?, cpu_percent=?,"
      " mem_used_mb=?, mem_total_mb=?, disk_used_mb=?, disk_total_mb=?,"
      " load1=? WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_double(st, 2, cpu_percent);
  sqlite3_bind_double(st, 3, mem_used_mb);
  sqlite3_bind_double(st, 4, mem_total_mb);
  sqlite3_bind_double(st, 5, disk_used_mb);
  sqlite3_bind_double(st, 6, disk_total_mb);
  sqlite3_bind_double(st, 7, load1);
  sqlite3_bind_int64(st, 8, static_cast<sqlite3_int64>(id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupServerRow> ServerStore::server_list(
    std::uint64_t group_id) {
  std::vector<GroupServerRow> out;
  const char* sql =
      "SELECT s.id, s.group_id, s.name, s.host, s.enrolled_by, s.created_ms,"
      " s.last_seen_ms, s.cpu_percent, s.mem_used_mb, s.mem_total_mb,"
      " s.disk_used_mb, s.disk_total_mb, s.load1,"
      " COALESCE(c.updated_by, ''), COALESCE(c.updated_ms, 0)"
      " FROM group_servers s"
      " LEFT JOIN group_server_credentials c ON c.server_id = s.id"
      " WHERE s.group_id = ? ORDER BY s.id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupServerRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* e = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.name = n ? n : "";
    r.host = h ? h : "";
    r.enrolled_by = e ? e : "";
    r.created_ms = sqlite3_column_int64(st, 5);
    r.last_seen_ms = sqlite3_column_int64(st, 6);
    r.cpu_percent = sqlite3_column_double(st, 7);
    r.mem_used_mb = sqlite3_column_double(st, 8);
    r.mem_total_mb = sqlite3_column_double(st, 9);
    r.disk_used_mb = sqlite3_column_double(st, 10);
    r.disk_total_mb = sqlite3_column_double(st, 11);
    r.load1 = sqlite3_column_double(st, 12);
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 13));
    r.cred_updated_by = cb ? cb : "";
    r.cred_updated_ms = sqlite3_column_int64(st, 14);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R26-3 远程会话面 ——

std::uint64_t ServerStore::server_session_open(std::uint64_t group_id,
                                               std::uint64_t server_id,
                                               const std::string& actor,
                                               const std::string& protocol,
                                               const std::string& ticket_hash,
                                               std::int64_t ts_ms) {
  // 短票锚定的服务器须属该群（不透他群服务器存在性）
  sqlite3_stmt* chk = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT id FROM group_servers WHERE id = ? AND"
                         " group_id = ?;",
                         -1, &chk, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_int64(chk, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_int64(chk, 2, static_cast<sqlite3_int64>(group_id));
  const bool owned = sqlite3_step(chk) == SQLITE_ROW;
  sqlite3_finalize(chk);
  if (!owned) return 0;
  const char* sql =
      "INSERT INTO group_server_sessions(group_id, server_id, actor,"
      " protocol, ticket_hash, opened_ms) VALUES(?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_text(st, 3, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, protocol.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, ticket_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::uint64_t id =
      ok ? static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_)) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::ServerSessionRow>
ServerStore::server_session_by_ticket(const std::string& ticket_hash) {
  const char* sql =
      "SELECT s.id, s.group_id, s.server_id, g.name, g.host, s.actor,"
      " s.protocol, s.opened_ms, s.redeemed_ms, s.closed_ms"
      " FROM group_server_sessions s"
      " JOIN group_servers g ON g.id = s.server_id"
      " WHERE s.ticket_hash = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, ticket_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<ServerSessionRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    ServerSessionRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    r.server_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.server_name = n ? n : "";
    r.host = h ? h : "";
    r.actor = a ? a : "";
    r.protocol = p ? p : "";
    r.opened_ms = sqlite3_column_int64(st, 7);
    r.redeemed_ms = sqlite3_column_int64(st, 8);
    r.closed_ms = sqlite3_column_int64(st, 9);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::server_session_mark_redeemed(std::uint64_t id,
                                               std::int64_t ts_ms) {
  // 一次性闸：redeemed_ms=0 才更新（并发双兑只有一胜）
  const char* sql =
      "UPDATE group_server_sessions SET redeemed_ms=? WHERE id = ? AND"
      " redeemed_ms = 0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::server_session_close(std::uint64_t group_id,
                                       std::uint64_t id,
                                       const std::string& actor,
                                       std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_server_sessions SET closed_ms=? WHERE id = ? AND"
      " group_id = ? AND actor = ? AND closed_ms = 0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(id));
  sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 4, actor.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::ServerSessionRow> ServerStore::server_session_list(
    std::uint64_t group_id) {
  std::vector<ServerSessionRow> out;
  const char* sql =
      "SELECT s.id, s.group_id, s.server_id, g.name, g.host, s.actor,"
      " s.protocol, s.opened_ms, s.redeemed_ms, s.closed_ms"
      " FROM group_server_sessions s"
      " JOIN group_servers g ON g.id = s.server_id"
      " WHERE s.group_id = ? ORDER BY s.id DESC LIMIT 200;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ServerSessionRow r;
    r.id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    r.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    r.server_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.server_name = n ? n : "";
    r.host = h ? h : "";
    r.actor = a ? a : "";
    r.protocol = p ? p : "";
    r.opened_ms = sqlite3_column_int64(st, 7);
    r.redeemed_ms = sqlite3_column_int64(st, 8);
    r.closed_ms = sqlite3_column_int64(st, 9);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R26-4 服务器凭据面 ——

bool ServerStore::server_cred_set(std::uint64_t group_id,
                                  std::uint64_t server_id,
                                  const std::string& sealed_hex,
                                  const std::string& updated_by,
                                  std::int64_t ts_ms) {
  // 服务器须属该群（跨群 id 不给过）
  sqlite3_stmt* own = nullptr;
  const char* own_sql =
      "SELECT 1 FROM group_servers WHERE id = ? AND group_id = ?;";
  if (sqlite3_prepare_v2(db_, own_sql, -1, &own, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(own, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_int64(own, 2, static_cast<sqlite3_int64>(group_id));
  const bool owned = sqlite3_step(own) == SQLITE_ROW;
  sqlite3_finalize(own);
  if (!owned) return false;
  const char* sql =
      "INSERT INTO group_server_credentials(server_id, sealed_hex,"
      " updated_by, updated_ms) VALUES(?,?,?,?)"
      " ON CONFLICT(server_id) DO UPDATE SET"
      " sealed_hex=excluded.sealed_hex, updated_by=excluded.updated_by,"
      " updated_ms=excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_text(st, 2, sealed_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::server_cred_delete(std::uint64_t group_id,
                                     std::uint64_t server_id) {
  const char* sql =
      "DELETE FROM group_server_credentials WHERE server_id = ? AND"
      " EXISTS(SELECT 1 FROM group_servers s"
      " WHERE s.id = server_id AND s.group_id = ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(server_id));
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::optional<std::string> ServerStore::server_cred_sealed(
    std::uint64_t server_id) {
  const char* sql =
      "SELECT sealed_hex FROM group_server_credentials WHERE server_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(server_id));
  std::optional<std::string> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out = h ? h : "";
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::ServerCredentialMeta> ServerStore::server_cred_list(
    std::uint64_t group_id) {
  std::vector<ServerCredentialMeta> out;
  const char* sql =
      "SELECT c.server_id, c.updated_by, c.updated_ms"
      " FROM group_server_credentials c"
      " JOIN group_servers s ON s.id = c.server_id"
      " WHERE s.group_id = ? ORDER BY c.server_id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    ServerCredentialMeta m;
    m.server_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* u = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.updated_by = u ? u : "";
    m.updated_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
