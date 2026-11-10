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

// —— 品牌物料（设计稿 docs/design/品牌物料.md）：单行聚合，写后 version+1 ——

ServerStore::Branding ServerStore::branding_get() {
  Branding b;
  const char* sql =
      "SELECT company_name, accent, slogan, logo, splash, version,"
      " updated_ms FROM branding WHERE id = 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return b;
  if (sqlite3_step(st) == SQLITE_ROW) {
    b.company_name =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    b.accent = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    b.slogan = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    if (sqlite3_column_type(st, 3) == SQLITE_BLOB) {
      const auto* p =
          static_cast<const unsigned char*>(sqlite3_column_blob(st, 3));
      b.logo.assign(p, p + sqlite3_column_bytes(st, 3));
    }
    if (sqlite3_column_type(st, 4) == SQLITE_BLOB) {
      const auto* p =
          static_cast<const unsigned char*>(sqlite3_column_blob(st, 4));
      b.splash.assign(p, p + sqlite3_column_bytes(st, 4));
    }
    b.version = sqlite3_column_int64(st, 5);
    b.updated_ms = sqlite3_column_int64(st, 6);
  }
  sqlite3_finalize(st);
  return b;
}

// 写辅助：确保单行存在后按 COALESCE 只动给出的项，version 单调 +1
namespace {
std::int64_t branding_bump(sqlite3* db, const char* set_col,
                           const std::optional<std::string>& text_val,
                           const std::vector<unsigned char>& blob_val,
                           bool is_blob, std::int64_t ts_ms) {
  const char* ins = "INSERT INTO branding(id) VALUES(1)"
                    " ON CONFLICT(id) DO NOTHING;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, ins, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_step(st);
  }
  sqlite3_finalize(st);
  std::string sql = "UPDATE branding SET ";
  sql += set_col;
  sql += " = ?, version = version + 1, updated_ms = ? WHERE id = 1;";
  st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return -1;
  }
  if (is_blob) {
    if (blob_val.empty()) {
      sqlite3_bind_null(st, 1);
    } else {
      sqlite3_bind_blob(st, 1, blob_val.data(),
                        static_cast<int>(blob_val.size()), SQLITE_TRANSIENT);
    }
  } else if (!text_val.has_value()) {
    sqlite3_bind_null(st, 1); // 不可达（文本面恒有值），防御保持同形
  } else {
    sqlite3_bind_text(st, 1, text_val->c_str(),
                      static_cast<int>(text_val->size()), SQLITE_TRANSIENT);
  }
  sqlite3_bind_int64(st, 2, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return -1;
  std::int64_t version = -1;
  const char* q = "SELECT version FROM branding WHERE id = 1;";
  st = nullptr;
  if (sqlite3_prepare_v2(db, q, -1, &st, nullptr) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) version = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return version;
}
} // namespace

std::int64_t ServerStore::branding_set(
    const std::optional<std::string>& company_name,
    const std::optional<std::string>& accent,
    const std::optional<std::string>& slogan, std::int64_t ts_ms) {
  if (!company_name.has_value() && !accent.has_value() &&
      !slogan.has_value()) {
    return -1; // 无可写项（路由层不会这样调，防御）
  }
  // 单行聚合一次写：给出的项覆盖、缺省项 COALESCE 保旧；version 只 +1
  //（一次 set＝一个版本，客户端变更判据粒度=一次保存）
  const char* ins =
      "INSERT INTO branding(id) VALUES(1) ON CONFLICT(id) DO NOTHING;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, ins, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_step(st);
  }
  sqlite3_finalize(st);
  const char* upd =
      "UPDATE branding SET"
      " company_name = COALESCE(?, company_name),"
      " accent = COALESCE(?, accent),"
      " slogan = COALESCE(?, slogan),"
      " version = version + 1, updated_ms = ? WHERE id = 1;";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, upd, -1, &st, nullptr) != SQLITE_OK) return -1;
  const auto bind_opt = [&](int idx, const std::optional<std::string>& v) {
    if (v.has_value()) {
      sqlite3_bind_text(st, idx, v->c_str(), static_cast<int>(v->size()),
                        SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(st, idx);
    }
  };
  bind_opt(1, company_name);
  bind_opt(2, accent);
  bind_opt(3, slogan);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return -1;
  std::int64_t version = -1;
  const char* q = "SELECT version FROM branding WHERE id = 1;";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) version = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return version;
}

