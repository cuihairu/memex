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
  if (!find_account(account)) return false;           // 账号不存在
  if (manager == account) return false;               // 不得自为上级
  if (!manager.empty() && !find_account(manager)) {
    return false;                                     // 上级账号不存在
  }
  if (!manager.empty()) {
    // 环校验：从拟设上级沿权威表链路上溯，若回到本人则构成环
    std::string cur = manager;
    std::set<std::string> seen;
    while (!cur.empty() && cur != account) {
      if (!seen.insert(cur).second) break; // 既有环防御，止步
      sqlite3_stmt* s = nullptr;
      if (sqlite3_prepare_v2(
              db_,
              "SELECT manager_account FROM org_reporting_lines"
              " WHERE account = ?;",
              -1, &s, nullptr) != SQLITE_OK)
        return false;
      sqlite3_bind_text(s, 1, cur.c_str(), -1, SQLITE_TRANSIENT);
      std::string next;
      if (sqlite3_step(s) == SQLITE_ROW &&
          sqlite3_column_type(s, 0) != SQLITE_NULL) {
        next = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
      }
      sqlite3_finalize(s);
      if (next == account) return false; // 链路回到本人：环
      cur = next;
    }
  }
  // 平台-3：三关联权威表与单值镜像同事务写（member_profiles 的
  // department_id/manager 列=单值视图，消费者 member_profile/manager_chain
  // 不改语义；多部门/链路独立维护走 membership_*/reporting_*）。
  // SAVEPOINT 而非 BEGIN：import_members 在外层事务内逐行调用本函数
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT mprof;", nullptr, nullptr, &tx) !=
      SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(db_,
                            "DELETE FROM org_memberships WHERE account = ?;",
                            -1, &d, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE;
    }
    sqlite3_finalize(d);
  }
  if (ok && department_id >= 0) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO org_memberships(account, department_id, created_ms)"
             " VALUES(?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(i, 2, department_id);
      sqlite3_bind_int64(i, 3, now_ms());
      ok = sqlite3_step(i) == SQLITE_DONE;
    }
    sqlite3_finalize(i);
  }
  if (ok) {
    sqlite3_stmt* r = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "DELETE FROM org_reporting_lines WHERE account = ?;",
             -1, &r, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(r, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(r) == SQLITE_DONE;
    }
    sqlite3_finalize(r);
  }
  if (ok && !manager.empty()) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO org_reporting_lines(account, manager_account,"
             " created_ms) VALUES(?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(i, 3, now_ms());
      ok = sqlite3_step(i) == SQLITE_DONE;
    }
    sqlite3_finalize(i);
  }
  if (ok) {
    sqlite3_stmt* st = nullptr;
    ok = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      if (department_id < 0) {
        sqlite3_bind_null(st, 2);
      } else {
        sqlite3_bind_int(st, 2, department_id);
      }
      sqlite3_bind_text(st, 3, title.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 4, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 5, now_ms());
      ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE mprof;", nullptr, nullptr, &tx) == SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO mprof;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE mprof;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
}

std::optional<MemberProfile> ServerStore::member_profile(
    const std::string& account) {
  const char* sql =
      "SELECT p.title, p.manager, p.department_id,"
      " a.display_name, a.role, p.signature FROM member_profiles p"
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
    m.signature = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
    out = m;
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<MemberProfile> ServerStore::member_list() {
  std::vector<MemberProfile> out;
  const char* sql =
      "SELECT a.account, a.display_name, a.role, p.title, p.manager,"
      " p.department_id, p.signature FROM accounts a"
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
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
      m.signature = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    }
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

// 个性签名设置/清除（需求批⑪）：档案行可能不存在（create_account 不建档），
// upsert 兜底建行；空串=清除。长度门 120 字由会话层把关。
bool ServerStore::set_signature(const std::string& account,
                                const std::string& signature) {
  if (!find_account(account)) return false;
  const char* sql =
      "INSERT INTO member_profiles(account, signature, updated_ms)"
      " VALUES(?, ?, ?)"
      " ON CONFLICT(account) DO UPDATE SET"
      " signature = excluded.signature, updated_ms = excluded.updated_ms;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, signature.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, now_ms());
  bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<std::string> ServerStore::manager_chain(
    const std::string& account) {
  std::vector<std::string> chain;
  std::set<std::string> seen{account};
  std::string cur = account;
  // 平台-3：沿权威表 org_reporting_lines 逐级上溯（镜像列不入链路判定）
  while (true) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT manager_account FROM org_reporting_lines"
            " WHERE account = ?;",
            -1, &s, nullptr) != SQLITE_OK)
      break;
    sqlite3_bind_text(s, 1, cur.c_str(), -1, SQLITE_TRANSIENT);
    std::string next;
    if (sqlite3_step(s) == SQLITE_ROW &&
        sqlite3_column_type(s, 0) != SQLITE_NULL) {
      next = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
    }
    sqlite3_finalize(s);
    if (next.empty()) break;
    if (!seen.insert(next).second) break; // 环防御
    chain.push_back(next);
    cur = next;
  }
  return chain;
}

