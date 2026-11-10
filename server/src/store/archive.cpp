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

// T2.3 消息归档：协作态消息全量落库
bool ServerStore::store_message(const std::string& msg_id, const std::string& from_account,
                                const std::string& to_account, int type,
                                const std::string& text, std::int64_t ts_ms) {
  // 平台-4：归档走 created 事件（payload 带全量字段，事件与投影同事务落）
  nlohmann::json p;
  p["from"] = from_account;
  p["to"] = to_account;
  p["type"] = type;
  p["text"] = text;
  p["ts_ms"] = ts_ms;
  return append_message_event(msg_id, "created", from_account, p.dump(),
                              ts_ms > 0 ? ts_ms : now_ms());
}

// —— 平台-4 归档事件溯源 ——

bool ServerStore::append_message_event(const std::string& msg_id,
                                       const std::string& event,
                                       const std::string& by_account,
                                       const std::string& payload,
                                       std::int64_t ts_ms) {
  if (msg_id.empty() || event.empty()) return false;
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT msg_evt;", nullptr, nullptr, &tx) !=
      SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  if (event == "created") {
    // created 物化归档行：payload 带全量字段；重投去重（已有行不重放）
    nlohmann::json p = nlohmann::json::parse(payload, nullptr, false);
    if (p.is_discarded() || !p.contains("from") || !p.contains("to")) {
      sqlite3_exec(db_, "ROLLBACK TO msg_evt;", nullptr, nullptr, &tx);
      sqlite3_exec(db_, "RELEASE msg_evt;", nullptr, nullptr, &tx);
      sqlite3_free(tx);
      return false;
    }
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT OR IGNORE INTO messages(msg_id, from_account, to_account,"
             " type, text, ts_ms) VALUES(?,?,?,?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2,
                        p.value("from", std::string{}).c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 3,
                        p.value("to", std::string{}).c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_int(i, 4, p.value("type", 0));
      sqlite3_bind_text(i, 5,
                        p.value("text", std::string{}).c_str(), -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_int64(i, 6, p.value("ts_ms", std::int64_t{0}));
      ok = sqlite3_step(i) == SQLITE_DONE;
      if (ok) ok = sqlite3_changes(db_) > 0; // 已存在=重投，不记重复事件
    }
    sqlite3_finalize(i);
  } else if (event == "recalled") {
    // 投影跟随事件：recall=1（正文不动——留痕纪律）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(db_,
                            "UPDATE messages SET recall = 1 WHERE msg_id = ?;",
                            -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(u, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    }
    sqlite3_finalize(u);
  } else if (event == "edited") {
    // 投影跟随事件：正文替换（原 created payload 里原文永在——可审计）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(db_,
                            "UPDATE messages SET text = ? WHERE msg_id = ?;",
                            -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(u, 1, payload.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(u, 2, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    }
    sqlite3_finalize(u);
  } else if (event == "purged") {
    // 平台-5 留存清除：投影物理移除；事件表留痕（重建跳过——清除可重现）
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE msg_id = ?;",
                            -1, &d, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    }
    sqlite3_finalize(d);
  }
  // delivered/read：只记事件，不动投影
  if (ok) {
    sqlite3_stmt* e = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO message_events(msg_id, event, by_account, payload,"
             " ts_ms) VALUES(?,?,?,?,?);",
             -1, &e, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(e, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(e, 2, event.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(e, 3, by_account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(e, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(e, 5, ts_ms);
      ok = sqlite3_step(e) == SQLITE_DONE;
    }
    sqlite3_finalize(e);
  }
  // 453 归档完整性账：事件落库满块即封（同一事务——封账失败回滚整笔，
  // 账本与事件流永不脱节；开尾不落行，等下一条事件或 CLI seal 收尾）。
  // 免查缓存省每事件一条 SELECT（10 万级语料敏感，见 search_fuzzy 预算）
  if (ok) {
    const std::int64_t row_id = sqlite3_last_insert_rowid(db_);
    if (archive_last_sealed_cache_ < 0) {
      sqlite3_stmt* s = nullptr;
      if (sqlite3_prepare_v2(db_,
                             "SELECT COALESCE(MAX(last_event_id), 0)"
                             " FROM archive_chain;",
                             -1, &s, nullptr) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW)
          archive_last_sealed_cache_ = sqlite3_column_int64(s, 0);
      }
      sqlite3_finalize(s);
      if (archive_last_sealed_cache_ < 0) archive_last_sealed_cache_ = 0;
    }
    if (row_id - archive_last_sealed_cache_ >=
        ServerStore::kArchiveChainBlockEvents) {
      archive_last_sealed_cache_ = -1; // 封后失忆——以库为准（回滚安全）
      if (!archive_chain_seal()) ok = false;
    }
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE msg_evt;", nullptr, nullptr, &tx) ==
         SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO msg_evt;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE msg_evt;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
}

bool ServerStore::delivered_recorded(const std::string& msg_id,
                                     const std::string& by_account) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_,
                         "SELECT 1 FROM message_events"
                         " WHERE msg_id = ? AND event = 'delivered'"
                         "   AND by_account = ? LIMIT 1",
                         -1, &st, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, by_account.c_str(), -1, SQLITE_TRANSIENT);
  const bool found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return found;
}

std::vector<ServerStore::MessageEvent> ServerStore::message_events(
    const std::string& msg_id) {
  std::vector<MessageEvent> out;
  std::string sql =
      "SELECT id, msg_id, event, by_account, payload, ts_ms"
      " FROM message_events";
  if (!msg_id.empty()) sql += " WHERE msg_id = ?";
  sql += " ORDER BY id;"; // 发生序（append-only 单调）
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!msg_id.empty())
    sqlite3_bind_text(st, 1, msg_id.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    MessageEvent e;
    e.id = sqlite3_column_int64(st, 0);
    e.msg_id = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    e.event = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    e.by_account = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    e.payload = reinterpret_cast<const char*>(sqlite3_column_text(st, 4));
    e.ts_ms = sqlite3_column_int64(st, 5);
    out.push_back(std::move(e));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ArchivedMessage> ServerStore::rebuild_messages_from_events() {
  // 重放事件重建当前态：created 建（按首现序）、recalled 置标记、
  // edited 替正文、purged 移除（留存清除可重现）；其余事件不变形状
  std::vector<ArchivedMessage> out;
  std::map<std::string, std::size_t> index; // msg_id → out 下标
  std::set<std::string> purged;
  for (const auto& e : message_events("")) {
    if (e.event == "created") {
      nlohmann::json p = nlohmann::json::parse(e.payload, nullptr, false);
      if (p.is_discarded()) continue;
      ArchivedMessage m;
      m.msg_id = e.msg_id;
      m.from_account = p.value("from", std::string{});
      m.to_account = p.value("to", std::string{});
      m.type = p.value("type", 0);
      m.text = p.value("text", std::string{});
      m.ts_ms = p.value("ts_ms", std::int64_t{0});
      index[e.msg_id] = out.size();
      out.push_back(std::move(m));
    } else if (e.event == "recalled") {
      const auto it = index.find(e.msg_id);
      if (it != index.end()) out[it->second].recalled = true;
    } else if (e.event == "edited") {
      const auto it = index.find(e.msg_id);
      if (it != index.end() && !e.payload.empty()) out[it->second].text = e.payload;
    } else if (e.event == "purged") {
      purged.insert(e.msg_id);
    }
  }
  if (!purged.empty()) {
    std::vector<ArchivedMessage> kept;
    kept.reserve(out.size());
    for (auto& m : out) {
      if (!purged.count(m.msg_id)) kept.push_back(std::move(m));
    }
    out = std::move(kept);
  }
  return out;
}

// —— 归档完整性账（评审四·远期加固）：分块封账＋链式摘要 ——

namespace {

// 单事件摘要：长度前缀框架（payload 可含任意字节，防拼接歧义）→ SHA-256。
std::string archive_event_digest(std::int64_t id, const std::string& msg_id,
                                 const std::string& event,
                                 const std::string& by_account,
                                 const std::string& payload,
                                 std::int64_t ts_ms) {
  const auto field = [](const std::string& v) {
    return std::to_string(v.size()) + ":" + v;
  };
  std::string buf;
  buf += field(std::to_string(id));
  buf += field(msg_id);
  buf += field(event);
  buf += field(by_account);
  buf += field(payload);
  buf += field(std::to_string(ts_ms));
  return sha256_hex(buf);
}

constexpr char kArchiveChainGenesis[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

// 自上块之后的事件折叠链式摘要（〔first,last〕id 连续段，调用方保证）。
// 封账与校验共用同一折叠口径——口径即契约，改动需同步重建账本。
std::string archive_fold_range(sqlite3* db, std::int64_t first,
                               std::int64_t last,
                               const std::string& prev_hash,
                               std::int64_t* count_out) {
  std::string h = prev_hash;
  std::int64_t count = 0;
  const char* sql =
      "SELECT id, msg_id, event, by_account, payload, ts_ms"
      " FROM message_events WHERE id BETWEEN ? AND ? ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, first);
    sqlite3_bind_int64(st, 2, last);
    while (sqlite3_step(st) == SQLITE_ROW) {
      const std::int64_t id = sqlite3_column_int64(st, 0);
      const auto txt = [&st](int col) {
        const char* p =
            reinterpret_cast<const char*>(sqlite3_column_text(st, col));
        return p ? std::string(p) : std::string{};
      };
      h = sha256_hex(h + archive_event_digest(id, txt(1), txt(2), txt(3),
                                              txt(4),
                                              sqlite3_column_int64(st, 5)));
      ++count;
    }
  }
  sqlite3_finalize(st);
  if (count_out) *count_out = count;
  return h;
}

} // namespace

// 封账：自上块之后的事件封成一块。空尾（无新事件）返回 true。
// 在 append_message_event 的 SAVEPOINT 内被调（满块自动封）＝事件与
// 账本行同事务落——不存在「事件在、账未封」的中间态窗口。
bool ServerStore::archive_chain_seal() {
  std::int64_t last_sealed = 0;
  {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT COALESCE(MAX(last_event_id),0)"
                           " FROM archive_chain;",
                           -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    if (sqlite3_step(st) == SQLITE_ROW) last_sealed = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  std::int64_t max_id = 0;
  {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT COALESCE(MAX(id),0) FROM message_events;",
                           -1, &st, nullptr) != SQLITE_OK) {
      return false;
    }
    if (sqlite3_step(st) == SQLITE_ROW) max_id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  if (max_id <= last_sealed) return true; // 空尾＝无事可封
  std::string prev_hash = kArchiveChainGenesis;
  {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT chain_hash FROM archive_chain"
                           " ORDER BY id DESC LIMIT 1;",
                           -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        if (p) prev_hash = p;
      }
      sqlite3_finalize(st);
    }
  }
  std::int64_t count = 0;
  const std::string h = archive_fold_range(db_, last_sealed + 1, max_id,
                                           prev_hash, &count);
  if (count <= 0) return false; // 折叠出 0 条＝事件流缺口（id 段断裂）
  sqlite3_stmt* st = nullptr;
  bool ok =
      sqlite3_prepare_v2(db_,
                         "INSERT INTO archive_chain(first_event_id,"
                         " last_event_id, event_count, prev_hash, chain_hash,"
                         " sealed_ms) VALUES(?,?,?,?,?,?);",
                         -1, &st, nullptr) == SQLITE_OK;
  if (ok) {
    sqlite3_bind_int64(st, 1, last_sealed + 1);
    sqlite3_bind_int64(st, 2, max_id);
    sqlite3_bind_int64(st, 3, count);
    sqlite3_bind_text(st, 4, prev_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, h.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, now_ms());
    ok = sqlite3_step(st) == SQLITE_DONE;
  }
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::ArchiveChainBlock> ServerStore::archive_chain_list() {
  std::vector<ArchiveChainBlock> out;
  const char* sql =
      "SELECT id, first_event_id, last_event_id, event_count, prev_hash,"
      " chain_hash, sealed_ms FROM archive_chain ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    ArchiveChainBlock b;
    b.id = sqlite3_column_int64(st, 0);
    b.first_event_id = sqlite3_column_int64(st, 1);
    b.last_event_id = sqlite3_column_int64(st, 2);
    b.event_count = sqlite3_column_int64(st, 3);
    const auto txt = [&st](int col) {
      const char* p = reinterpret_cast<const char*>(sqlite3_column_text(st, col));
      return p ? std::string(p) : std::string{};
    };
    b.prev_hash = txt(4);
    b.chain_hash = txt(5);
    b.sealed_ms = sqlite3_column_int64(st, 6);
    out.push_back(std::move(b));
  }
  sqlite3_finalize(st);
  return out;
}

ServerStore::ArchiveChainVerdict ServerStore::archive_chain_verify() {
  ArchiveChainVerdict v;
  const auto blocks = archive_chain_list();
  v.blocks = static_cast<std::int64_t>(blocks.size());
  std::int64_t last_sealed = 0;
  {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT COALESCE(MAX(id),0) FROM message_events;",
                           -1, &st, nullptr) != SQLITE_OK) {
      v.broken_at = "事件流不可读";
      return v;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
      last_sealed = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
  }
  v.events = last_sealed;
  // 无账本＝未封状态（空账校验恒真——旧库升级不破口径）
  if (blocks.empty()) {
    v.ok = true;
    v.open_tail = last_sealed;
    return v;
  }
  // 逐块对账：块边界连续（first==上块 last+1，首块从 1 起）＋块内计数
  // 一致＋链式摘要复算一致＋prev 链接一致。
  std::int64_t expect_first = 1;
  std::string prev_hash = kArchiveChainGenesis;
  for (const auto& b : blocks) {
    if (b.first_event_id != expect_first || b.prev_hash != prev_hash) {
      v.broken_at = "块 " + std::to_string(b.id) + " 边界/链接断裂";
      return v;
    }
    std::int64_t count = 0;
    const std::string h = archive_fold_range(db_, b.first_event_id,
                                             b.last_event_id, b.prev_hash,
                                             &count);
    if (count != b.event_count) {
      v.broken_at = "块 " + std::to_string(b.id) + " 计数不符（账 " +
                    std::to_string(b.event_count) + " 实 " +
                    std::to_string(count) + "）";
      return v;
    }
    if (h != b.chain_hash) {
      v.broken_at = "块 " + std::to_string(b.id) + " 摘要不符（事件行被改动）";
      return v;
    }
    expect_first = b.last_event_id + 1;
    prev_hash = b.chain_hash;
  }
  // 账本行自身完整性：任一行被改（如 count/边界）已在上面显形；
  // 未封尾如实报告（正常态，待 seal 收尾）。
  if (expect_first <= last_sealed) {
    v.open_tail = last_sealed - expect_first + 1;
  }
  v.ok = true;
  return v;
}

// —— 平台-5 留存策略与 Retention Purge ——

bool ServerStore::retention_set(int days, const std::string& department_path,
                                const std::string& updated_by,
                                std::int64_t ts_ms) {
  // 白名单枚举（蓝图§十四）：30 天/6 个月/1 年/3 年/Indefinite
  if (days != 0 && days != 30 && days != 180 && days != 365 && days != 1095)
    return false;
  if (!department_path.empty()) {
    // 部门行须挂已存在部门（与 set_policy 同口径，不顺手建部门）
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
      "INSERT INTO retention_policies(department_path, retention_days,"
      " updated_by, updated_ms) VALUES(?,?,?,?)"
      " ON CONFLICT(department_path) DO UPDATE SET"
      " retention_days = excluded.retention_days,"
      " updated_by = excluded.updated_by,"
      " updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, days);
  sqlite3_bind_text(st, 3, updated_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::RetentionPolicy> ServerStore::retention_list() {
  std::vector<RetentionPolicy> out;
  const char* sql =
      "SELECT id, department_path, retention_days, updated_by, updated_ms"
      " FROM retention_policies ORDER BY department_path = '' DESC,"
      " department_path ASC;"; // 全局行在前
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  while (sqlite3_step(st) == SQLITE_ROW) {
    RetentionPolicy p;
    p.id = sqlite3_column_int64(st, 0);
    p.department_path = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    p.retention_days = sqlite3_column_int(st, 2);
    p.updated_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.updated_ms = sqlite3_column_int64(st, 4);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}

ServerStore::RetentionPolicy ServerStore::retention_resolve(
    const std::string& account) {
  // 生效留存期：本人部门链逐级上溯（与 resolve_policy 同走法）→ 全局行 →
  // 内置默认 Indefinite（0）
  std::map<std::string, RetentionPolicy> by_path;
  for (const auto& p : retention_list()) by_path[p.department_path] = p;
  std::string path;
  if (const auto prof = member_profile(account)) {
    path = prof->department_path;
  }
  while (!path.empty()) {
    const auto it = by_path.find(path);
    if (it != by_path.end()) return it->second;
    const auto pos = path.rfind('/');
    if (pos == std::string::npos) break;
    path.resize(pos);
  }
  const auto g = by_path.find("");
  if (g != by_path.end()) return g->second;
  RetentionPolicy fallback;
  // 内置默认：6 个月（180 天）——2026-10-10 评审组拍板决策 #6（授权代拍、
  // 效果后审）；配置化可改：retention set <days|indefinite> 覆盖（部门链/全局）
  fallback.retention_days = 180;
  return fallback;
}

std::int64_t ServerStore::retention_purge(std::int64_t before_ms,
                                          const std::string& reason,
                                          const std::string& purged_by,
                                          const std::string& approved_by,
                                          int policy_days,
                                          std::int64_t ts_ms) {
  // 高风险三道闸：双人（两账号存在且不同）＋理由必填＋时间线有效
  if (before_ms <= 0) return 0;
  if (reason.empty()) return 0;
  if (purged_by.empty() || approved_by.empty()) return 0;
  if (purged_by == approved_by) return 0; // 双人审批：一人不得自批自清
  if (!find_account(purged_by) || !find_account(approved_by)) return 0;
  const auto victims = [&] {
    std::vector<std::string> ids;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT msg_id FROM messages WHERE ts_ms < ?"
                           " ORDER BY id;",
                           -1, &st, nullptr) != SQLITE_OK)
      return ids;
    sqlite3_bind_int64(st, 1, before_ms);
    while (sqlite3_step(st) == SQLITE_ROW) {
      ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
    }
    sqlite3_finalize(st);
    return ids;
  }();
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT purge;", nullptr, nullptr, &tx) !=
      SQLITE_OK) {
    sqlite3_free(tx);
    return 0;
  }
  bool ok = true;
  for (const auto& id : victims) {
    // 逐条落 purged 事件（事件与清除同事务——无痕删除被禁止）
    ok = append_message_event(id, "purged", purged_by, reason, ts_ms);
    if (!ok) break;
  }
  std::int64_t ledger_id = 0;
  if (ok) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO retention_purges(purged_by, approved_by, reason,"
             " policy_days, msg_count, before_ms, purged_ms)"
             " VALUES(?,?,?,?,?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, purged_by.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2, approved_by.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 3, reason.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(i, 4, policy_days);
      sqlite3_bind_int(i, 5, static_cast<int>(victims.size()));
      sqlite3_bind_int64(i, 6, before_ms);
      sqlite3_bind_int64(i, 7, ts_ms);
      ok = sqlite3_step(i) == SQLITE_DONE;
      if (ok) ledger_id = sqlite3_last_insert_rowid(db_);
    }
    sqlite3_finalize(i);
  }
  if (ok) { // 审计留痕复用查阅台账（purge 属敏感动作，谁何时清了几条）
    AuditReadRow a;
    a.op_account = purged_by;
    a.action = "purge";
    a.filters = "before_ms=" + std::to_string(before_ms) +
                " 批准=" + approved_by +
                " 理由=" + reason;
    a.result_count = static_cast<int>(victims.size());
    a.ts_ms = ts_ms;
    add_audit_read(a);
  }
  if (ok && ledger_id > 0) {
    ok = sqlite3_exec(db_, "RELEASE purge;", nullptr, nullptr, &tx) ==
         SQLITE_OK;
  } else {
    ok = false;
    sqlite3_exec(db_, "ROLLBACK TO purge;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE purge;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok ? ledger_id : 0;
}

std::vector<ServerStore::RetentionPurge> ServerStore::retention_purges(
    int limit) {
  std::vector<RetentionPurge> out;
  const char* sql =
      "SELECT id, purged_by, approved_by, reason, policy_days, msg_count,"
      " before_ms, purged_ms FROM retention_purges ORDER BY id DESC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(st, 1, limit > 0 ? limit : 50);
  while (sqlite3_step(st) == SQLITE_ROW) {
    RetentionPurge p;
    p.id = sqlite3_column_int64(st, 0);
    p.purged_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    p.approved_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    p.reason = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    p.policy_days = sqlite3_column_int(st, 4);
    p.msg_count = sqlite3_column_int(st, 5);
    p.before_ms = sqlite3_column_int64(st, 6);
    p.purged_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(p));
  }
  sqlite3_finalize(st);
  return out;
}


} // namespace memex::server
