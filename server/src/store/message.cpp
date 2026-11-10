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

// 消息检索：账号为空=全部；非空=该账号收发两侧＋其所在群的群消息都命中
//（管理员检索面；T4.1 起群消息 to="group:<群号>" 联入成员检索面）
std::vector<ArchivedMessage> ServerStore::messages(const std::string& account,
                                                   int limit) {
  MessageSearch q;
  q.account = account;
  q.limit = limit;
  return search_messages(q);
}

// T3.2 条件检索：账号（收发双侧）／时间窗（含端点）／关键词（多词子串 AND + 相关度排序）
std::vector<ArchivedMessage> ServerStore::search_messages(
    const MessageSearch& q) {
  std::vector<ArchivedMessage> out;

  // 关键词分词：ASCII 空白 + 半角逗号 + 全角逗号「，」(EF BC 8C)。
  // 每词独立子串命中并 AND 组合（全部命中才出）；空词丢弃。
  std::vector<std::string> toks;
  {
    std::string cur;
    const auto flush = [&]() {
      if (!cur.empty()) {
        toks.push_back(cur);
        cur.clear();
      }
    };
    for (std::size_t i = 0; i < q.keyword.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(q.keyword[i]);
      if (c == 0xEF && i + 2 < q.keyword.size() &&
          static_cast<unsigned char>(q.keyword[i + 1]) == 0xBC &&
          static_cast<unsigned char>(q.keyword[i + 2]) == 0x8C) {
        flush();
        i += 2;
      } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                 c == '\f' || c == '\v' || c == ',') {
        flush();
      } else {
        cur += q.keyword[i];
      }
    }
    flush();
    // ASCII 转小写（SQLite lower() 对中文为恒等，中文即按字符直接匹配）
    for (auto& t : toks)
      for (auto& ch : t)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
  }
  const bool has_kw = !toks.empty();

  // 相关度 = Σ(每词：命中次数×1000 + 首命中位置提前量（越靠前越高，第 1000 位起为 0))。
  // 占位符出现次序：SELECT 相关度表达式（每词 3 个）→ WHERE 账号 → WHERE peer
  // → WHERE LIKE（每词 1 个）→ 时间窗 → LIMIT。
  std::string sql =
      "SELECT msg_id, from_account, to_account, type, text, ts_ms, recall";
  if (has_kw) {
    sql += ", (";
    for (std::size_t i = 0; i < toks.size(); ++i) {
      if (i > 0) sql += " + ";
      sql += "((length(lower(text)) - length(replace(lower(text), ?, '')))"
             " / length(?) * 1000 + max(0, 1000 - instr(lower(text), ?)))";
    }
    sql += ") AS score";
  }
  // row_id＝messages 自增主键（keyset 游标，群组-2 导出分页）；附加在 score
  // 之后，既有消费方按索引取 score(7) 不受影响。
  sql += ", id AS row_id FROM messages WHERE 1=1";
  if (!q.account.empty()) {
    // 收发双侧 ＋ 该账号所在群的群消息（to="group:<群号>"）
    sql += " AND (from_account = ? OR to_account = ? OR to_account IN"
           " (SELECT 'group:' || group_id FROM group_members"
           "  WHERE account = ?))";
  }
  // 会话维度（需求批⑧）：群键限定目标群（成员资格由 account 子查询把关）；
  // 单聊对端限定双向会话（from/to 两侧成对匹配——与 account 条件叠加后
  // 恰为「我与该对端的完整双向会话」）。以 account 视角为前提（无主体
  // 的 peer 查询无意义，忽略）。
  const bool peer_is_group =
      q.peer.rfind("group:", 0) == 0;
  if (!q.peer.empty() && !q.account.empty()) {
    if (peer_is_group) {
      sql += " AND to_account = ?";
    } else {
      sql += " AND ((from_account = ? AND to_account = ?) OR"
             " (from_account = ? AND to_account = ?))";
    }
  }
  // 关键词子串匹配：%／_／转义符先转义，避免用户输入被当通配符；
  // lower() 双侧统一小写 → 英文大小写不敏感、中文按字符直接匹配。
  std::vector<std::string> pats;
  if (has_kw) {
    pats.reserve(toks.size());
    for (const auto& t : toks) {
      std::string pat;
      pat.reserve(t.size() + 2);
      pat += '%';
      for (const char c : t) {
        if (c == '%' || c == '_' || c == '\\') pat += '\\';
        pat += c;
      }
      pat += '%';
      pats.push_back(std::move(pat));
      sql += " AND lower(text) LIKE ? ESCAPE '\\'";
    }
  }
  if (q.since_ms > 0) sql += " AND ts_ms >= ?";
  if (q.until_ms > 0) sql += " AND ts_ms <= ?";
  // keyset 游标（群组-2）：升序分页续传——AND id > ?（主键单调≈时序，无重无漏）。
  if (q.asc && q.after_id > 0) sql += " AND id > ?";
  if (has_kw) {
    sql += " ORDER BY score DESC, ts_ms DESC, id DESC LIMIT ?;";
  } else if (q.asc) {
    sql += " ORDER BY id ASC LIMIT ?;";
  } else {
    sql += " ORDER BY id DESC LIMIT ?;";
  }

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    return out;
  }
  int idx = 1;
  // SELECT 里的相关度占位符（每词 3 个，先于 WHERE 子句出现）
  for (const auto& t : toks) {
    sqlite3_bind_text(st, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, t.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!q.account.empty()) {
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!q.peer.empty() && !q.account.empty()) {
    if (peer_is_group) {
      sqlite3_bind_text(st, idx++, q.peer.c_str(), -1, SQLITE_TRANSIENT);
    } else {
      // (from=peer AND to=account) OR (from=account AND to=peer)
      sqlite3_bind_text(st, idx++, q.peer.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, idx++, q.account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, idx++, q.peer.c_str(), -1, SQLITE_TRANSIENT);
    }
  }
  for (const auto& p : pats) {
    sqlite3_bind_text(st, idx++, p.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (q.since_ms > 0) sqlite3_bind_int64(st, idx++, q.since_ms);
  if (q.until_ms > 0) sqlite3_bind_int64(st, idx++, q.until_ms);
  if (q.asc && q.after_id > 0) sqlite3_bind_int64(st, idx++, q.after_id);
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
    // row_id 附加在 score 之后：无关键词在列 7、有关键词在列 8（score 仍占 7）。
    m.row_id = sqlite3_column_int64(st, has_kw ? 8 : 7);
    if (has_kw) m.score = sqlite3_column_int64(st, 7);
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

// 消息撤回：平台-4 起走 recalled 事件（事件与投影同事务落），
// 并保 recall_events 对账行（既有对账口径不变）
bool ServerStore::recall_message(const std::string& msg_id,
                                 const std::string& by_account,
                                 std::int64_t ts_ms) {
  if (message_from(msg_id).empty()) return false; // 消息不存在
  if (!append_message_event(msg_id, "recalled", by_account, "", ts_ms))
    return false;
  record_recall_event(msg_id, by_account, ts_ms); // 对账表尽力（主证在事件表）
  return true;
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

std::optional<ArchivedMessage> ServerStore::message_by_id(
    const std::string& msg_id) {
  const char* sql =
      "SELECT msg_id, from_account, to_account, type, text, ts_ms, recall "
      "FROM messages WHERE msg_id = ?;";
  sqlite3_stmt* st = nullptr;
  std::optional<ArchivedMessage> out;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    ArchivedMessage m;
    m.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.from_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.to_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    m.type = sqlite3_column_int(st, 3);
    m.text = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.ts_ms = sqlite3_column_int64(st, 5);
    m.recalled = sqlite3_column_int(st, 6) != 0;
    out = m;
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

// —— 管理台归档检索（desktop-search）三面 ——

std::optional<ServerStore::RecallEventRow> ServerStore::last_recall_event(
    const std::string& msg_id) {
  // 对账副本在前（含撤回人＋时刻齐备）；缺行回落事件表（平台-4 主证）。
  {
    const char* sql = "SELECT by_account, ts_ms FROM recall_events"
                      " WHERE msg_id = ? ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW) {
        RecallEventRow row;
        row.msg_id = msg_id;
        row.by_account =
            reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        row.ts_ms = sqlite3_column_int64(st, 1);
        sqlite3_finalize(st);
        return row;
      }
      sqlite3_finalize(st);
    }
  }
  const char* sql = "SELECT by_account, ts_ms FROM message_events"
                    " WHERE msg_id = ? AND event = 'recalled'"
                    " ORDER BY id DESC LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK)
    return std::nullopt;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<RecallEventRow> out;
  if (sqlite3_step(st) == SQLITE_ROW) {
    RecallEventRow row;
    row.msg_id = msg_id;
    row.by_account =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    row.ts_ms = sqlite3_column_int64(st, 1);
    out = row;
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ArchivedMessage> ServerStore::context_around(
    const std::string& msg_id, int span) {
  std::vector<ArchivedMessage> out;
  if (span < 1) span = 1;
  if (span > 50) span = 50;
  // 命中行定位：id（游标）＋会话键（群=to 群键；单聊=from/to 成对）
  sqlite3_stmt* st = nullptr;
  const char* probe_sql =
      "SELECT id, from_account, to_account FROM messages WHERE msg_id = ?;";
  std::int64_t hit_id = 0;
  std::string side_a, side_b;
  if (sqlite3_prepare_v2(db_, probe_sql, -1, &st, nullptr) != SQLITE_OK)
    return out;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st) == SQLITE_ROW) {
    hit_id = sqlite3_column_int64(st, 0);
    side_a = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    side_b = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
  }
  sqlite3_finalize(st);
  if (hit_id == 0) return out;

  // 会话键 WHERE 片段：群消息（to="group:N"）按群键；单聊按双方成对
  //（与 search_messages 的 peer 口径同构——上下文即「该会话」的窗口）
  std::string where;
  std::vector<std::string> params;
  if (side_b.rfind("group:", 0) == 0) {
    where = "to_account = ?";
    params.push_back(side_b);
  } else {
    where = "((from_account = ? AND to_account = ?) OR"
            " (from_account = ? AND to_account = ?))";
    params.push_back(side_a);
    params.push_back(side_b);
    params.push_back(side_b);
    params.push_back(side_a);
  }
  const std::string sql =
      "SELECT msg_id, from_account, to_account, type, text, ts_ms, recall, id"
      " FROM messages WHERE " +
      where + " AND id BETWEEN ? AND ? ORDER BY id ASC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  int idx = 1;
  for (const auto& p : params) {
    sqlite3_bind_text(st, idx++, p.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int64(st, idx++, hit_id - span);
  sqlite3_bind_int64(st, idx++, hit_id + span);
  sqlite3_bind_int(st, idx, span * 2 + 1);
  while (sqlite3_step(st) == SQLITE_ROW) {
    ArchivedMessage m;
    m.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    m.from_account =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.to_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    m.type = sqlite3_column_int(st, 3);
    const char* text_ptr =
        reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    m.text = text_ptr ? text_ptr : "";
    m.ts_ms = sqlite3_column_int64(st, 5);
    m.recalled = sqlite3_column_int(st, 6) != 0;
    m.row_id = sqlite3_column_int64(st, 7);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