// —— 平台-3：组织三关联（成员/授权/汇报线）——

bool ServerStore::membership_add(const std::string& account, int department_id,
                                 std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return false;
  if (department_id < 0) return false;
  sqlite3_stmt* d = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT 1 FROM departments WHERE id = ?;", -1,
                         &d, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int(d, 1, department_id);
  const bool dept_ok = sqlite3_step(d) == SQLITE_ROW;
  sqlite3_finalize(d);
  if (!dept_ok) return false;
  const char* sql =
      "INSERT INTO org_memberships(account, department_id, created_ms)"
      " VALUES(?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, department_id);
  sqlite3_bind_int64(st, 3, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE; // 唯一冲突（已入）→ false
  sqlite3_finalize(st);
  return ok;
}

bool ServerStore::membership_remove(const std::string& account,
                                    int department_id) {
  const char* sql =
      "DELETE FROM org_memberships WHERE account = ? AND department_id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, department_id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<ServerStore::OrgMembership> ServerStore::memberships_of(
    const std::string& account) {
  std::vector<OrgMembership> out;
  const char* sql =
      "SELECT id, account, department_id, created_ms FROM org_memberships"
      " WHERE account = ? ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    OrgMembership m;
    m.id = sqlite3_column_int64(st, 0);
    m.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    m.department_id = sqlite3_column_int(st, 2);
    m.created_ms = sqlite3_column_int64(st, 3);
    out.push_back(std::move(m));
  }
  sqlite3_finalize(st);
  return out;
}

std::int64_t ServerStore::role_grant(const std::string& account,
                                     const std::string& role,
                                     const std::string& scope,
                                     std::int64_t valid_from_ms,
                                     std::int64_t valid_until_ms,
                                     const std::string& granted_by,
                                     std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return 0;
  if (role.empty()) return 0;
  // 时间窗倒置拒（0 端点=不开窗不参与比较）
  if (valid_from_ms > 0 && valid_until_ms > 0 && valid_from_ms >= valid_until_ms)
    return 0;
  const char* sql =
      "INSERT INTO org_role_assignments(account, role, scope, valid_from_ms,"
      " valid_until_ms, granted_by, created_ms) VALUES(?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, role.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, scope.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, valid_from_ms);
  sqlite3_bind_int64(st, 5, valid_until_ms);
  sqlite3_bind_text(st, 6, granted_by.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 7, ts_ms);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok ? sqlite3_last_insert_rowid(db_) : 0;
}

bool ServerStore::role_revoke(std::int64_t id) {
  const char* sql = "DELETE FROM org_role_assignments WHERE id = ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_int64(st, 1, id);
  const bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
}

