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