std::int64_t ServerStore::branding_set_logo(
    const std::vector<unsigned char>& png, std::int64_t ts_ms) {
  return branding_bump(db_, "logo", std::nullopt, png, true, ts_ms);
}

std::int64_t ServerStore::branding_set_splash(
    const std::vector<unsigned char>& png, std::int64_t ts_ms) {
  return branding_bump(db_, "splash", std::nullopt, png, true, ts_ms);
}

std::int64_t ServerStore::branding_clear_logo() {
  return branding_bump(db_, "logo", std::nullopt, {}, true, now_ms());
}

std::int64_t ServerStore::branding_clear_splash() {
  return branding_bump(db_, "splash", std::nullopt, {}, true, now_ms());
}

// —— 表情包素材（需求批②）——

std::int64_t ServerStore::emoji_add(const std::string& account,
                                    const std::string& name,
                                    const std::vector<unsigned char>& bytes,
                                    std::int64_t ts_ms) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT INTO emoji_assets(account, name, size, ts_ms,"
                         " bytes) VALUES(?1, ?2, ?3, ?4, ?5)",
                         -1, &st, nullptr) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, static_cast<std::int64_t>(bytes.size()));
  sqlite3_bind_int64(st, 4, ts_ms);
  sqlite3_bind_blob(st, 5, bytes.data(), static_cast<int>(bytes.size()),
                    SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : -1;
}

std::vector<ServerStore::EmojiAsset> ServerStore::emoji_list(
    const std::string& account) {
  std::vector<EmojiAsset> out;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT id, name, size, ts_ms FROM emoji_assets "
                         "WHERE account = ?1 ORDER BY ts_ms DESC, id DESC",
                         -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    EmojiAsset a;
    a.id = sqlite3_column_int64(st, 0);
    a.name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    a.size = sqlite3_column_int64(st, 2);
    a.ts_ms = sqlite3_column_int64(st, 3);
    out.push_back(std::move(a));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::emoji_bytes(std::int64_t id, const std::string& account,
                              std::vector<unsigned char>& out,
                              std::string& name_out) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT name, bytes FROM emoji_assets "
                         "WHERE id = ?1 AND account = ?2",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  const bool found = sqlite3_step(st) == SQLITE_ROW;
  if (found) {
    name_out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const void* blob = sqlite3_column_blob(st, 1);
    const int n = sqlite3_column_bytes(st, 1);
    out.assign(static_cast<const unsigned char*>(blob),
               static_cast<const unsigned char*>(blob) + n);
  }
  sqlite3_finalize(st);
  return found;
}

bool ServerStore::emoji_delete(std::int64_t id, const std::string& account) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "DELETE FROM emoji_assets WHERE id = ?1 AND account = ?2",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

// —— 需求批⑫ 用户头像 ——

bool ServerStore::avatar_size_valid(int size) {
  for (const int s : kAvatarSizes) {
    if (s == size) return true;
  }
  return false;
}

