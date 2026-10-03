#include "store.hpp"

#include "cred.hpp"

#include <sqlite3.h>

#include <chrono>
#include <algorithm>
#include <map>
#include <set>

namespace memex::server {

namespace {
std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
// 与登录校验共用同一迭代数（改值需同步重建账号表口径）
constexpr int kPbkdf2IterationsForStore = 60000;
// 部门层级：ancestor 是否为 path 本身或其上级（"公司/研发" ⊑ "公司/研发/客户端组"）
bool dept_covers(const std::string& ancestor, const std::string& path) {
  if (ancestor.empty() || path.empty()) return false;
  if (path == ancestor) return true;
  return path.size() > ancestor.size() &&
         path.compare(0, ancestor.size(), ancestor) == 0 &&
         path[ancestor.size()] == '/';
}
// 逗号分隔字段表是否含某字段（精确匹配，容忍空格）
bool csv_field_has(const std::string& csv, const std::string& field) {
  std::size_t start = 0;
  while (start <= csv.size()) {
    const std::size_t comma = csv.find(',', start);
    std::string item =
        csv.substr(start, comma == std::string::npos ? std::string::npos
                                                     : comma - start);
    const auto b = item.find_first_not_of(' ');
    const auto e = item.find_last_not_of(' ');
    if (b != std::string::npos && item.substr(b, e - b + 1) == field) return true;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}
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
      "  role TEXT NOT NULL DEFAULT 'member',"
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
      "  ON login_records(account, ts_ms);"
      // T3.3 设备台账：首登建档、责任人登记、启停（停用拒绝登录）
      "CREATE TABLE IF NOT EXISTS devices ("
      "  fingerprint TEXT PRIMARY KEY,"
      "  kind TEXT NOT NULL,"
      "  name TEXT NOT NULL,"
      "  owner_account TEXT NOT NULL DEFAULT '',"
      "  enabled INTEGER NOT NULL DEFAULT 1,"
      "  first_seen_ms INTEGER NOT NULL,"
      "  last_seen_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS offline_messages ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  msg_id TEXT NOT NULL,"
      "  to_account TEXT NOT NULL,"
      "  envelope BLOB NOT NULL,"
      "  queued_ms INTEGER NOT NULL,"
      "  UNIQUE(msg_id, to_account));" // 群扇出一人多列（T4.1），按接收方 ACK
      "CREATE INDEX IF NOT EXISTS idx_offline_to"
      "  ON offline_messages(to_account, id);"
      "CREATE TABLE IF NOT EXISTS messages ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  msg_id TEXT NOT NULL UNIQUE,"
      "  from_account TEXT NOT NULL,"
      "  to_account TEXT NOT NULL,"
      "  type INTEGER NOT NULL,"
      "  text TEXT NOT NULL DEFAULT '',"
      "  ts_ms INTEGER NOT NULL,"
      "  recall INTEGER NOT NULL DEFAULT 0);"
      "CREATE INDEX IF NOT EXISTS idx_messages_to"
      "  ON messages(to_account, id);"
      "CREATE TABLE IF NOT EXISTS recall_events ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  msg_id TEXT NOT NULL,"
      "  by_account TEXT NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      // T2.6 组织架构：部门树（parent_id 成树）＋成员资料
      //（直属上级为独立单列——每人至多一名，结构性约束）
      "CREATE TABLE IF NOT EXISTS departments ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  name TEXT NOT NULL,"
      "  parent_id INTEGER,"
      "  created_ms INTEGER NOT NULL);"
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_departments_parent_name"
      "  ON departments(parent_id, name);"
      "CREATE TABLE IF NOT EXISTS member_profiles ("
      "  account TEXT PRIMARY KEY,"
      "  department_id INTEGER,"
      "  title TEXT NOT NULL DEFAULT '',"
      "  manager TEXT NOT NULL DEFAULT '',"
      "  updated_ms INTEGER NOT NULL);"
      // T3.2 查阅日志：检索／导出动作逐次落一条（只附加，不删改）
      "CREATE TABLE IF NOT EXISTS audit_reads ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  op_account TEXT NOT NULL,"
      "  action TEXT NOT NULL,"
      "  filters TEXT NOT NULL DEFAULT '',"
      "  result_count INTEGER NOT NULL,"
      "  ts_ms INTEGER NOT NULL);"
      // T3.4 策略开关：按部门配置（department_path 空=全局兜底行）
      "CREATE TABLE IF NOT EXISTS policies ("
      "  department_path TEXT PRIMARY KEY,"
      "  allow_anonymous INTEGER NOT NULL,"
      "  allow_cross_state INTEGER NOT NULL,"
      "  new_device_approval INTEGER NOT NULL);"
      // T4.1 群聊：群表＋成员表（群主退群=解散，成员记录清除、群号与归档保留）
      "CREATE TABLE IF NOT EXISTS groups ("
      "  group_id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  name TEXT NOT NULL,"
      "  owner TEXT NOT NULL,"
      "  announcement TEXT NOT NULL DEFAULT '',"
      "  created_ms INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS group_members ("
      "  group_id INTEGER NOT NULL,"
      "  account TEXT NOT NULL,"
      "  joined_ms INTEGER NOT NULL,"
      "  PRIMARY KEY(group_id, account));"
      // T4.3 已读回执：(msg_id, 已读方) 复合主键天然幂等
      "CREATE TABLE IF NOT EXISTS message_reads ("
      "  msg_id TEXT NOT NULL,"
      "  reader TEXT NOT NULL,"
      "  read_ms INTEGER NOT NULL,"
      "  PRIMARY KEY(msg_id, reader));"
      // T4.2 跨态会话日志：时间/双方/时长，不含内容（ended_ms=0 进行中）
      "CREATE TABLE IF NOT EXISTS cross_state_logs ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  account TEXT NOT NULL,"
      "  peer_device TEXT NOT NULL,"
      "  peer_name TEXT NOT NULL,"
      "  started_ms INTEGER NOT NULL,"
      "  ended_ms INTEGER NOT NULL DEFAULT 0,"
      "  duration_ms INTEGER NOT NULL DEFAULT 0);"
      // T4.5 常用联系人：(账号, 对端) 联合主键 UPSERT
      "CREATE TABLE IF NOT EXISTS favorites ("
      "  account TEXT NOT NULL,"
      "  peer TEXT NOT NULL,"
      "  starred INTEGER NOT NULL DEFAULT 0,"
      "  last_ms INTEGER NOT NULL DEFAULT 0,"
      "  PRIMARY KEY(account, peer));"
      // T4.6 通讯录可见性：按成员／部门的隐藏、限看与敏感字段（UPSERT 单行）
      "CREATE TABLE IF NOT EXISTS org_visibility ("
      "  scope TEXT NOT NULL," // member（key=账号）| dept（key=部门全路径）
      "  target_key TEXT NOT NULL,"
      "  hidden INTEGER NOT NULL DEFAULT 0,"
      "  restrict_scope INTEGER NOT NULL DEFAULT 0,"
      "  hide_fields TEXT NOT NULL DEFAULT '',"
      "  PRIMARY KEY(scope, target_key));"
      // 白名单例外：viewer 可见 target（成员账号或部门全路径，整树豁免）
      "CREATE TABLE IF NOT EXISTS org_visibility_allow ("
      "  viewer TEXT NOT NULL,"
      "  target TEXT NOT NULL,"
      "  PRIMARY KEY(viewer, target));"; // 本段为 schema 字符串最后一段
  char* err = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    sqlite3_free(err);
    return false;
  }
  // 旧库迁移：补 role 列（已存在则忽略失败）
  sqlite3_exec(db_, "ALTER TABLE accounts ADD COLUMN"
                    " role TEXT NOT NULL DEFAULT 'member'",
              nullptr, nullptr, nullptr);
  // 旧库迁移（T4.1）：offline_messages 单列 UNIQUE(msg_id) →
  // 复合 UNIQUE(msg_id, to_account)。旧表不重建则群扇出 INSERT OR IGNORE
  // 按 msg_id 把第 2..N 个离线接收方静默丢弃。事务化重建，失败回滚保数据。
  {
    bool legacy = false;
    sqlite3_stmt* st = nullptr;
    const char* probe =
        "SELECT sql FROM sqlite_master WHERE type='table'"
        " AND name='offline_messages';";
    if (sqlite3_prepare_v2(db_, probe, -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        const char* ddl =
            reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        legacy = ddl && std::string(ddl).find("UNIQUE(msg_id, to_account)") ==
                           std::string::npos;
      }
      sqlite3_finalize(st);
    }
    if (legacy) {
      const char* steps[] = {
          "BEGIN;",
          "ALTER TABLE offline_messages RENAME TO offline_messages_legacy;",
          "CREATE TABLE offline_messages ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  msg_id TEXT NOT NULL,"
          "  to_account TEXT NOT NULL,"
          "  envelope BLOB NOT NULL,"
          "  queued_ms INTEGER NOT NULL,"
          "  UNIQUE(msg_id, to_account));",
          "INSERT INTO offline_messages(msg_id, to_account, envelope,"
          " queued_ms) SELECT msg_id, to_account, envelope, queued_ms"
          " FROM offline_messages_legacy;",
          "DROP TABLE offline_messages_legacy;",
          "CREATE INDEX IF NOT EXISTS idx_offline_to"
          " ON offline_messages(to_account, id);",
          "COMMIT;",
      };
      bool migrated = true;
      for (const char* s : steps) {
        if (sqlite3_exec(db_, s, nullptr, nullptr, nullptr) != SQLITE_OK) {
          migrated = false;
          break;
        }
      }
      if (!migrated) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
  }
  return true;
}

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
  return ok; // 唯一键冲突（账号已存在）→ DONE 之外 → false
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
  sqlite3_finalize(st);
  return ok;
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

// T2.3 消息归档：协作态消息全量落库
bool ServerStore::store_message(const std::string& msg_id, const std::string& from_account,
                                const std::string& to_account, int type,
                                const std::string& text, std::int64_t ts_ms) {
  const char* sql =
      "INSERT OR IGNORE INTO messages(msg_id, from_account, to_account, type, text, ts_ms)"
      " VALUES(?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, from_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, to_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, type);
  sqlite3_bind_text(st, 5, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok; // 唯一键冲突（消息已存在）→ DONE 之外 → false
}

// 消息检索：账号为空=全部；非空=该账号收发两侧＋其所在群的群消息都命中
//（管理员检索面；T4.1 起群消息 to="group:<群号>" 联入成员检索面）
std::vector<ArchivedMessage> ServerStore::messages(const std::string& account,
                                                   int limit) {
  MessageSearch q;
  q.account = account;
  q.limit = limit;
  return search_messages(q);
}

// T3.2 条件检索：账号（收发双侧）／时间窗（含端点）／关键词（子串，LIKE 转义）AND 组合
std::vector<ArchivedMessage> ServerStore::search_messages(
    const MessageSearch& q) {
  std::vector<ArchivedMessage> out;
  std::string sql =
      "SELECT msg_id, from_account, to_account, type, text, ts_ms, recall"
      " FROM messages WHERE 1=1";
  if (!q.account.empty()) {
    // 收发双侧 ＋ 该账号所在群的群消息（to="group:<群号>"）
    sql += " AND (from_account = ? OR to_account = ? OR to_account IN"
           " (SELECT 'group:' || group_id FROM group_members"
           "  WHERE account = ?))";
  }
  // 关键词子串匹配：%／_／转义符先转义，避免用户输入被当通配符
  std::string like;
  if (!q.keyword.empty()) {
    like.reserve(q.keyword.size() + 8);
    for (const char c : q.keyword) {
      if (c == '%' || c == '_' || c == '\\') like += '\\';
      like += c;
    }
    sql += " AND text LIKE ? ESCAPE '\\'";
  }
  if (q.since_ms > 0) sql += " AND ts_ms >= ?";
  if (q.until_ms > 0) sql += " AND ts_ms <= ?";
  sql += " ORDER BY id DESC LIMIT ?;";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  if (!q.account.empty()) {
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!like.empty()) {
    const std::string pat = "%" + like + "%";
    sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (q.since_ms > 0) sqlite3_bind_int64(st, idx++, q.since_ms);
  if (q.until_ms > 0) sqlite3_bind_int64(st, idx++, q.until_ms);
  sqlite3_bind_int(st, idx, q.limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    ArchivedMessage m;
    m.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.from_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.to_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    m.type = sqlite3_column_int(st, 3);
    const char* text_ptr =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.text = text_ptr ? text_ptr : "";
    m.ts_ms = sqlite3_column_int64(st, 5);
    m.recalled = sqlite3_column_int(st, 6) != 0;
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

// 查阅留痕：只附加
bool ServerStore::add_audit_read(const AuditReadRow& rec) {
  const char* sql =
      "INSERT INTO audit_reads(op_account, action, filters, result_count, ts_ms)"
      " VALUES(?, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, rec.op_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, rec.action.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, rec.filters.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 4, rec.result_count);
  sqlite3_bind_int64(st, 5, rec.ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<AuditReadRow> ServerStore::audit_reads(int limit) {
  std::vector<AuditReadRow> out;
  const char* sql =
      "SELECT id, op_account, action, filters, result_count, ts_ms"
      " FROM audit_reads ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    AuditReadRow r;
    r.id = sqlite3_column_int64(st, 0);
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    r.op_account = text_of(1);
    r.action = text_of(2);
    r.filters = text_of(3);
    r.result_count = sqlite3_column_int(st, 4);
    r.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// 消息撤回：只置标记不清正文
bool ServerStore::recall_message(const std::string& msg_id) {
  const char* sql = "UPDATE messages SET recall = 1 WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

// 查询消息是否被撤回
bool ServerStore::is_recalled(const std::string& msg_id) {
  const char* sql = "SELECT recall FROM messages WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  bool recalled = false;
  if (sqlite3_step(st) == SQLITE_ROW) {
    recalled = sqlite3_column_int(st, 0) != 0;
  }
  sqlite3_finalize(st);
  return recalled;
}

// 查某消息的发送方（撤回权限判定：只能撤回自己发的）
std::string ServerStore::message_from(const std::string& msg_id) {
  const char* sql = "SELECT from_account FROM messages WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  std::string out;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);
  return out;
}

// 撤回事件独立记录（原文与序得在此表对账；只附加，不删改）
bool ServerStore::record_recall_event(const std::string& msg_id,
                                      const std::string& by_account,
                                      std::int64_t ts_ms) {
  const char* sql =
      "INSERT INTO recall_events(msg_id, by_account, ts_ms) VALUES(?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, by_account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::size_t ServerStore::recall_event_count(const std::string& msg_id) {
  const char* sql = "SELECT COUNT(*) FROM recall_events WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  std::size_t n = 0;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    n = static_cast<std::size_t>(sqlite3_column_int64(st, 0));
  }
  sqlite3_finalize(st);
  return n;
}

// —— T2.6 组织架构 ——

// 部门路径逐级创建："公司/研发部/客户端组" → 三级，已存在即复用
int ServerStore::ensure_department_path(const std::string& path) {
  if (path.empty()) return -1;
  int parent_id = -1;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string seg =
        path.substr(start, slash == std::string::npos ? std::string::npos
                                                      : slash - start);
    if (seg.empty()) return -1; // 连续斜杠／尾斜杠等非法路径
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id FROM departments WHERE name = ? AND"
                      " ((parent_id IS NULL AND ? = -1) OR parent_id = ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, seg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, parent_id);
    sqlite3_bind_int(st, 3, parent_id);
    int id = -1;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (id < 0) {
      const char* ins =
          "INSERT INTO departments(name, parent_id, created_ms)"
          " VALUES(?, ?, ?);";
      if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) != SQLITE_OK) {
        return -1;
      }
      sqlite3_bind_text(st, 1, seg.c_str(), -1, SQLITE_TRANSIENT);
      if (parent_id < 0) {
        sqlite3_bind_null(st, 2);
      } else {
        sqlite3_bind_int(st, 2, parent_id);
      }
      sqlite3_bind_int64(st, 3, now_ms());
      if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
      }
      sqlite3_finalize(st);
      id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    }
    parent_id = id;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return parent_id;
}

std::vector<std::pair<int, std::string>> ServerStore::department_list() {
  std::vector<std::pair<int, std::string>> out;
  // 自底向上拼全路径（递归 CTE；SQLite ≥3.8.3）
  const char* sql =
      "WITH RECURSIVE tree(id, name, path) AS ("
      " SELECT id, name, name FROM departments WHERE parent_id IS NULL"
      " UNION ALL"
      " SELECT d.id, d.name, tree.path || '/' || d.name"
      "  FROM departments d JOIN tree ON d.parent_id = tree.id)"
      " SELECT id, path FROM tree ORDER BY path;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    out.emplace_back(sqlite3_column_int(st, 0),
                     reinterpret_cast<const char*>(sqlite3_column_text(st, 1)));
  }
  sqlite3_finalize(st);
  return out;
}

std::string ServerStore::department_path(int id) {
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT path FROM ("
                    "WITH RECURSIVE tree(id, name, path) AS ("
                    " SELECT id, name, name FROM departments WHERE parent_id IS NULL"
                    " UNION ALL"
                    " SELECT d.id, d.name, tree.path || '/' || d.name"
                    "  FROM departments d JOIN tree ON d.parent_id = tree.id)"
                    " SELECT id, path FROM tree) WHERE id = ?;";
  std::string out;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, id);
  if (sqlite3_step(st) == SQLITE_ROW) {
    out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::set_member_profile(const std::string& account,
                                     int department_id, const std::string& title,
                                     const std::string& manager) {
  if (!find_account(account)) return false;           // 账号不存在
  if (manager == account) return false;               // 不得自为上级
  if (!manager.empty() && !find_account(manager)) {
    return false;                                     // 上级账号不存在
  }
  if (!manager.empty()) {
    // 环校验：从拟设上级沿现有链路上溯，若回到本人则构成环
    std::string cur = manager;
    std::set<std::string> seen;
    while (!cur.empty() && cur != account) {
      if (!seen.insert(cur).second) break; // 既有环防御，止步
      const auto p = member_profile(cur);
      if (!p) break;
      if (p->manager == account) return false; // 链路回到本人：环
      cur = p->manager;
    }
  }
  const char* sql =
      "INSERT INTO member_profiles(account, department_id, title, manager,"
      " updated_ms) VALUES(?, ?, ?, ?, ?)"
      " ON CONFLICT(account) DO UPDATE SET department_id = excluded.department_id,"
      " title = excluded.title, manager = excluded.manager,"
      " updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  if (department_id < 0) {
    sqlite3_bind_null(st, 2);
  } else {
    sqlite3_bind_int(st, 2, department_id);
  }
  sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, manager.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::optional<MemberProfile> ServerStore::member_profile(
    const std::string& account) {
  const char* sql =
      "SELECT p.title, p.manager, p.department_id,"
      " a.display_name, a.role FROM member_profiles p"
      " JOIN accounts a ON a.account = p.account WHERE p.account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<MemberProfile> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    MemberProfile m;
    m.account = account;
    m.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.manager = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    if (sqlite3_column_type(st, 2) != SQLITE_NULL) {
      m.department_path = department_path(sqlite3_column_int(st, 2));
    }
    m.display_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    m.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    out = m;
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<MemberProfile> ServerStore::member_list() {
  std::vector<MemberProfile> out;
  const char* sql =
      "SELECT a.account, a.display_name, a.role, p.title, p.manager,"
      " p.department_id FROM accounts a"
      " LEFT JOIN member_profiles p ON p.account = a.account"
      " ORDER BY a.account;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    MemberProfile m;
    m.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.display_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
      m.title = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    }
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
      m.manager = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    }
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
      m.department_path = department_path(sqlite3_column_int(st, 5));
    }
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<std::string> ServerStore::manager_chain(
    const std::string& account) {
  std::vector<std::string> chain;
  std::set<std::string> seen{account};
  std::string cur = account;
  while (true) {
    const auto p = member_profile(cur);
    if (!p || p->manager.empty()) break;
    if (!seen.insert(p->manager).second) break; // 环防御
    chain.push_back(p->manager);
    cur = p->manager;
  }
  return chain;
}

OrgImportResult ServerStore::import_members(
    const std::vector<OrgImportRow>& rows) {
  OrgImportResult result;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  for (const OrgImportRow& r : rows) {
    sqlite3_exec(db_, "SAVEPOINT imp;", nullptr, nullptr, nullptr);
    int dept_id = -1;
    std::string error;
    if (!find_account(r.account)) {
      error = "账号不存在";
    } else if (r.manager == r.account) {
      error = "不得自为直属上级";
    } else if (!r.manager.empty() && !find_account(r.manager)) {
      error = "直属上级账号不存在";
    } else if (!r.dept.empty() && (dept_id = ensure_department_path(r.dept)) < 0) {
      error = "部门路径非法";
    }
    if (error.empty() && !set_member_profile(r.account, dept_id, r.title,
                                             r.manager)) {
      error = "构成汇报环或写入失败";
    }
    if (error.empty()) {
      sqlite3_exec(db_, "RELEASE imp;", nullptr, nullptr, nullptr);
      ++result.imported;
    } else {
      sqlite3_exec(db_, "ROLLBACK TO imp;", nullptr, nullptr, nullptr);
      sqlite3_exec(db_, "RELEASE imp;", nullptr, nullptr, nullptr);
      result.errors.push_back("第 " + std::to_string(r.line_no) + " 行（" +
                              r.account + "）被拒绝：" + error);
    }
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return result;
}

// —— T3.4 策略开关 ——

bool ServerStore::set_policy(const std::string& department_path,
                             bool allow_anonymous, bool allow_cross_state,
                             bool new_device_approval) {
  if (!department_path.empty()) {
    // 部门行须挂已存在部门（防拼错挂空名；不顺手建部门）
    bool found = false;
    for (const auto& [id, path] : department_list()) {
      (void)id;
      if (path == department_path) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  const char* sql =
      "INSERT INTO policies(department_path, allow_anonymous, allow_cross_state,"
      " new_device_approval) VALUES(?, ?, ?, ?)"
      " ON CONFLICT(department_path) DO UPDATE SET"
      " allow_anonymous = excluded.allow_anonymous,"
      " allow_cross_state = excluded.allow_cross_state,"
      " new_device_approval = excluded.new_device_approval;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, allow_anonymous ? 1 : 0);
  sqlite3_bind_int(st, 3, allow_cross_state ? 1 : 0);
  sqlite3_bind_int(st, 4, new_device_approval ? 1 : 0);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<PolicyRow> ServerStore::policy_list() {
  std::vector<PolicyRow> out;
  const char* sql =
      "SELECT department_path, allow_anonymous, allow_cross_state,"
      " new_device_approval FROM policies"
      " ORDER BY department_path = '' DESC, department_path ASC;"; // 全局行在前
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    PolicyRow p;
    const char* path_ptr =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    p.department_path = path_ptr ? path_ptr : "";
    p.allow_anonymous = sqlite3_column_int(st, 1) != 0;
    p.allow_cross_state = sqlite3_column_int(st, 2) != 0;
    p.new_device_approval = sqlite3_column_int(st, 3) != 0;
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

// 生效策略：本人部门 → 逐级上级部门（路径去尾）→ 全局行 → 内置默认
PolicyRow ServerStore::resolve_policy(const std::string& account) {
  const auto all = [&] {
    std::map<std::string, PolicyRow> by_path;
    for (const auto& p : policy_list()) by_path[p.department_path] = p;
    return by_path;
  }();
  std::string path;
  if (const auto prof = member_profile(account)) {
    path = prof->department_path;
  }
  while (!path.empty()) {
    const auto it = all.find(path);
    if (it != all.end()) return it->second;
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) break;
    path.resize(pos);
  }
  if (const auto it = all.find(""); it != all.end()) return it->second;
  PolicyRow fallback; // 内置默认：宽松（免登录可用、跨态可通、新设备免审批）
  return fallback;
}

// —— T4.6 通讯录可见性 ——

bool ServerStore::set_visibility(const std::string& scope,
                                 const std::string& key, bool hidden,
                                 bool restrict_scope,
                                 const std::string& hide_fields) {
  if (scope != "member" && scope != "dept") return false;
  if (key.empty()) return false;
  if (scope == "member") {
    if (!find_account(key).has_value()) return false;
  } else {
    bool exists = false;
    for (const auto& [id, path] : department_list()) {
      (void)id;
      if (path == key) {
        exists = true;
        break;
      }
    }
    if (!exists) return false;
  }
  const char* sql =
      "INSERT INTO org_visibility(scope, target_key, hidden, restrict_scope,"
      " hide_fields) VALUES(?,?,?,?,?)"
      " ON CONFLICT(scope, target_key) DO UPDATE SET"
      " hidden=excluded.hidden, restrict_scope=excluded.restrict_scope,"
      " hide_fields=excluded.hide_fields;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, hidden ? 1 : 0);
  sqlite3_bind_int(st, 4, restrict_scope ? 1 : 0);
  sqlite3_bind_text(st, 5, hide_fields.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::optional<VisibilityRow> ServerStore::visibility_row(
    const std::string& scope, const std::string& key) {
  const char* sql =
      "SELECT hidden, restrict_scope, hide_fields FROM org_visibility"
      " WHERE scope=? AND target_key=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<VisibilityRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    VisibilityRow r;
    r.scope = scope;
    r.key = key;
    r.hidden = sqlite3_column_int(st, 0) != 0;
    r.restrict_scope = sqlite3_column_int(st, 1) != 0;
    if (sqlite3_column_type(st, 2) != SQLITE_NULL) {
      r.hide_fields = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    }
    out = r;
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<VisibilityRow> ServerStore::visibility_list() {
  std::vector<VisibilityRow> out;
  const char* sql =
      "SELECT scope, target_key, hidden, restrict_scope, hide_fields"
      " FROM org_visibility ORDER BY scope, target_key;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    VisibilityRow r;
    r.scope = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.key = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.hidden = sqlite3_column_int(st, 2) != 0;
    r.restrict_scope = sqlite3_column_int(st, 3) != 0;
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
      r.hide_fields = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    }
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::add_visibility_allow(const std::string& viewer,
                                       const std::string& target) {
  if (viewer.empty() || target.empty()) return false;
  const char* sql =
      "INSERT OR IGNORE INTO org_visibility_allow(viewer, target)"
      " VALUES(?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, viewer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, target.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::remove_visibility_allow(const std::string& viewer,
                                          const std::string& target) {
  const char* sql =
      "DELETE FROM org_visibility_allow WHERE viewer=? AND target=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, viewer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, target.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<VisibilityAllow> ServerStore::visibility_allows() {
  std::vector<VisibilityAllow> out;
  const char* sql =
      "SELECT viewer, target FROM org_visibility_allow"
      " ORDER BY viewer, target;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    VisibilityAllow a;
    a.viewer = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    a.target = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

namespace {

// 过滤上下文（一次计算，member/dept 两表共用）：查看者角色与部门、白名单
// 目标集、生效的限看范围、全部配置行。
struct VisCtx {
  bool is_admin{false};
  std::string vdept;              // 查看者所在部门（空=未分配）
  std::set<std::string> allows;   // 白名单目标（账号或部门全路径）
  std::string limit_dept;         // 生效的限看部门（空=不限）
  std::vector<VisibilityRow> rows; // 全部配置行
};

} // namespace

std::vector<MemberProfile> ServerStore::visible_members(
    const std::string& viewer) {
  VisCtx ctx;
  ctx.rows = visibility_list();
  if (const auto vprof = member_profile(viewer)) {
    ctx.vdept = vprof->department_path;
    ctx.is_admin = vprof->role == "admin";
  }
  for (const auto& a : visibility_allows()) {
    if (a.viewer == viewer) ctx.allows.insert(a.target);
  }
  if (!ctx.is_admin) {
    // 限看本部门：本人部门链上最近的 restrict 行（与策略解析同口径）
    std::string path = ctx.vdept;
    while (!path.empty()) {
      if (const auto r = visibility_row("dept", path); r && r->restrict_scope) {
        ctx.limit_dept = path;
        break;
      }
      const auto pos = path.rfind('/');
      if (pos == std::string::npos) break;
      path.resize(pos);
    }
  }

  const auto whitelisted = [&](const std::string& account,
                               const std::string& dept) {
    if (ctx.allows.count(account) != 0) return true;
    for (const std::string& t : ctx.allows) {
      if (dept_covers(t, dept)) return true;
    }
    return false;
  };
  const auto dept_hidden_outside =
      [&](const std::string& dept) { // 部门被隐藏且查看者不在其内
      if (dept.empty()) return false;
      for (const auto& r : ctx.rows) {
        if (r.scope == "dept" && r.hidden && dept_covers(r.key, dept) &&
            !dept_covers(r.key, ctx.vdept)) {
          return true;
        }
      }
      return false;
    };

  std::vector<MemberProfile> out;
  for (const auto& m : member_list()) {
    if (ctx.is_admin || m.account == viewer) {
      out.push_back(m); // 管理员全量；查看者本人始终在列
      continue;
    }
    const bool allow = whitelisted(m.account, m.department_path);
    if (!allow) {
      // ① 限看本部门：非白名单成员须落在限看部门子树内
      if (!ctx.limit_dept.empty() &&
          !dept_covers(ctx.limit_dept, m.department_path)) {
        continue;
      }
      // ② 隐藏成员；③ 隐藏部门（整树）——部门内自己人互见
      bool hidden = false;
      if (const auto r = visibility_row("member", m.account);
          r && r->hidden) {
        hidden = true;
      }
      if (!hidden) hidden = dept_hidden_outside(m.department_path);
      if (hidden) continue;
    }
    // ④ 敏感字段脱敏（非管理员、非白名单）
    MemberProfile m2 = m;
    if (!allow) {
      if (const auto r = visibility_row("member", m.account)) {
        if (csv_field_has(r->hide_fields, "title")) m2.title.clear();
        if (csv_field_has(r->hide_fields, "manager")) m2.manager.clear();
        if (csv_field_has(r->hide_fields, "role")) m2.role.clear();
      }
    }
    out.push_back(std::move(m2));
  }
  // ⑤ 上级引用随可见性走：直属上级若不可见则抹去（不留不可见者的账号线索）
  std::set<std::string> visible_accounts;
  for (const auto& m : out) visible_accounts.insert(m.account);
  for (auto& m : out) {
    if (!m.manager.empty() && visible_accounts.count(m.manager) == 0) {
      m.manager.clear();
    }
  }
  return out;
}

std::vector<std::pair<int, std::string>> ServerStore::visible_departments(
    const std::string& viewer) {
  // 与 visible_members 同规则；另保证可见成员所在部门链完整
  //（客户端按全路径成树，链缺一级成员就挂不上）
  VisCtx ctx;
  ctx.rows = visibility_list();
  if (const auto vprof = member_profile(viewer)) {
    ctx.vdept = vprof->department_path;
    ctx.is_admin = vprof->role == "admin";
  }
  for (const auto& a : visibility_allows()) {
    if (a.viewer == viewer) ctx.allows.insert(a.target);
  }
  if (!ctx.is_admin) {
    std::string path = ctx.vdept;
    while (!path.empty()) {
      if (const auto r = visibility_row("dept", path); r && r->restrict_scope) {
        ctx.limit_dept = path;
        break;
      }
      const auto pos = path.rfind('/');
      if (pos == std::string::npos) break;
      path.resize(pos);
    }
  }
  std::set<std::string> needed; // 可见成员所在部门链（含各级前缀）
  for (const auto& m : visible_members(viewer)) {
    std::string path = m.department_path;
    while (!path.empty()) {
      needed.insert(path);
      const auto pos = path.rfind('/');
      if (pos == std::string::npos) break;
      path.resize(pos);
    }
  }
  std::vector<std::pair<int, std::string>> out;
  for (const auto& [id, path] : department_list()) {
    if (needed.count(path) != 0) {
      out.emplace_back(id, path);
      continue;
    }
    if (ctx.is_admin) {
      out.emplace_back(id, path);
      continue;
    }
    if (!ctx.limit_dept.empty() && !dept_covers(ctx.limit_dept, path)) continue;
    bool skip = false;
    for (const auto& r : ctx.rows) {
      if (r.scope != "dept" || !r.hidden || !dept_covers(r.key, path)) continue;
      const bool in_dept = dept_covers(r.key, ctx.vdept);
      const bool allow = ctx.allows.count(r.key) != 0 ||
                         ctx.allows.count(path) != 0;
      if (!in_dept && !allow) skip = true;
    }
    if (skip) continue;
    out.emplace_back(id, path);
  }
  return out;
}

// —— T4.1 群聊 ——

std::uint64_t ServerStore::create_group(const std::string& name,
                                        const std::string& owner,
                                        const std::vector<std::string>& members) {
  if (name.empty() || owner.empty()) return 0;
  // 成员账号存在性校验（建群者一并校验）；去重
  std::vector<std::string> uniq;
  for (const auto& a : members) {
    if (std::find(uniq.begin(), uniq.end(), a) == uniq.end()) uniq.push_back(a);
  }
  for (const auto& a : uniq) {
    if (!find_account(a).has_value()) return 0;
  }
  if (std::find(uniq.begin(), uniq.end(), owner) == uniq.end()) {
    uniq.push_back(owner);
  }

  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  const char* sql =
      "INSERT INTO groups(name, owner, announcement, created_ms)"
      " VALUES(?, ?, '', ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, now_ms());
  if (sqlite3_step(st) != SQLITE_DONE) {
    sqlite3_finalize(st);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return 0;
  }
  sqlite3_finalize(st);
  const std::uint64_t gid =
      static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db_));
  for (const auto& a : uniq) {
    sqlite3_stmt* ms = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT OR IGNORE INTO group_members(group_id,"
                           " account, joined_ms) VALUES(?, ?, ?);",
                           -1, &ms, nullptr) != SQLITE_OK) {
      continue;
    }
    sqlite3_bind_int64(ms, 1, static_cast<sqlite3_int64>(gid));
    sqlite3_bind_text(ms, 2, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ms, 3, now_ms());
    sqlite3_step(ms);
    sqlite3_finalize(ms);
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return gid;
}

std::vector<std::string> ServerStore::group_members(std::uint64_t group_id) {
  std::vector<std::string> out;
  const char* sql =
      "SELECT account FROM group_members WHERE group_id = ?"
      " ORDER BY joined_ms ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (p) out.emplace_back(p);
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<GroupInfo> ServerStore::group_info(std::uint64_t group_id) {
  const char* sql =
      "SELECT group_id, name, owner, announcement FROM groups"
      " WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  std::optional<GroupInfo> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupInfo g;
    g.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    g.name = text_of(1);
    g.owner = text_of(2);
    g.announcement = text_of(3);
    g.members = group_members(group_id);
    out = std::move(g);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<GroupInfo> ServerStore::groups_of(const std::string& account) {
  std::vector<GroupInfo> out;
  const char* sql =
      "SELECT group_id FROM group_members WHERE account = ?"
      " ORDER BY group_id ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<std::uint64_t> ids;
  while (sqlite3_step(st) == SQLITE_ROW) {
    ids.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(st, 0)));
  }
  sqlite3_finalize(st);
  for (const auto id : ids) {
    if (auto g = group_info(id)) out.push_back(std::move(*g));
  }
  return out;
}

bool ServerStore::is_group_member(std::uint64_t group_id,
                                  const std::string& account) {
  for (const auto& a : group_members(group_id)) {
    if (a == account) return true;
  }
  return false;
}

bool ServerStore::group_invite(std::uint64_t group_id,
                               const std::string& account) {
  if (!group_info(group_id).has_value()) return false;
  if (!find_account(account).has_value()) return false;
  if (is_group_member(group_id, account)) return false;
  const char* sql =
      "INSERT INTO group_members(group_id, account, joined_ms)"
      " VALUES(?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, now_ms());
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::group_leave(std::uint64_t group_id,
                              const std::string& account) {
  const auto info = group_info(group_id);
  if (!info.has_value() || !is_group_member(group_id, account)) return false;
  sqlite3_exec(db_, "BEGIN;", nullptr, nullptr, nullptr);
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "DELETE FROM group_members WHERE group_id = ?"
                         " AND account = ?;",
                         -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  if (info->owner == account) {
    // 群主退群＝解散：清完整张成员表（群号与历史归档保留，留痕纪律）
    if (sqlite3_prepare_v2(db_,
                           "DELETE FROM group_members WHERE group_id = ?;",
                           -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
  }
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool ServerStore::group_announce(std::uint64_t group_id,
                                 const std::string& owner,
                                 const std::string& announcement) {
  const auto info = group_info(group_id);
  if (!info.has_value() || info->owner != owner) return false;
  const char* sql = "UPDATE groups SET announcement = ? WHERE group_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, announcement.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(group_id));
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

// —— T4.2 跨态会话日志 ——

// 只记时间/双方/时长，不含内容（上报帧本身也无内容字段）
bool ServerStore::cross_log_start(const std::string& account,
                                  const std::string& peer_device,
                                  const std::string& peer_name,
                                  std::int64_t started_ms) {
  if (account.empty() || peer_device.empty()) return false;
  const char* sql =
      "INSERT INTO cross_state_logs(account, peer_device, peer_name,"
      " started_ms) SELECT ?, ?, ?, ? WHERE NOT EXISTS"
      " (SELECT 1 FROM cross_state_logs WHERE account = ? AND peer_device = ?"
      "  AND started_ms = ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, peer_device.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, peer_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, started_ms);
  sqlite3_bind_text(st, 5, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 6, peer_device.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, started_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::cross_log_end(const std::string& account,
                                const std::string& peer_device,
                                std::int64_t started_ms,
                                std::int64_t ended_ms) {
  if (account.empty() || peer_device.empty() || ended_ms <= started_ms) {
    return false;
  }
  const char* sql =
      "UPDATE cross_state_logs SET ended_ms = ?, duration_ms = ?"
      " WHERE id = (SELECT id FROM cross_state_logs WHERE account = ?"
      "  AND peer_device = ? AND started_ms = ? AND ended_ms = 0"
      "  ORDER BY id ASC LIMIT 1);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, ended_ms);
  sqlite3_bind_int64(st, 2, ended_ms - started_ms);
  sqlite3_bind_text(st, 3, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, peer_device.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, started_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<CrossLogRow> ServerStore::cross_logs(int limit) {
  std::vector<CrossLogRow> out;
  const char* sql =
      "SELECT id, account, peer_device, peer_name, started_ms, ended_ms,"
      " duration_ms FROM cross_state_logs ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    CrossLogRow r;
    r.id = sqlite3_column_int64(st, 0);
    const auto text_of = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    r.account = text_of(1);
    r.peer_device = text_of(2);
    r.peer_name = text_of(3);
    r.started_ms = sqlite3_column_int64(st, 4);
    r.ended_ms = sqlite3_column_int64(st, 5);
    r.duration_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— T4.5 常用联系人 ——

bool ServerStore::fav_star(const std::string& account,
                           const std::string& peer, bool starred) {
  if (account.empty() || peer.empty()) return false;
  const char* sql =
      "INSERT INTO favorites(account, peer, starred, last_ms)"
      " VALUES(?, ?, ?, 0)"
      " ON CONFLICT(account, peer) DO UPDATE SET starred=excluded.starred;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, starred ? 1 : 0);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::fav_touch(const std::string& account,
                            const std::string& peer, std::int64_t ts_ms) {
  if (account.empty() || peer.empty() || ts_ms <= 0) return false;
  const char* sql =
      "INSERT INTO favorites(account, peer, starred, last_ms)"
      " VALUES(?, ?, 0, ?)"
      " ON CONFLICT(account, peer) DO UPDATE SET"
      " last_ms=MAX(last_ms, excluded.last_ms);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<FavRow> ServerStore::fav_list(const std::string& account) {
  std::vector<FavRow> out;
  const char* sql =
      "SELECT peer, starred, last_ms FROM favorites WHERE account = ?"
      " ORDER BY starred DESC, last_ms DESC, peer ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    FavRow r;
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.peer = p ? std::string(p) : std::string{};
    r.starred = sqlite3_column_int(st, 1) != 0;
    r.last_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// 归档起点（A8）：首条归档消息（含其所在群消息）之前最近一次成功登录时刻。
// 跨态直连会话不进归档——故归档起点＝进入协作态的实际登录时间。
// 已读上报只给归档库存在的消息留痕（伪造 msg_id 灌库直接拒绝）；
// 同 (msg_id, 已读方) 重复上报幂等——首条为准，仍返回 true。
bool ServerStore::record_read(const std::string& msg_id,
                              const std::string& reader,
                              std::int64_t read_ms) {
  if (msg_id.empty() || reader.empty() || read_ms <= 0) return false;
  sqlite3_stmt* st = nullptr;
  const char* exists = "SELECT 1 FROM messages WHERE msg_id = ? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, exists, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool known = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  if (!known) return false;
  const char* sql =
      "INSERT OR IGNORE INTO message_reads(msg_id, reader, read_ms)"
      " VALUES(?, ?, ?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, reader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, read_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ReadRow> ServerStore::readers_for(const std::string& msg_id) {
  std::vector<ReadRow> out;
  sqlite3_stmt* st = nullptr;
  const char* sql =
      "SELECT msg_id, reader, read_ms FROM message_reads WHERE msg_id = ?"
      " ORDER BY read_ms ASC;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    ReadRow r;
    r.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    r.reader = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.read_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::archive_start_ms(const std::string& account) {
  if (account.empty()) return 0;
  std::int64_t first_msg = 0;
  {
    const char* sql =
        "SELECT MIN(ts_ms) FROM messages WHERE from_account = ?"
        " OR to_account = ?"
        " OR to_account IN (SELECT 'group:' || group_id FROM group_members"
        "  WHERE account = ?);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, account.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
      first_msg = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
  }
  if (first_msg == 0) return 0; // 无归档
  std::int64_t login_ms = 0;
  {
    const char* sql =
        "SELECT MAX(ts_ms) FROM login_records WHERE account = ?"
        " AND result = 'ok' AND ts_ms <= ?;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      return first_msg;
    }
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, first_msg);
    if (sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_type(st, 0) != SQLITE_NULL) {
      login_ms = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
  }
  return login_ms > 0 ? login_ms : first_msg;
}

} // namespace memex::server