std::vector<std::string> ServerStore::effective_roles(
    const std::string& account, std::int64_t at_ms) {
  std::vector<std::string> out;
  // 基础角色（accounts.role）
  sqlite3_stmt* b = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT role FROM accounts WHERE account = ?;",
                         -1, &b, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(b, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(b) == SQLITE_ROW && sqlite3_column_type(b, 0) != SQLITE_NULL) {
      out.push_back(reinterpret_cast<const char*>(sqlite3_column_text(b, 0)));
    }
    sqlite3_finalize(b);
  }
  // 窗内追加授权：from=0 不设下界；until=0 不设上界；否则 from<=at<until
  sqlite3_stmt* st = nullptr;
  const char* sql =
      "SELECT role FROM org_role_assignments WHERE account = ?"
      " AND (valid_from_ms = 0 OR valid_from_ms <= ?)"
      " AND (valid_until_ms = 0 OR ? < valid_until_ms) ORDER BY id;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, at_ms);
  sqlite3_bind_int64(st, 3, at_ms);
  std::set<std::string> seen(out.begin(), out.end());
  while (sqlite3_step(st) == SQLITE_ROW) {
    if (sqlite3_column_type(st, 0) == SQLITE_NULL) continue;
    std::string role = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    if (role.empty() || !seen.insert(role).second) continue;
    out.push_back(std::move(role));
  }
  sqlite3_finalize(st);
  return out;
}