bool ServerStore::avatar_put(const std::string& account, int size,
                             const std::string& mime,
                             const std::vector<unsigned char>& bytes,
                             std::int64_t ts_ms) {
  if (!avatar_size_valid(size) || account.empty() || bytes.empty()) {
    return false;
  }
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "INSERT INTO member_avatars(account, size, mime,"
                         " bytes, ts_ms) VALUES(?1, ?2, ?3, ?4, ?5)"
                         " ON CONFLICT(account, size) DO UPDATE SET"
                         " mime = excluded.mime, bytes = excluded.bytes,"
                         " ts_ms = excluded.ts_ms;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, size);
  sqlite3_bind_text(st, 3, mime.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(st, 4, bytes.data(), static_cast<int>(bytes.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::avatar_bytes(const std::string& account, int size,
                               std::vector<unsigned char>& out,
                               std::string& mime_out) {
  out.clear();
  mime_out.clear();
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT mime, bytes FROM member_avatars"
                         " WHERE account = ?1 AND size = ?2;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, size);
  const bool hit = sqlite3_step(st) == SQLITE_ROW;
  if (hit) {
    mime_out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    const auto* p = static_cast<const unsigned char*>(
        sqlite3_column_blob(st, 1));
    const int n = sqlite3_column_bytes(st, 1);
    out.assign(p, p + n);
  }
  sqlite3_finalize(st);
  return hit;
}

bool ServerStore::avatar_clear(const std::string& account) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "DELETE FROM member_avatars WHERE account = ?1;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::avatar_ver(const std::string& account) const {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT MAX(ts_ms) FROM member_avatars"
                         " WHERE account = ?1;",
                         -1, &st, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::int64_t ver = 0;
  if (sqlite3_step(st) == SQLITE_ROW) {
    ver = sqlite3_column_int64(st, 0); // 无行/全 NULL → 0
  }
  sqlite3_finalize(st);
  return ver;
}

// —— 二期·办公室位置图（设计稿 docs/design/办公室位置图.md）——

std::int64_t ServerStore::seat_upsert(const std::string& floor,
                                      const std::string& label, double x,
                                      double y, std::int64_t ts_ms) {
  static_cast<void>(ts_ms); // 拖拽改位高频不高敏：不落操作留痕（设计口径）
  if (floor.empty() || label.empty()) return 0;
  const char* sql =
      "INSERT INTO office_seats(floor, label, x, y) VALUES(?,?,?,?)"
      " ON CONFLICT(floor, label) DO UPDATE SET x = excluded.x,"
      " y = excluded.y;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, floor.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, label.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(st, 3, x);
  sqlite3_bind_double(st, 4, y);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  if (!ok) return 0;
  const char* q =
      "SELECT id FROM office_seats WHERE floor = ? AND label = ?;";
  st = nullptr;
  std::int64_t id = 0;
  if (sqlite3_prepare_v2(db_, q, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, floor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, label.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return id;
}

bool ServerStore::seat_delete(std::int64_t id) {
  const char* sql = "DELETE FROM office_seats WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::seat_bind(std::int64_t id, const std::string& account,
                            std::int64_t ts_ms) {
  if (!account.empty() && !find_account(account)) return false; // 幽灵拒
  const char* sql = "UPDATE office_seats SET account = ? WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE &&
                  sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::SeatRow> ServerStore::seats_on_floor(
    const std::string& floor) {
  std::vector<SeatRow> out;
  const char* sql =
      "SELECT id, floor, label, x, y, account FROM office_seats"
      " WHERE floor = ? ORDER BY label;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, floor.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    SeatRow t;
    t.id = sqlite3_column_int64(st, 0);
    t.floor = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    t.label = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.x = sqlite3_column_double(st, 3);
    t.y = sqlite3_column_double(st, 4);
    t.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    out.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  return out;
}

std::optional<ServerStore::SeatRow> ServerStore::seat_of(
    const std::string& account) {
  const char* sql =
      "SELECT id, floor, label, x, y, account FROM office_seats"
      " WHERE account = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<SeatRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    SeatRow t;
    t.id = sqlite3_column_int64(st, 0);
    t.floor = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    t.label = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.x = sqlite3_column_double(st, 3);
    t.y = sqlite3_column_double(st, 4);
    t.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    out = std::move(t);
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
