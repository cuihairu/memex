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

// —— 二期群工具三件（原生互动，不走 R25 外部工具代理）——

std::int64_t ServerStore::poll_create(std::uint64_t group_id,
                                      const std::string& topic,
                                      const std::vector<std::string>& options,
                                      std::int64_t deadline_ms,
                                      const std::string& by,
                                      std::int64_t ts_ms, bool anonymous,
                                      bool multi) {
  if (!group_info(group_id).has_value()) return 0;
  if (topic.empty() || options.size() < 2 || options.size() > 10) return 0;
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& o : options) arr.push_back(o);
  const char* sql =
      "INSERT INTO group_polls(group_id, topic, options_json, deadline_ms,"
      " closed, anonymous, multi, created_by, created_ms)"
      " VALUES(?, ?, ?, ?, 0, ?, ?, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, topic.c_str(), -1, SQLITE_TRANSIENT);
  const std::string arr_s = arr.dump();
  sqlite3_bind_text(st, 3, arr_s.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, deadline_ms);
  sqlite3_bind_int64(st, 5, anonymous ? 1 : 0);
  sqlite3_bind_int64(st, 6, multi ? 1 : 0);
  sqlite3_bind_text(st, 7, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 8, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupPoll> ServerStore::poll_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, topic, options_json, deadline_ms, closed,"
      " anonymous, multi, created_by, created_ms"
      " FROM group_polls WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupPoll> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupPoll p;
    p.id = sqlite3_column_int64(st, 0);
    p.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    p.topic = t ? t : "";
    const char* oj = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    if (oj) {
      const auto arr = nlohmann::json::parse(oj, nullptr, false);
      if (arr.is_array()) {
        for (const auto& e : arr) {
          if (e.is_string()) p.options.push_back(e.get<std::string>());
        }
      }
    }
    p.deadline_ms = sqlite3_column_int64(st, 4);
    p.closed = sqlite3_column_int64(st, 5) != 0;
    p.anonymous = sqlite3_column_int64(st, 6) != 0;
    p.multi = sqlite3_column_int64(st, 7) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
    p.created_by = cb ? cb : "";
    p.created_ms = sqlite3_column_int64(st, 9);
    out = std::move(p);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupPoll> ServerStore::polls_list(
    std::uint64_t group_id) {
  std::vector<GroupPoll> out;
  const char* sql =
      "SELECT id, group_id, topic, options_json, deadline_ms, closed,"
      " anonymous, multi, created_by, created_ms"
      " FROM group_polls WHERE group_id = ?"
      " ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupPoll p;
    p.id = sqlite3_column_int64(st, 0);
    p.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    p.topic = t ? t : "";
    const char* oj = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    if (oj) {
      const auto arr = nlohmann::json::parse(oj, nullptr, false);
      if (arr.is_array()) {
        for (const auto& e : arr) {
          if (e.is_string()) p.options.push_back(e.get<std::string>());
        }
      }
    }
    p.deadline_ms = sqlite3_column_int64(st, 4);
    p.closed = sqlite3_column_int64(st, 5) != 0;
    p.anonymous = sqlite3_column_int64(st, 6) != 0;
    p.multi = sqlite3_column_int64(st, 7) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 8));
    p.created_by = cb ? cb : "";
    p.created_ms = sqlite3_column_int64(st, 9);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::poll_vote(std::int64_t poll_id, const std::string& account,
                            int choice, std::int64_t ts_ms) {
  const auto p = poll_by_id(poll_id);
  // 到点视同已关闭（与 closed 同语义；store 层兜底——直接调 store 的
  // 路径同样被挡，惰性判定不回写）
  if (!p.has_value() || p->closed ||
      (p->deadline_ms > 0 && ts_ms >= p->deadline_ms)) {
    return false;
  }
  const int n = static_cast<int>(p->options.size());
  if (p->multi) {
    // 多选：choice=位集（bit i=选 i+1 号）——非零且每位都在界内
    if (choice < 1 || choice >= (1 << n)) return false;
  } else {
    if (choice < 1 || choice > n) return false;
  }
  const char* sql =
      "INSERT INTO group_poll_votes(poll_id, account, choice, ts_ms)"
      " VALUES(?, ?, ?, ?)"
      " ON CONFLICT(poll_id, account) DO UPDATE SET choice = excluded.choice,"
      " ts_ms = excluded.ts_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, poll_id);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, choice);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupPollVote> ServerStore::poll_votes(
    std::int64_t poll_id) {
  std::vector<GroupPollVote> out;
  const char* sql =
      "SELECT account, choice, ts_ms FROM group_poll_votes"
      " WHERE poll_id = ? ORDER BY ts_ms ASC, account ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, poll_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupPollVote v;
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    v.account = a ? a : "";
    v.choice = sqlite3_column_int(st, 1);
    v.ts_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(v));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::poll_close(std::int64_t poll_id) {
  const auto p = poll_by_id(poll_id);
  if (!p.has_value()) return false;
  if (p->closed) return true; // 幂等收口
  const char* sql = "UPDATE group_polls SET closed = 1 WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, poll_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::chain_create(std::uint64_t group_id,
                                       const std::string& title,
                                       const std::string& format_hint,
                                       const std::string& by,
                                       std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return 0;
  if (title.empty()) return 0;
  const char* sql =
      "INSERT INTO group_chains(group_id, title, format_hint, closed,"
      " created_by, created_ms) VALUES(?, ?, ?, 0, ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, format_hint.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 4, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 5, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::optional<ServerStore::GroupChain> ServerStore::chain_by_id(
    std::int64_t id) {
  const char* sql =
      "SELECT id, group_id, title, format_hint, closed, created_by,"
      " created_ms FROM group_chains WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_int64(st, 1, id);
  std::optional<GroupChain> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    GroupChain c;
    c.id = sqlite3_column_int64(st, 0);
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    c.title = t ? t : "";
    const char* fh = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.format_hint = fh ? fh : "";
    c.closed = sqlite3_column_int64(st, 4) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    c.created_by = cb ? cb : "";
    c.created_ms = sqlite3_column_int64(st, 6);
    out = std::move(c);
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::GroupChain> ServerStore::chains_list(
    std::uint64_t group_id) {
  std::vector<GroupChain> out;
  const char* sql =
      "SELECT id, group_id, title, format_hint, closed, created_by,"
      " created_ms FROM group_chains WHERE group_id = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupChain c;
    c.id = sqlite3_column_int64(st, 0);
    c.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    c.title = t ? t : "";
    const char* fh = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    c.format_hint = fh ? fh : "";
    c.closed = sqlite3_column_int64(st, 4) != 0;
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    c.created_by = cb ? cb : "";
    c.created_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(c));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::chain_join(std::int64_t chain_id,
                             const std::string& account,
                             const std::string& content, std::int64_t ts_ms) {
  const auto c = chain_by_id(chain_id);
  if (!c.has_value() || c->closed) return false;
  if (content.empty()) return false;
  const char* sql =
      "INSERT INTO group_chain_entries(chain_id, account, content, ts_ms)"
      " VALUES(?, ?, ?, ?)"
      " ON CONFLICT(chain_id, account) DO UPDATE SET content ="
      " excluded.content, ts_ms = excluded.ts_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, chain_id);
  sqlite3_bind_text(st, 2, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, content.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::GroupChainEntry> ServerStore::chain_entries(
    std::int64_t chain_id) {
  std::vector<GroupChainEntry> out;
  const char* sql =
      "SELECT account, content, ts_ms FROM group_chain_entries"
      " WHERE chain_id = ? ORDER BY ts_ms ASC, account ASC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, chain_id);
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupChainEntry e;
    const char* a = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    e.account = a ? a : "";
    const char* c = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    e.content = c ? c : "";
    e.ts_ms = sqlite3_column_int64(st, 2);
    out.push_back(std::move(e));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::chain_close(std::int64_t chain_id) {
  const auto c = chain_by_id(chain_id);
  if (!c.has_value()) return false;
  if (c->closed) return true; // 幂等收口
  const char* sql = "UPDATE group_chains SET closed = 1 WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_int64(st, 1, chain_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

std::int64_t ServerStore::gtask_create(std::uint64_t group_id,
                                       const std::string& title,
                                       const std::string& assignee,
                                       std::int64_t due_ms,
                                       const std::string& by,
                                       std::int64_t ts_ms) {
  if (!group_info(group_id).has_value()) return 0;
  if (title.empty()) return 0;
  if (!assignee.empty() && !is_group_member(group_id, assignee)) return 0;
  const char* sql =
      "INSERT INTO group_tasks(group_id, title, assignee, due_ms, status,"
      " created_by, created_ms) VALUES(?, ?, ?, ?, 'todo', ?, ?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, assignee.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, due_ms);
  sqlite3_bind_text(st, 5, by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 6, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  const std::int64_t id = ok ? sqlite3_last_insert_rowid(db_) : 0;
  sqlite3_finalize(st);
  return id;
}

std::vector<ServerStore::GroupTask> ServerStore::gtasks_list(
    std::uint64_t group_id) {
  std::vector<GroupTask> out;
  const char* sql =
      "SELECT id, group_id, title, assignee, due_ms, claimed_ms, status,"
      " created_by, created_ms, done_by, done_ms FROM group_tasks"
      " WHERE group_id = ? ORDER BY id DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int64(st, 1, static_cast<sqlite3_int64>(group_id));
  while (sqlite3_step(st) == SQLITE_ROW) {
    GroupTask t;
    t.id = sqlite3_column_int64(st, 0);
    t.group_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
    const char* ti = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    t.title = ti ? ti : "";
    const char* as = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    t.assignee = as ? as : "";
    t.due_ms = sqlite3_column_int64(st, 4);
    t.claimed_ms = sqlite3_column_int64(st, 5);
    const char* stt = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    t.status = stt ? stt : "todo";
    const char* cb = reinterpret_cast<const char*>(sqlite3_column_text(st, 7));
    t.created_by = cb ? cb : "";
    t.created_ms = sqlite3_column_int64(st, 8);
    const char* db2 = reinterpret_cast<const char*>(sqlite3_column_text(st, 9));
    t.done_by = db2 ? db2 : "";
    t.done_ms = sqlite3_column_int64(st, 10);
    out.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::gtask_claim(std::int64_t id, const std::string& account,
                              std::int64_t ts_ms) {
  // 认领＝todo 且无人认领；占位原子落（status/assignee 同行判）
  const char* sql =
      "UPDATE group_tasks SET assignee = ?, claimed_ms = ? WHERE id = ?"
      " AND status = 'todo' AND assignee = '';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_int64(st, 3, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::gtask_done(std::int64_t id, const std::string& done_by,
                             std::int64_t ts_ms) {
  // 终态留痕：todo→done 一次性迁移，done_by/done_ms 落行不删行
  const char* sql =
      "UPDATE group_tasks SET status = 'done', done_by = ?, done_ms = ?"
      " WHERE id = ? AND status = 'todo';";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, done_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, ts_ms);
  sqlite3_bind_int64(st, 3, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) == 1;
  sqlite3_finalize(st);
  return ok;
}


} // namespace memex::server
