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

// —— R24-3 群密码箱：服务端只存 b64 密文/包裹块，不碰明文、无解锁状态 ——
bool ServerStore::group_vault_init(std::uint64_t group_id,
                                   const std::string& kdf_salt,
                                   int kdf_iters,
                                   const std::string& wrapped_dek,
                                   std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_vaults (group_id, kdf_salt, kdf_iters, wrapped_dek,"
      " created_ms, updated_ms) VALUES (?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, kdf_salt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, kdf_iters);
  sqlite3_bind_text(st, 4, wrapped_dek.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE; // 主键冲突=false（409）
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::group_vault_rekey(std::uint64_t group_id,
                                    const std::string& kdf_salt,
                                    int kdf_iters,
                                    const std::string& wrapped_dek,
                                    std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_vaults SET kdf_salt = ?, kdf_iters = ?, wrapped_dek = ?,"
      " updated_ms = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, kdf_salt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, kdf_iters);
  sqlite3_bind_text(st, 3, wrapped_dek.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  sqlite3_bind_int64(st, 5, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return false;
  sqlite3_stmt* cnt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT changes();", -1, &cnt, nullptr) !=
      SQLITE_OK) {
    return true;
  }
  const bool touched = sqlite3_step(cnt) == SQLITE_ROW &&
                       sqlite3_column_int(cnt, 0) > 0;
  sqlite3_finalize(cnt);
  return touched; // 无箱行（404）
}

std::optional<ServerStore::GroupVault> ServerStore::group_vault_info(
    std::uint64_t group_id) {
  const char* sql =
      "SELECT group_id, kdf_salt, kdf_iters, wrapped_dek, created_ms,"
      " updated_ms FROM group_vaults WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  std::optional<GroupVault> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupVault v;
    v.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* w = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    v.kdf_salt = s ? s : "";
    v.kdf_iters = sqlite3_column_int(st, 2);
    v.wrapped_dek = w ? w : "";
    v.created_ms = sqlite3_column_int64(st, 4);
    v.updated_ms = sqlite3_column_int64(st, 5);
    out = std::move(v);
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::vault_create_entry(
    std::uint64_t group_id, const std::string& name,
    const std::string& account_name, const std::string& secret_ct,
    const std::string& secret_nonce, const std::string& author,
    std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_vault_entries (group_id, name, account_name,"
      " secret_ct, secret_nonce, created_by, created_ms, updated_ms)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, account_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, secret_ct.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, secret_nonce.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, author.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  sqlite3_bind_int64(st, 8, ts_ms);
  std::int64_t id = -1;
  if (sqlite3_step(st) == SQLITE_DONE) {
    id = sqlite3_last_insert_rowid(db_);
  }
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupVaultEntry> ServerStore::vault_entry_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, name, account_name, secret_ct, secret_nonce,"
      " created_by, created_ms, updated_ms FROM group_vault_entries"
      " WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupVaultEntry> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupVaultEntry e;
    e.id = sqlite3_column_int64(st, 0);
    e.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* s[5] = {reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 2)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 3)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 4)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 5)),
                        reinterpret_cast<const char*>(
                            sqlite3_column_text(st, 6))};
    e.name = s[0] ? s[0] : "";
    e.account_name = s[1] ? s[1] : "";
    e.secret_ct = s[2] ? s[2] : "";
    e.secret_nonce = s[3] ? s[3] : "";
    e.created_by = s[4] ? s[4] : "";
    e.created_ms = sqlite3_column_int64(st, 7);
    e.updated_ms = sqlite3_column_int64(st, 8);
    out = std::move(e);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::vault_update_entry(std::int64_t id,
                                     const std::string& name,
                                     const std::string& account_name,
                                     const std::string& secret_ct,
                                     const std::string& secret_nonce,
                                     const std::string& editor,
                                     std::int64_t ts_ms) {
  const char* sql =
      "UPDATE group_vault_entries SET name = ?, account_name = ?,"
      " secret_ct = ?, secret_nonce = ?, updated_ms = ?"
      " WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, secret_ct.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, secret_nonce.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_bind_int64(st, 6, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::vault_delete_entry(std::int64_t id, std::uint64_t group_id) {
  const char* sql =
      "DELETE FROM group_vault_entries WHERE id = ? AND group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupVaultEntry> ServerStore::vault_list_entries(
    std::uint64_t group_id) {
  std::vector<GroupVaultEntry> out;
  const char* sql =
      "SELECT id, group_id, name, account_name, created_by, created_ms,"
      " updated_ms FROM group_vault_entries WHERE group_id = ?"
      " ORDER BY updated_ms DESC, id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupVaultEntry e;
    e.id = sqlite3_column_int64(st, 0);
    e.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* b = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    e.name = n ? n : "";
    e.account_name = a ? a : "";
    e.created_by = b ? b : "";
    e.created_ms = sqlite3_column_int64(st, 5);
    e.updated_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(e));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::vault_set_acl(
    std::uint64_t group_id, const std::vector<std::string>& accounts) {
  // 事务替换：清空重灌（空名单=恢复全成员）
  if (sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    return false;
  }
  const char* del = "DELETE FROM group_vault_acl WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, del, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_finalize(st);
  const char* ins =
      "INSERT OR IGNORE INTO group_vault_acl (group_id, account, added_ms)"
      " VALUES (?, ?, ?);";
  if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  const std::int64_t now = now_ms();
  for (const auto& a : accounts) {
    if (a.empty()) continue;
    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
    sqlite3_bind_text(st, 2, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    if (sqlite3_step(st) != SQLITE_DONE) {
      sqlite3_finalize(st);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }
  sqlite3_finalize(st);
  if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  return true;
}

std::vector<std::string> ServerStore::vault_acl_list(std::uint64_t group_id) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT account FROM group_vault_acl WHERE group_id = ?"
      " ORDER BY added_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    out.emplace_back(a ? a : "");
  }
  sqlite3_finalize(st);
  return out;
}

void ServerStore::vault_audit_add(std::uint64_t group_id,
                                  std::int64_t entry_id,
                                  const std::string& actor,
                                  const std::string& action,
                                  std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO group_vault_audit (group_id, entry_id, actor, action,"
      " ts_ms) VALUES (?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int64(st, 2, entry_id);
  sqlite3_bind_text(st, 3, actor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  sqlite3_step(st);
  sqlite3_finalize(st);
}

std::vector<ServerStore::GroupVaultAudit> ServerStore::vault_audit_list(
    std::uint64_t group_id, int limit) {
  std::vector<GroupVaultAudit> out;
  const char* sql =
      "SELECT id, group_id, entry_id, actor, action, ts_ms"
      " FROM group_vault_audit WHERE group_id = ?"
      " ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_int(st, 2, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupVaultAudit a;
    a.id = sqlite3_column_int64(st, 0);
    a.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    a.entry_id = sqlite3_column_int64(st, 2);
    const char* ac = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* act = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    a.actor = ac ? ac : "";
    a.action = act ? act : "";
    a.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