std::vector<ServerStore::RoleAssignment> ServerStore::role_assignments(
    const std::string& account) {
  std::vector<RoleAssignment> out;
  std::string sql =
      "SELECT id, account, role, scope, valid_from_ms, valid_until_ms,"
      " granted_by, created_ms FROM org_role_assignments";
  if (!account.empty()) sql += " WHERE account = ?";
  sql += " ORDER BY id;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
    return out;
  if (!account.empty())
    sqlite3_bind_text(st, 1, account.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    RoleAssignment r;
    r.id = sqlite3_column_int64(st, 0);
    r.account = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    r.role = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    r.scope = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
    r.valid_from_ms = sqlite3_column_int64(st, 4);
    r.valid_until_ms = sqlite3_column_int64(st, 5);
    r.granted_by = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
    r.created_ms = sqlite3_column_int64(st, 7);
    out.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return out;
}

bool ServerStore::reporting_set(const std::string& account,
                                const std::string& manager,
                                std::int64_t ts_ms) {
  if (!find_account(account).has_value()) return false;
  if (manager.empty() || manager == account) return false; // 自为上级拒
  if (!find_account(manager).has_value()) return false;
  // 环校验：manager 的上游链不得回到 account（沿权威表走）
  {
    std::set<std::string> seen{account};
    std::string cur = manager;
    int guard = 0;
    while (!cur.empty() && guard++ < 256) {
      if (!seen.insert(cur).second) return false; // manager 已在环上
      if (cur == account) return false;
      sqlite3_stmt* s = nullptr;
      if (sqlite3_prepare_v2(
              db_,
              "SELECT manager_account FROM org_reporting_lines"
              " WHERE account = ?;",
              -1, &s, nullptr) != SQLITE_OK)
        return false;
      sqlite3_bind_text(s, 1, cur.c_str(), -1, SQLITE_TRANSIENT);
      std::string next;
      if (sqlite3_step(s) == SQLITE_ROW &&
          sqlite3_column_type(s, 0) != SQLITE_NULL) {
        next = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
      }
      sqlite3_finalize(s);
      cur = next;
    }
  }
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT rpt;", nullptr, nullptr, &tx) != SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(
             db_, "DELETE FROM org_reporting_lines WHERE account = ?;", -1, &d,
             nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE;
    }
    sqlite3_finalize(d);
  }
  if (ok) {
    sqlite3_stmt* i = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "INSERT INTO org_reporting_lines(account, manager_account,"
             " created_ms) VALUES(?,?,?);",
             -1, &i, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(i, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(i, 2, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(i, 3, ts_ms);
      ok = sqlite3_step(i) == SQLITE_DONE;
    }
    sqlite3_finalize(i);
  }
  if (ok) { // 镜像列同步（有档案才更；无档案不建档）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "UPDATE member_profiles SET manager = ?, updated_ms = ?"
             " WHERE account = ?;",
             -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(u, 1, manager.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(u, 2, now_ms());
      sqlite3_bind_text(u, 3, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE;
    }
    sqlite3_finalize(u);
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx) == SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO rpt;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
}

bool ServerStore::reporting_clear(const std::string& account) {
  if (!find_account(account).has_value()) return false;
  char* tx = nullptr;
  if (sqlite3_exec(db_, "SAVEPOINT rpt;", nullptr, nullptr, &tx) != SQLITE_OK) {
    sqlite3_free(tx);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* d = nullptr;
    ok = sqlite3_prepare_v2(
             db_, "DELETE FROM org_reporting_lines WHERE account = ?;", -1, &d,
             nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(d, 1, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(d) == SQLITE_DONE;
    }
    sqlite3_finalize(d);
  }
  if (ok) { // 镜像列同步（有档案才清；无档案不动）
    sqlite3_stmt* u = nullptr;
    ok = sqlite3_prepare_v2(
             db_,
             "UPDATE member_profiles SET manager = '', updated_ms = ?"
             " WHERE account = ?;",
             -1, &u, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_int64(u, 1, now_ms());
      sqlite3_bind_text(u, 2, account.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(u) == SQLITE_DONE;
    }
    sqlite3_finalize(u);
  }
  if (ok) {
    ok = sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx) == SQLITE_OK;
  } else {
    sqlite3_exec(db_, "ROLLBACK TO rpt;", nullptr, nullptr, &tx);
    sqlite3_exec(db_, "RELEASE rpt;", nullptr, nullptr, &tx);
  }
  sqlite3_free(tx);
  return ok;
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
                             bool new_device_approval,
                             bool allow_cross_dept_file,
                             bool allow_forward_file) {
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
      " new_device_approval, allow_cross_dept_file, allow_forward_file)"
      " VALUES(?, ?, ?, ?, ?, ?)"
      " ON CONFLICT(department_path) DO UPDATE SET"
      " allow_anonymous = excluded.allow_anonymous,"
      " allow_cross_state = excluded.allow_cross_state,"
      " new_device_approval = excluded.new_device_approval,"
      " allow_cross_dept_file = excluded.allow_cross_dept_file,"
      " allow_forward_file = excluded.allow_forward_file;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
  sqlite3_bind_text(st, 1, department_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 2, allow_anonymous ? 1 : 0);
  sqlite3_bind_int(st, 3, allow_cross_state ? 1 : 0);
  sqlite3_bind_int(st, 4, new_device_approval ? 1 : 0);
  sqlite3_bind_int(st, 5, allow_cross_dept_file ? 1 : 0);
  sqlite3_bind_int(st, 6, allow_forward_file ? 1 : 0);
  const bool ok = sqlite3_step(st) == SQLITE_DONE;
  sqlite3_finalize(st);
  return ok;
}

std::vector<PolicyRow> ServerStore::policy_list() {
  std::vector<PolicyRow> out;
  const char* sql =
      "SELECT department_path, allow_anonymous, allow_cross_state,"
      " new_device_approval, allow_cross_dept_file, allow_forward_file"
      " FROM policies"
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
    p.allow_cross_dept_file = sqlite3_column_int(st, 4) != 0;
    p.allow_forward_file = sqlite3_column_int(st, 5) != 0;
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
  // 平台-3：admin 判定走有效角色（基础 ∪ 窗内追加授权）
  const auto vroles = effective_roles(viewer, now_ms());
  ctx.is_admin = std::find(vroles.begin(), vroles.end(), "admin") != vroles.end();
  if (const auto vprof = member_profile(viewer)) {
    ctx.vdept = vprof->department_path;
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
  const auto vroles = effective_roles(viewer, now_ms());
  ctx.is_admin = std::find(vroles.begin(), vroles.end(), "admin") != vroles.end();
  if (const auto vprof = member_profile(viewer)) {
    ctx.vdept = vprof->department_path;
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

bool ServerStore::co_members(const std::string& a, const std::string& b) {
  if (a.empty() || a == b) return false;
  const char* sql =
      "SELECT 1 FROM group_members m1 JOIN group_members m2"
      " ON m1.group_id = m2.group_id"
      " WHERE m1.account = ? AND m2.account = ? LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(st, 1, a.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, b.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return ok;
}


} // namespace memex::server
