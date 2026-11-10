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

namespace {
// 与登录校验共用同一迭代数（改值需同步重建账号表口径）
constexpr int kPbkdf2IterationsForStore = 60000;
} // namespace

bool ServerStore::create_account(const std::string& account,
                                 const std::string& password,
                                 const std::string& display_name,
                                 const std::string& role) {
  const std::string salt = random_salt_hex();
  if (salt.empty()) return false;
  const char* sql =
      "INSERT INTO accounts(account, display_name, salt, digest, role,"
      " created_ms) VALUES(?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  // 摘要计算独立于语句，失败即放弃本次插入（盐只存在内存，无残留）
  std::string digest;
  try {
    digest = pbkdf2_sha256_hex(password, salt, kPbkdf2IterationsForStore);
  } catch (...) {
    sqlite3_finalize(st);
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, display_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, salt.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, digest.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, (role == "admin") ? "admin" : "member", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return false; // 唯一键冲突（账号已存在）→ false
  // 平台-2：口令凭据独立落 credentials（判登唯一来源；accounts 行内
  // salt/digest 仅作老列兼容保留，不再参与判登）
  return credential_insert(account, "password", salt, digest, now_ms());
}

// 凭据行插入（create_account 与未来面共用；UNIQUE 冲突=false）
bool ServerStore::credential_insert(const std::string& account,
                                    const std::string& type,
                                    const std::string& salt_hex,
                                    const std::string& digest_hex,
                                    std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO credentials(account, type, salt, digest, created_ms)"
      " VALUES(?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, salt_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, digest_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

// —— 平台-2 Identity 模型补全 ——

std::optional<ServerStore::CredentialRow> ServerStore::find_credential(
    const std::string& account, const std::string& type) {
  const char* sql =
      "SELECT id, account, type, salt, digest, created_ms, disabled"
      " FROM credentials WHERE account = ? AND type = ? AND disabled = 0"
      " ORDER BY id DESC LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<CredentialRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    CredentialRow r;
    r.id = sqlite3_column_int64(st, 0);
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    const char* d = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.account = a ? a : "";
    r.type = t ? t : "";
    r.salt_hex = s ? s : "";
    r.digest_hex = d ? d : "";
    r.created_ms = sqlite3_column_int64(st, 5);
    r.disabled = sqlite3_column_int(st, 6) != 0;
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::identity_bind(const std::string& account,
                                        const std::string& issuer,
                                        const std::string& subject,
                                        std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return 0;
  if (issuer.empty() || subject.empty()) return 0;
  const char* sql =
      "INSERT INTO identity_bindings(account, issuer, subject, created_ms)"
      " VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, issuer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, subject.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0; // 唯一冲突（已绑）→ 0
}

bool ServerStore::identity_unbind(std::int64_t id) {
  const char* sql = "DELETE FROM identity_bindings WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::optional<ServerStore::IdentityBinding> ServerStore::identity_find(
    const std::string& issuer, const std::string& subject) {
  const char* sql =
      "SELECT id, account, issuer, subject, created_ms"
      " FROM identity_bindings WHERE issuer = ? AND subject = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, issuer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<IdentityBinding> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    IdentityBinding r;
    r.id = sqlite3_column_int64(st, 0);
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* i = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.account = a ? a : "";
    r.issuer = i ? i : "";
    r.subject = s ? s : "";
    r.created_ms = sqlite3_column_int64(st, 4);
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::IdentityBinding> ServerStore::identity_list(
    const std::string& account) {
  std::vector<IdentityBinding> out;
  const char* sql =
      "SELECT id, account, issuer, subject, created_ms"
      " FROM identity_bindings WHERE (? = '' OR account = ?)"
      " ORDER BY id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    IdentityBinding r;
    r.id = sqlite3_column_int64(st, 0);
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* i = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.account = a ? a : "";
    r.issuer = i ? i : "";
    r.subject = s ? s : "";
    r.created_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::session_insert(const SessionRecord& rec) {
  const char* sql =
      "INSERT OR REPLACE INTO sessions(token_hash, account, scope, device_id,"
      " created_ms, expires_ms, logged_out_ms, logout_reason)"
      " VALUES(?,?,?,?,?,?,0,'');";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, rec.token_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, rec.account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, rec.scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, rec.device_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, rec.created_ms);
  sqlite3_bind_int64(st, 6, rec.expires_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::session_close(const std::string& token_hash,
                                const std::string& reason,
                                std::int64_t ts_ms) {
  const char* sql =
      "UPDATE sessions SET logged_out_ms = ?, logout_reason = ?"
      " WHERE token_hash = ? AND logged_out_ms = 0;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ts_ms);
  sqlite3_bind_text(st, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, token_hash.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::SessionRecord> ServerStore::session_list(
    const std::string& account, int limit) {
  std::vector<SessionRecord> out;
  const char* sql =
      "SELECT token_hash, account, scope, device_id, created_ms, expires_ms,"
      " logged_out_ms, logout_reason FROM sessions"
      " WHERE (? = '' OR account = ?) ORDER BY created_ms DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, limit > 0 ? limit : 100);
  while (sqlite3_step(st) == SQLITE_ROW) {
    SessionRecord r;
    const char* h = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    const char* s = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    const char* d = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.token_hash = h ? h : "";
    r.account = a ? a : "";
    r.scope = s ? s : "";
    r.device_id = d ? d : "";
    r.created_ms = sqlite3_column_int64(st, 4);
    r.expires_ms = sqlite3_column_int64(st, 5);
    r.logged_out_ms = sqlite3_column_int64(st, 6);
    const char* lr =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.logout_reason = lr ? lr : "";
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<AccountRow> ServerStore::find_account(const std::string& account) {
  const char* sql =
      "SELECT account, display_name, salt, digest, role FROM accounts"
      " WHERE account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<AccountRow> row;
  if (sqlite3_step(st) == SQLITE_ROW) {
    AccountRow r;
    r.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.display_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.salt_hex = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.digest_hex = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    row = r;
  }
  sqlite3_finalize(st);
  return row;
}

std::vector<std::pair<std::string, std::string>> ServerStore::account_list() {
  std::vector<std::pair<std::string, std::string>> out;
  const char* sql =
      "SELECT account, display_name || '（' || role || '）' FROM accounts"
      " ORDER BY account;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)),
        reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::add_login_record(const LoginRecord& rec) {
  const char* sql =
      "INSERT INTO login_records(account, fingerprint, kind, name, source_ip,"
      " version, result, ts_ms) VALUES(?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, rec.account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, rec.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, rec.kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, rec.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, rec.source_ip.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, rec.version.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, rec.result.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, rec.ts_ms != 0 ? rec.ts_ms : now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<LoginRecord> ServerStore::login_records(const std::string& account,
                                                    const std::string& fp_prefix,
                                                    int limit) {
  std::vector<LoginRecord> out;
  const char* sql =
      "SELECT id, account, fingerprint, kind, name, source_ip, version, result,"
      " ts_ms FROM login_records";
  std::string query = sql;
  std::string where;
  if (!account.empty()) where += "account = ?";
  if (!fp_prefix.empty()) {
    if (!where.empty()) where += " AND ";
    where += "fingerprint LIKE ? || '%'";
  }
  if (!where.empty()) query += " WHERE " + where;
  query += " ORDER BY ts_ms DESC, id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, query.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  if (!account.empty()) {
    sqlite3_bind_text(st, idx++, account.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!fp_prefix.empty()) {
    sqlite3_bind_text(st, idx++, fp_prefix.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(st, idx, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    LoginRecord r;
    r.id = sqlite3_column_int64(st, 0);
    r.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.source_ip = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    r.version = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.result = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.ts_ms = sqlite3_column_int64(st, 8);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— 需求批⑩ 在线时长：上下线事件流水 ——

bool ServerStore::add_presence_event(const std::string& account,
                                     const std::string& event,
                                     std::int64_t ts_ms) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT INTO presence_events (account, event, ts_ms)"
                         " VALUES (?, ?, ?);",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, event.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::online_ms_between(const std::string& account,
                                            std::int64_t from_ms,
                                            std::int64_t to_ms) const {
  std::int64_t sum = 0;
  if (to_ms <= from_ms) return sum;
  // 全量事件按插入序（id）取——登记点单调递增，等价时间序
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT event, ts_ms FROM presence_events"
                         " WHERE account = ? ORDER BY id ASC;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return sum;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  // 深度扫掠：depth 0→正开段（起点=max(事件 ts, from)）、正→0 闭段
  //（终点=min(事件 ts, to)）；depth>0 的 offline 只减一（多端并行去重），
  // depth==0 的 offline 是脏数据直接忽略。末笔 online 未闭→计到窗尾。
  std::int64_t depth = 0;
  std::int64_t open_since = -1;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* ev =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const std::int64_t ts = sqlite3_column_int64(st, 1);
    const bool was_open = depth > 0;
    if (ev && std::string(ev) == "online") {
      ++depth;
    } else if (depth > 0) {
      --depth;
    }
    if (!was_open && depth > 0) {
      if (ts < to_ms) open_since = std::max(ts, from_ms);
    } else if (was_open && depth == 0) {
      if (ts > from_ms && open_since >= 0) {
        sum += std::min(ts, to_ms) - open_since;
      }
      open_since = -1;
    }
  }
  if (depth > 0 && open_since >= 0) sum += to_ms - open_since;
  sqlite3_finalize(st);
  return sum;
}

void ServerStore::trim_dangling_online(std::int64_t ts_ms) {
  // 末态 depth>0 的账号：online 计数多于 offline（断电/崩溃丢掉闭笔）
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT account, SUM(CASE WHEN event = 'online'"
                         " THEN 1 ELSE -1 END) AS depth"
                         " FROM presence_events GROUP BY account"
                         " HAVING depth > 0;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return;
  }
  std::vector<std::string> dangling;
  while (sqlite3_step(st) == SQLITE_ROW) {
    dangling.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  }
  sqlite3_finalize(st);
  for (const auto& acct : dangling) {
    add_presence_event(acct, "offline", ts_ms);
  }
}

// —— T3.3 设备台账 ——

// 首登建档（INSERT OR IGNORE 保首见时间）、再登刷新 last_seen
bool ServerStore::upsert_device(const std::string& fingerprint,
                                const std::string& kind,
                                const std::string& name,
                                std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO devices(fingerprint, kind, name, first_seen_ms, last_seen_ms)"
      " VALUES(?, ?, ?, ?, ?)"
      " ON CONFLICT(fingerprint) DO UPDATE SET"
      " kind = excluded.kind, name = excluded.name,"
      " last_seen_ms = excluded.last_seen_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::optional<DeviceRow> ServerStore::find_device(
    const std::string& fingerprint) {
  const char* sql =
      "SELECT fingerprint, kind, name, owner_account, enabled,"
      " first_seen_ms, last_seen_ms FROM devices WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
  sqlite3_bind_text(st, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<DeviceRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    DeviceRow d;
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    d.fingerprint = text_of(0);
    d.kind = text_of(1);
    d.name = text_of(2);
    d.owner_account = text_of(3);
    d.enabled = sqlite3_column_int(st, 4) != 0;
    d.first_seen_ms = sqlite3_column_int64(st, 5);
    d.last_seen_ms = sqlite3_column_int64(st, 6);
    out = std::move(d);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<DeviceRow> ServerStore::device_list() {
  std::vector<DeviceRow> out;
  const char* sql =
      "SELECT fingerprint, kind, name, owner_account, enabled,"
      " first_seen_ms, last_seen_ms FROM devices"
      " ORDER BY last_seen_ms DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    DeviceRow d;
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    d.fingerprint = text_of(0);
    d.kind = text_of(1);
    d.name = text_of(2);
    d.owner_account = text_of(3);
    d.enabled = sqlite3_column_int(st, 4) != 0;
    d.first_seen_ms = sqlite3_column_int64(st, 5);
    d.last_seen_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(d));
  }
  sqlite3_finalize(st);
  return out;
}

// 指纹前缀定位：≥8 位防误配；多义时 second=true（CLI 拒绝并提示补长）
std::pair<std::string, bool> ServerStore::device_by_prefix(
    const std::string& prefix) {
  if (prefix.size() < 8) return {"", false};
  std::vector<std::string> hits;
  for (const auto& d : device_list()) {
    if (d.fingerprint.compare(0, prefix.size(), prefix) == 0) {
      hits.push_back(d.fingerprint);
    }
  }
  if (hits.size() == 1) return {hits[0], false};
  return {"", hits.size() > 1};
}

// 责任人登记：账号须已存在（防拼错挂空名）
bool ServerStore::set_device_owner(const std::string& fingerprint,
                                   const std::string& owner_account) {
  if (!find_account(owner_account).has_value()) return false;
  const char* sql = "UPDATE devices SET owner_account = ? WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, owner_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::set_device_enabled(const std::string& fingerprint,
                                     bool enabled) {
  const char* sql = "UPDATE devices SET enabled = ? WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, enabled ? 1 : 0);
  sqlite3_bind_text(st, 2, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// 解绑：清责任人并停用（该设备须重新启用并登记责任人才可用）
bool ServerStore::unbind_device(const std::string& fingerprint) {
  const char* sql =
      "UPDATE devices SET owner_account = '', enabled = 0"
      " WHERE fingerprint = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::queue_offline(const std::string& msg_id,
                                const std::string& to_account,
                                const std::string& envelope_blob) {
  const char* sql =
      "INSERT OR IGNORE INTO offline_messages(msg_id, to_account, envelope,"
      " queued_ms) VALUES(?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, to_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(st, 3, envelope_blob.data(),
                    static_cast<int>(envelope_blob.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  // 真落新行（changes>0；撞键被 IGNORE 跳过不唤醒）才触发钩子——bot 长轮
  // 询唤醒源，注：changes 须在本语句 finalize 前读（期间无其他语句）
  const int inserted = ok ? sqlite3_changes(db_) : 0;
  sqlite3_finalize(st);
  if (inserted > 0 && enqueue_hook_) enqueue_hook_(to_account);
  return ok;
}

void ServerStore::set_enqueue_hook(
    void* owner, std::function<void(const std::string&)> hook) {
  enqueue_hook_owner_ = owner;
  enqueue_hook_ = std::move(hook);
}

void ServerStore::clear_enqueue_hook(void* owner) {
  if (enqueue_hook_owner_ != owner) return; // 非注册者本人：不动现行钩子
  enqueue_hook_owner_ = nullptr;
  enqueue_hook_ = nullptr;
}

std::vector<std::string> ServerStore::pending_offline(const std::string& account) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT envelope FROM offline_messages WHERE to_account = ?"
      " ORDER BY id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const auto* p = static_cast<const char*>(sqlite3_column_blob(st, 0));
    const int n = sqlite3_column_bytes(st, 0);
    out.emplace_back(p, static_cast<std::size_t>(n));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::ack_offline(const std::string& msg_id,
                              const std::string& account) {
  // 群扇出场景：同一条群消息对每名成员各有一行，仅清本接收方那份
  const char* sql =
      "DELETE FROM offline_messages WHERE msg_id = ? AND to_account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::size_t ServerStore::offline_count(const std::string& account) {
  const char* sql =
      "SELECT COUNT(*) FROM offline_messages WHERE to_account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::size_t n = 0;
  if (sqlite3_step(st) == SQLITE_ROW) {
    n = static_cast<std::size_t>(sqlite3_column_int64(st, 0));
  }
  sqlite3_finalize(st);
  return n;
}

std::vector<std::string> ServerStore::admin_accounts() {
  std::vector<std::string> out;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT account FROM accounts WHERE role = 'admin'"
                         " ORDER BY account;",
                         -1, &st, nullptr) != SQLITE_OK)
    return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
