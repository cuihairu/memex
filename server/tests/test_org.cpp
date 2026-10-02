// T2.6 组织架构验收：部门树、成员资料（直属上级独立字段，每人至多一名）、
// 上级链路逐级上溯、批量导入 CSV（错误行校验拒绝并报告行号）。
// 库级（ServerStore 直测）＋进程级（CLI org 子命令真跑）双层。
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "store.hpp"

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond    \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

std::string run_cli(const std::string& args, int* exit_code = nullptr) {
  const std::string out_path = "/tmp/memex-org-test-out.txt";
  const std::string cmd = std::string("\"" MEMEX_SERVER_BIN "\" ") + args +
                          " > " + out_path + " 2>&1";
  const int rc = std::system(cmd.c_str());
  if (exit_code) *exit_code = rc;
  std::ifstream f(out_path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

} // namespace

int main() {
  // —— 库级：部门树 ——
  memex::server::ServerStore store;
  CHECK(store.open(":memory:"));
  for (const auto& [acct, name] : std::vector<std::pair<std::string, std::string>>{
           {"alice", "Alice"}, {"bob", "Bob"}, {"carol", "Carol"},
           {"dave", "Dave"}, {"eve", "Eve"}}) {
    CHECK(store.create_account(acct, "pw-" + acct, name));
  }

  const int dept_id = store.ensure_department_path("公司/研发部/客户端组");
  CHECK(dept_id > 0);
  const auto depts = store.department_list();
  CHECK(depts.size() == 3); // 三级逐级创建
  bool has_full_path = false;
  for (const auto& [id, path] : depts) {
    if (path == "公司/研发部/客户端组" && id == dept_id) has_full_path = true;
  }
  CHECK(has_full_path);
  CHECK(store.ensure_department_path("公司/研发部/客户端组") == dept_id); // 幂等
  CHECK(store.ensure_department_path("") == -1);
  CHECK(store.ensure_department_path("公司//坏路径") == -1);

  // —— 库级：成员资料与上级链路 ——
  CHECK(store.set_member_profile("alice", store.ensure_department_path("公司"),
                                 "总经理", ""));
  CHECK(store.set_member_profile("bob", store.ensure_department_path("公司/研发部"),
                                 "研发主管", "alice"));
  CHECK(store.set_member_profile("carol",
                                 store.ensure_department_path("公司/研发部/客户端组"),
                                 "组长", "bob"));
  CHECK(store.set_member_profile("dave",
                                 store.ensure_department_path("公司/研发部/客户端组"),
                                 "工程师", "carol"));

  const auto p = store.member_profile("dave");
  CHECK(p.has_value());
  CHECK(p->manager == "carol"); // 直属上级独立字段
  CHECK(p->title == "工程师");
  CHECK(p->department_path == "公司/研发部/客户端组");
  CHECK(p->display_name == "Dave");

  const auto chain = store.manager_chain("dave");
  CHECK(chain.size() == 3); // 逐级上溯：carol → bob → alice
  CHECK(chain[0] == "carol" && chain[1] == "bob" && chain[2] == "alice");
  CHECK(store.manager_chain("alice").empty()); // 顶到头

  // 校验拒绝：自为上级／上级账号不存在／构成汇报环
  CHECK(!store.set_member_profile("eve", -1, "", "eve"));
  CHECK(!store.set_member_profile("eve", -1, "", "ghost"));
  CHECK(!store.set_member_profile("alice", -1, "总经理", "dave")); // 环
  CHECK(store.manager_chain("alice").empty()); // 被拒后链路不变

  // —— 库级：批量导入（错误行校验拒绝）——
  std::vector<memex::server::OrgImportRow> rows;
  auto make_row = [&](int line_no, const std::string& a, const std::string& dept,
                      const std::string& title, const std::string& mgr) {
    memex::server::OrgImportRow r;
    r.line_no = line_no;
    r.account = a;
    r.dept = dept;
    r.title = title;
    r.manager = mgr;
    rows.push_back(r);
  };
  make_row(2, "eve", "公司/研发部", "测试工程师", "carol"); // 合法
  make_row(3, "ghost", "公司", "", "");                    // 账号不存在
  make_row(4, "dave", "公司", "", "ghost");                // 上级不存在
  make_row(5, "carol", "公司", "", "carol");               // 自为上级
  const auto result = store.import_members(rows);
  CHECK(result.imported == 1);
  CHECK(result.errors.size() == 3);
  bool rejected_line3 = false, rejected_line4 = false, rejected_line5 = false;
  for (const auto& e : result.errors) {
    if (e.find("第 3 行") != std::string::npos) rejected_line3 = true;
    if (e.find("第 4 行") != std::string::npos) rejected_line4 = true;
    if (e.find("第 5 行") != std::string::npos) rejected_line5 = true;
  }
  CHECK(rejected_line3 && rejected_line4 && rejected_line5);
  CHECK(store.member_profile("eve").has_value()); // 合法行已入库
  CHECK(!store.member_profile("ghost").has_value());
  store.close();

  // —— 进程级：CLI org（建库→账号→导入→详情→链路→坏更新拒绝）——
  const std::string db = "/tmp/memex-org-test.db";
  std::remove(db.c_str());
  for (const char* acct : {"alice", "bob", "carol", "dave", "eve"}) {
    CHECK(run_cli(std::string("account add ") + acct + " pw --db " + db)
              .find("已建号") != std::string::npos);
  }

  const std::string csv = "/tmp/memex-org-test.csv";
  {
    std::ofstream f(csv);
    f << "账号,部门,职务,直属上级\n"
      << "alice,公司,总经理,\n"
      << "bob,公司/研发部,研发主管,alice\n"
      << "carol,公司/研发部/客户端组,组长,bob\n"
      << "dave,公司/研发部/客户端组,工程师,carol\n"
      << "ghost,公司,,\n"          // 账号不存在
      << "fred,公司,职员\n"        // 列数不足（3 列）
      << "gary,公司,职员,ghost\n"  // 上级不存在
      << "helen,公司,职员,helen\n"; // 自为上级
  }
  const std::string imp =
      run_cli("org import " + csv + " --db " + db);
  CHECK(imp.find("成功 4 条") != std::string::npos);
  CHECK(imp.find("拒绝 4 条") != std::string::npos);
  CHECK(imp.find("第 6 行") != std::string::npos);
  CHECK(imp.find("第 7 行") != std::string::npos);
  CHECK(imp.find("第 8 行") != std::string::npos);
  CHECK(imp.find("第 9 行") != std::string::npos);

  // 成员详情：直属上级＋链路逐级上溯（CLI 输出面）
  const std::string member = run_cli("org member dave --db " + db);
  CHECK(member.find("直属上级：carol") != std::string::npos);
  CHECK(member.find("部门：公司/研发部/客户端组") != std::string::npos);
  CHECK(member.find("carol → bob → alice") != std::string::npos);

  // 坏更新拒绝：把 bob 的上级改成 dave 会构成环
  int rc = 0;
  const std::string bad = run_cli("org set bob --manager dave --db " + db, &rc);
  CHECK(rc != 0);
  CHECK(bad.find("构成汇报环") != std::string::npos);
  CHECK(run_cli("org member bob --db " + db).find("直属上级：alice") !=
        std::string::npos); // 被拒后资料不变

  // 部门树列表
  const std::string dept_out = run_cli("org dept list --db " + db);
  CHECK(dept_out.find("公司") != std::string::npos);
  CHECK(dept_out.find("公司/研发部/客户端组") != std::string::npos);

  if (g_failures == 0) {
    std::cout << "org tests: all passed\n";
    return 0;
  }
  std::cerr << "org tests: " << g_failures << " failure(s)\n";
  return 1;
}
