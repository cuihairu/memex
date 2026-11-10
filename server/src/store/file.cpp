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

// —— R23-1 文件存储元数据（服务端权限/配额判断层）——
// 秒传键 UNIQUE(file_hash, owner)：同属主同哈希复用对象键，不重复落字节。

std::int64_t ServerStore::create_file_meta(const FileMeta& meta) {
  if (meta.owner.empty() || meta.file_name.empty()) return 0;
  const char* sql =
      "INSERT OR IGNORE INTO files(owner, belong_gid, belong_uid, file_name,"
      " file_size, file_hash, object_key, source, upload_ts, status, kind)"
      " VALUES(?,?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, meta.owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, meta.belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, meta.belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, meta.file_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, meta.file_size);
  sqlite3_bind_text(st, 6, meta.file_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 7, meta.object_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 8, static_cast<int>(meta.source));
  sqlite3_bind_int64(st, 9, meta.upload_ts);
  sqlite3_bind_int(st, 10, static_cast<int>(meta.status));
  sqlite3_bind_int(st, 11, static_cast<int>(meta.kind));
  std::int64_t id = 0;
  if (sqlite3_step(st) == SQLITE_DONE) {
    if (sqlite3_changes(db_) > 0) {
      id = sqlite3_last_insert_rowid(db_);
    } else {
      // 秒传键冲突（UNIQUE(file_hash, owner, belong_gid, belong_uid, kind)
      // 已有行）：last_insert_rowid 会返回连接上上一次无关插入的 rowid，
      // 必须回查已有行的 id 返回。
      sqlite3_finalize(st);
      const char* lookup =
          "SELECT id FROM files WHERE file_hash=? AND owner=?"
          " AND belong_gid=? AND belong_uid=? AND kind=?;";
      if (sqlite3_prepare_v2(db_, lookup, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, meta.file_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, meta.owner.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, meta.belong_gid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, meta.belong_uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, static_cast<int>(meta.kind));
        if (sqlite3_step(st) == SQLITE_ROW) {
          id = sqlite3_column_int64(st, 0);
        }
      }
    }
  }
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::FileMeta> ServerStore::check_second_transfer(
    const std::string& owner, const std::string& file_hash,
    const std::string& belong_gid, const std::string& belong_uid,
    FileKind kind) {
  if (owner.empty() || file_hash.empty()) return std::nullopt;
  // 秒传按「属主+哈希+归属+类目」判：同属主同哈希在另一群/个人空间/
  // 收件箱不算命中（对象前缀本就按归属隔离，见设计铁律 3）。
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files WHERE owner=? AND file_hash=? AND status=0"
      " AND belong_gid=? AND belong_uid=? AND kind=? LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, file_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, static_cast<int>(kind));
  std::optional<FileMeta> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::FileMeta> ServerStore::list_files(
    const std::string& belong_gid, const std::string& belong_uid, int limit,
    int offset, int kind) {
  std::vector<FileMeta> out;
  // kind<0 不过滤类目（全量）；0/1 只列对应类目（R23-3 空间隔离）
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files"
      " WHERE (? = '' OR belong_gid = ?) AND (? = '' OR belong_uid = ?)"
      " AND (? < 0 OR kind = ?)"
      " ORDER BY pin DESC, id DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, belong_gid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, belong_uid.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 5, kind);
  sqlite3_bind_int(st, 6, kind);
  sqlite3_bind_int(st, 7, limit);
  sqlite3_bind_int(st, 8, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::delete_file_meta(std::int64_t file_id) {
  const char* sql = "DELETE FROM files WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, file_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

namespace {
// 配额行读取（两级：群/人；无行=0 用量、0 上限=不限）。
ServerStore::QuotaInfo read_quota(sqlite3* db, const char* sql,
                                  const std::string& key) {
  ServerStore::QuotaInfo q;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return q;
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    q.gid = key;
    q.uid = key;
    q.used_bytes = sqlite3_column_int64(st, 1);
    q.limit_bytes = sqlite3_column_int64(st, 2);
  } else {
    q.gid = key;
    q.uid = key;
  }
  sqlite3_finalize(st);
  return q;
}
bool add_quota_used(sqlite3* db, const char* table, const char* key_col,
                    const std::string& key, std::int64_t delta) {
  std::string upsert = std::string("INSERT INTO ") + table + "(" + key_col +
                       ", used_bytes, limit_bytes) VALUES(?,?,0)"
                       " ON CONFLICT(" +
                       key_col +
                       ") DO UPDATE SET used_bytes=used_bytes+excluded.used_bytes;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, upsert.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, delta);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}
bool set_quota_limit(sqlite3* db, const char* table, const char* key_col,
                     const std::string& key, std::int64_t limit) {
  std::string upsert = std::string("INSERT INTO ") + table + "(" + key_col +
                       ", used_bytes, limit_bytes) VALUES(?,0,?)"
                       " ON CONFLICT(" +
                       key_col + ") DO UPDATE SET limit_bytes=excluded.limit_bytes;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, upsert.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, limit);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}
// 受检扣费（原子，单语句判限）：无行则建（limit=0=不限）；有行时仅当
// limit=0 或 used+delta≤limit 才更新。超限/负 delta 一律 changes=0。
bool charge_quota(sqlite3* db, const char* table, const char* key_col,
                  const std::string& key, std::int64_t delta) {
  std::string upsert =
      std::string("INSERT INTO ") + table + "(" + key_col +
      ", used_bytes, limit_bytes) VALUES(?,?,0)"
      " ON CONFLICT(" +
      key_col +
      ") DO UPDATE SET used_bytes=used_bytes+excluded.used_bytes"
      " WHERE " +
      table + ".limit_bytes=0"
              " OR " +
      table + ".used_bytes+excluded.used_bytes<=" + table + ".limit_bytes;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, upsert.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, delta);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
  sqlite3_finalize(st);
  return ok;
}
} // namespace

std::optional<ServerStore::QuotaInfo> ServerStore::get_group_quota(
    const std::string& gid) {
  if (gid.empty()) return std::nullopt;
  return read_quota(db_,
                    "SELECT gid, used_bytes, limit_bytes FROM group_quota"
                    " WHERE gid=?;",
                    gid);
}

std::optional<ServerStore::QuotaInfo> ServerStore::get_user_quota(
    const std::string& uid) {
  if (uid.empty()) return std::nullopt;
  return read_quota(db_,
                    "SELECT uid, used_bytes, limit_bytes FROM user_quota"
                    " WHERE uid=?;",
                    uid);
}

bool ServerStore::add_group_quota_used(const std::string& gid,
                                       std::int64_t delta_bytes) {
  if (gid.empty()) return false;
  return add_quota_used(db_, "group_quota", "gid", gid, delta_bytes);
}

bool ServerStore::add_user_quota_used(const std::string& uid,
                                      std::int64_t delta_bytes) {
  if (uid.empty()) return false;
  return add_quota_used(db_, "user_quota", "uid", uid, delta_bytes);
}

bool ServerStore::set_group_quota_limit(const std::string& gid,
                                        std::int64_t limit_bytes) {
  if (gid.empty() || limit_bytes < 0) return false;
  return set_quota_limit(db_, "group_quota", "gid", gid, limit_bytes);
}

bool ServerStore::set_user_quota_limit(const std::string& uid,
                                       std::int64_t limit_bytes) {
  if (uid.empty() || limit_bytes < 0) return false;
  return set_quota_limit(db_, "user_quota", "uid", uid, limit_bytes);
}

bool ServerStore::add_uplink_log(const UplinkLog& log) {
  if (log.uploader.empty() || log.file_name.empty()) return false;
  const char* sql =
      "INSERT INTO uplink_logs(uploader, file_name, file_size, file_hash,"
      " object_key, upload_ts) VALUES(?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, log.uploader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, log.file_name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, log.file_size);
  sqlite3_bind_text(st, 4, log.file_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 5, log.object_key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, log.upload_ts);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::UplinkLog> ServerStore::list_uplink_logs(
    const std::string& uploader, int limit) {
  std::vector<UplinkLog> out;
  const char* sql =
      "SELECT id, uploader, file_name, file_size, file_hash, object_key,"
      " upload_ts FROM uplink_logs"
      " WHERE (? = '' OR uploader = ?)"
      " ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, uploader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, uploader.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, limit);
  while (sqlite3_step(st) == SQLITE_ROW) {
    UplinkLog r;
    r.id = sqlite3_column_int64(st, 0);
    r.uploader = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.file_size = sqlite3_column_int64(st, 3);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    r.upload_ts = sqlite3_column_int64(st, 6);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::FileMeta> ServerStore::list_uplink_files(
    const std::string& owner, int limit, int offset) {
  std::vector<FileMeta> out;
  // 只列本人 uplink 来源的收件箱文件（source/kind 双过滤：内网收件箱
  // 文件不出现在外网面；他人记录同理不可见）
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files"
      " WHERE owner = ? AND source = 1 AND kind = 1"
      " ORDER BY id DESC LIMIT ? OFFSET ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, limit);
  sqlite3_bind_int(st, 3, offset);
  while (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

// —— R23-2 文件管理面：按 id 取/置顶/状态、对象引用计数、受检配额扣费、
//    群内角色（权限模型：群主/管理员/成员）——

std::optional<ServerStore::FileMeta> ServerStore::file_by_id(
    std::int64_t file_id) {
  const char* sql =
      "SELECT id, owner, belong_gid, belong_uid, file_name, file_size,"
      " file_hash, object_key, source, upload_ts, status, pin, kind"
      " FROM files WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, file_id);
  std::optional<FileMeta> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    FileMeta r;
    r.id = sqlite3_column_int64(st, 0);
    r.owner = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.belong_gid = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.belong_uid = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.file_name = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    r.file_size = sqlite3_column_int64(st, 5);
    r.file_hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.object_key = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    r.source = static_cast<FileSource>(sqlite3_column_int(st, 8));
    r.upload_ts = sqlite3_column_int64(st, 9);
    r.status = static_cast<FileStatus>(sqlite3_column_int(st, 10));
    r.pin = sqlite3_column_int(st, 11) != 0;
    r.kind = static_cast<FileKind>(sqlite3_column_int(st, 12));
    out = std::move(r);
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::set_file_pin(std::int64_t file_id, bool pin) {
  const char* sql = "UPDATE files SET pin=? WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, pin ? 1 : 0);
  sqlite3_bind_int64(st, 2, file_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::set_file_status(std::int64_t file_id, FileStatus status) {
  const char* sql = "UPDATE files SET status=? WHERE id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int(st, 1, static_cast<int>(status));
  sqlite3_bind_int64(st, 2, file_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::count_file_refs(const std::string& object_key) {
  if (object_key.empty()) return 0;
  const char* sql = "SELECT COUNT(*) FROM files WHERE object_key=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, object_key.c_str(), -1, SQLITE_TRANSIENT);
  std::int64_t n = 0;
  if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return n;
}

bool ServerStore::charge_group_quota(const std::string& gid,
                                     std::int64_t delta_bytes) {
  if (gid.empty() || delta_bytes < 0) return false;
  // 原子受检扣费：limit=0=不限；超限整条不生效（changes=0 → 上传拒绝）。
  return charge_quota(db_, "group_quota", "gid", gid, delta_bytes);
}

bool ServerStore::charge_user_quota(const std::string& uid,
                                    std::int64_t delta_bytes) {
  if (uid.empty() || delta_bytes < 0) return false;
  return charge_quota(db_, "user_quota", "uid", uid, delta_bytes);
}


} // namespace memex::server
