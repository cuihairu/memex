#include "store.hpp"

#include "cred.hpp"

#include <sqlite3.h>

#include <chrono>

namespace memex::server {

namespace {
std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
// 与登录校验共用同一迭代数（改值需同步重建账号表口径）
constexpr int kPbkdf2IterationsForStore = 60000;
} // namespace

ServerStore::~ServerStore() { close(); }

bool ServerStore::open(const std::string& path) {
  close();
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return false;
  }
  db_ = db;
  if (!ensure_schema()) {
    close();
    return false;
  }
  return true;
}

void ServerStore::close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool ServerStore::ensure_schema() {
  const char* sql =
      "CREATE TABLE IF NOT EXISTS accounts ("
      "  account TEXT PRIMARY KEY,"
      "  display_name TEXT NOT NULL,"
      "  salt TEXT NOT NULL,"
      "  digest TEXT NOT NULL,"
      "  created_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS login_records ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  fingerprint TEXT NOT NULL,"
      "  kind TEXT NOT NULL,"
      "  name TEXT NOT NULL,"
      "  source_ip TEXT NOT NULL,"
      "  version TEXT NOT NULL,"
      "  result TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_login_records_account"
      "  ON login_records(account, ts_ms);";
  char* err = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    sqlite3_free(err);
    return false;
  }
  return true;
}

bool ServerStore::create_account(const std::string& account,
                                 const std::string& password,
                                 const std::string& display_name) {
  const std::string salt = random_salt_hex();
  if (salt.empty()) return false;
  const char* sql =
      "INSERT INTO accounts(account, display_name, salt, digest, created_ms)"
      " VALUES(?, ?, ?, ?, ?);";
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
  sqlite3_bind_int64(st, 5, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok; // 唯一键冲突（账号已存在）→ DONE 之外 → false
}

std::optional<AccountRow> ServerStore::find_account(const std::string& account) {
  const char* sql =
      "SELECT account, display_name, salt, digest FROM accounts WHERE account = ?;";
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
    row = r;
  }
  sqlite3_finalize(st);
  return row;
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
                                                    int limit) {
  std::vector<LoginRecord> out;
  const char* sql =
      "SELECT id, account, fingerprint, kind, name, source_ip, version, result,"
      " ts_ms FROM login_records";
  std::string query = sql;
  if (!account.empty()) query += " WHERE account = ?";
  query += " ORDER BY ts_ms DESC, id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, query.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  if (!account.empty()) {
    sqlite3_bind_text(st, idx++, account.c_str(), -1, SQLITE_TRANSIENT);
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

} // namespace memex::server
