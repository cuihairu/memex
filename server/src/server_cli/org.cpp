// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"


int cmd_org(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server org dept add <路径> | dept list | "
                 "member <账号> | set … | import <CSV> | "
                 "membership add|remove|list … | role grant|revoke|list … | "
                 "reporting set|clear <账号> <上级> | hide/unhide <账号> | "
                 "dept hide|restrict <路径> | fields … | allow/disallow … | "
                 "visibility list [--db <库>]\n";
    return 2;
  }
  const std::string_view sub = argv[0];
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }

  if (sub == "dept") {
    if (argc >= 3 && std::string_view(argv[1]) == "add") {
      const int id = store.ensure_department_path(argv[2]);
      if (id < 0) {
        std::cerr << "部门路径非法（空段／连续斜杠）：" << argv[2] << "\n";
        return 1;
      }
      std::cout << "已建部门：" << argv[2] << "（id " << id << "）\n";
      return 0;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "list") {
      for (const auto& [id, path] : store.department_list()) {
        std::cout << id << '\t' << path << '\n';
      }
      return 0;
    }
    // T4.6 部门级可见性：隐藏整树（部门内自己人仍互见）／限看本部门
    if (argc >= 3) {
      const std::string_view op = argv[1];
      if (op == "hide" || op == "unhide" || op == "restrict" ||
          op == "unrestrict") {
        memex::server::VisibilityRow row =
            store.visibility_row("dept", argv[2])
                .value_or(memex::server::VisibilityRow{});
        row.scope = "dept";
        row.key = argv[2];
        if (op == "hide") row.hidden = true;
        if (op == "unhide") row.hidden = false;
        if (op == "restrict") row.restrict_scope = true;
        if (op == "unrestrict") row.restrict_scope = false;
        if (!store.set_visibility(row.scope, row.key, row.hidden,
                                  row.restrict_scope, row.hide_fields)) {
          std::cerr << "部门不存在或写入失败：" << argv[2] << "\n";
          return 1;
        }
        std::cout << "已配置部门可见性：" << argv[2] << "（"
                  << (row.hidden ? "隐藏" : "不隐藏") << "·"
                  << (row.restrict_scope ? "限看本部门" : "不限看") << "）\n";
        return 0;
      }
    }
    std::cerr << "用法：org dept add <路径> | dept list | dept hide <路径> | "
                 "dept unhide <路径> | dept restrict <路径> | dept "
                 "unrestrict <路径>\n";
    return 2;
  }

  if (sub == "list") {
    // 全员一览（账号、展示名、部门、职务、直属上级、角色）
    std::cout << "账号\t展示名\t部门\t职务\t直属上级\t角色\n";
    for (const auto& m : store.member_list()) {
      std::cout << m.account << '\t' << m.display_name << '\t'
                << (m.department_path.empty() ? "（未分配）" : m.department_path)
                << '\t' << (m.title.empty() ? "（无）" : m.title) << '\t'
                << (m.manager.empty() ? "（无）" : m.manager) << '\t'
                << (m.role.empty() ? "member" : m.role) << '\n';
    }
    return 0;
  }

  if (sub == "member") {    if (argc < 2) {
      std::cerr << "用法：org member <账号>\n";
      return 2;
    }
    const auto p = store.member_profile(argv[1]);
    const auto mems = store.memberships_of(argv[1]);
    if (!p && mems.empty()) {
      std::cerr << "成员未建档：" << argv[1] << "\n";
      return 1;
    }
    // 平台-3：部门列权威表多值（无档案行也有归属视图）；其余单值走镜像列
    if (p) {
      std::cout << "账号：" << p->account << "（" << p->display_name << "）\n";
    } else {
      std::cout << "账号：" << mems.front().account << "（无档案）\n";
    }
    if (mems.empty()) {
      std::cout << "部门：（未分配）\n";
    } else {
      std::cout << "部门：";
      for (std::size_t i = 0; i < mems.size(); ++i)
        std::cout << (i ? "、" : "") << store.department_path(
                                            mems[i].department_id);
      std::cout << "\n";
    }
    const auto chain = store.manager_chain(argv[1]);
    std::cout << "职务："
              << (p && !p->title.empty() ? p->title : "（无）") << "\n"
              << "直属上级："
              << (chain.empty() ? "（无）" : chain.front()) << "\n";
    if (!chain.empty()) {
      std::cout << "上级链路（逐级上溯）：";
      for (std::size_t i = 0; i < chain.size(); ++i) {
        std::cout << (i ? " → " : "") << chain[i];
      }
      std::cout << "\n";
    }
    const auto eff = store.effective_roles(argv[1],
                                           std::chrono::duration_cast<
                                               std::chrono::milliseconds>(
                                               std::chrono::system_clock::
                                                   now()
                                                       .time_since_epoch())
                                               .count());
    std::cout << "有效角色：";
    for (std::size_t i = 0; i < eff.size(); ++i)
      std::cout << (i ? "、" : "") << eff[i];
    std::cout << "\n";
    return 0;
  }

  // —— 平台-3：组织三关联（多部门归属／追加授权／汇报线独立维护）——
  if (sub == "membership") {
    // 用法：org membership add|remove <账号> <部门路径> | list <账号>
    const auto dept_id_by_path =
        [&store](const std::string& path) -> int {
      for (const auto& [id, p] : store.department_list())
        if (p == path) return id;
      return -1;
    };
    if (argc >= 4 && std::string_view(argv[1]) == "add") {
      const int id = store.ensure_department_path(argv[3]);
      if (id < 0) {
        std::cerr << "部门路径非法：" << argv[3] << "\n";
        return 1;
      }
      const std::int64_t ts =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      if (!store.membership_add(argv[2], id, ts)) {
        std::cerr << "归属失败（账号不存在或已在该部门）：" << argv[2]
                  << " → " << argv[3] << "\n";
        return 1;
      }
      std::cout << "已加入部门：" << argv[2] << " → " << argv[3]
                << "（id " << id << "）\n";
      return 0;
    }
    if (argc >= 4 && std::string_view(argv[1]) == "remove") {
      const int id = dept_id_by_path(argv[3]);
      if (id < 0 || !store.membership_remove(argv[2], id)) {
        std::cerr << "移除失败（部门或归属不存在）：" << argv[2] << " × "
                  << argv[3] << "\n";
        return 1;
      }
      std::cout << "已移出部门：" << argv[2] << " × " << argv[3] << "\n";
      return 0;
    }
    if (argc >= 3 && std::string_view(argv[1]) == "list") {
      for (const auto& m : store.memberships_of(argv[2])) {
        std::cout << m.id << '\t' << m.account << '\t'
                  << store.department_path(m.department_id) << '\t'
                  << m.created_ms << '\n';
      }
      return 0;
    }
    std::cerr << "用法：org membership add|remove <账号> <部门路径> | "
                 "list <账号>\n";
    return 2;
  }

  if (sub == "role") {
    // 用法：org role grant <账号> <role> [--scope 路径] [--from 毫秒]
    //            [--until 毫秒] [--by 操作者] | revoke <id> | list [账号]
    if (argc >= 2 && std::string_view(argv[1]) == "grant") {
      if (argc < 4) {
        std::cerr << "用法：org role grant <账号> <role> [--scope 路径] "
                     "[--from 毫秒] [--until 毫秒] [--by 操作者]\n";
        return 2;
      }
      std::string scope, by;
      std::int64_t from = 0, until = 0;
      for (int i = 4; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const char* val = argv[i + 1];
        if (flag == "--scope") scope = val;
        else if (flag == "--from") from = std::strtoll(val, nullptr, 10);
        else if (flag == "--until") until = std::strtoll(val, nullptr, 10);
        else if (flag == "--by") by = val;
      }
      const std::int64_t ts =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      const std::int64_t id = store.role_grant(argv[2], argv[3], scope, from,
                                               until, by, ts);
      if (id == 0) {
        std::cerr << "授权失败（账号不存在、role 为空或时间窗倒置）\n";
        return 1;
      }
      std::cout << "已授权：" << argv[2] << " ← " << argv[3]
                << (scope.empty() ? "" : "（scope " + scope + "）")
                << "（台账 id " << id << "）\n";
      return 0;
    }
    if (argc >= 3 && std::string_view(argv[1]) == "revoke") {
      if (!store.role_revoke(std::strtoll(argv[2], nullptr, 10))) {
        std::cerr << "撤销失败（台账 id 不存在）：" << argv[2] << "\n";
        return 1;
      }
      std::cout << "已撤销授权台账：" << argv[2] << "\n";
      return 0;
    }
    if (argc >= 2 && std::string_view(argv[1]) == "list") {
      const std::string only = argc >= 3 ? argv[2] : "";
      const std::int64_t now =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      std::cout << "id\t账号\t角色\tscope\t生效自\t生效至\t授予人\t状态\n";
      for (const auto& r : store.role_assignments(only)) {
        const bool active =
            (r.valid_from_ms == 0 || r.valid_from_ms <= now) &&
            (r.valid_until_ms == 0 || now < r.valid_until_ms);
        std::cout << r.id << '\t' << r.account << '\t' << r.role << '\t'
                  << (r.scope.empty() ? "（全scope）" : r.scope) << '\t'
                  << (r.valid_from_ms == 0 ? "（即起）"
                                           : std::to_string(r.valid_from_ms))
                  << '\t'
                  << (r.valid_until_ms == 0 ? "（无限）"
                                            : std::to_string(r.valid_until_ms))
                  << '\t' << (r.granted_by.empty() ? "（未记）" : r.granted_by)
                  << '\t' << (active ? "生效中" : "未生效/已过期") << '\n';
      }
      return 0;
    }
    std::cerr << "用法：org role grant <账号> <role> [--scope 路径] "
                 "[--from 毫秒] [--until 毫秒] [--by 操作者] | "
                 "revoke <id> | list [账号]\n";
    return 2;
  }

  if (sub == "reporting") {
    // 用法：org reporting set <账号> <上级账号> | clear <账号>
    if (argc >= 4 && std::string_view(argv[1]) == "set") {
      const std::int64_t ts =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      if (!store.reporting_set(argv[2], argv[3], ts)) {
        std::cerr << "设置失败（账号不存在、自为上级或会成环）："
                  << argv[2] << " → " << argv[3] << "\n";
        return 1;
      }
      std::cout << "已设直属上级：" << argv[2] << " → " << argv[3] << "\n";
      return 0;
    }
    if (argc >= 3 && std::string_view(argv[1]) == "clear") {
      if (!store.reporting_clear(argv[2])) {
        std::cerr << "清除失败（账号不存在）：" << argv[2] << "\n";
        return 1;
      }
      std::cout << "已清直属上级：" << argv[2] << "\n";
      return 0;
    }
    std::cerr << "用法：org reporting set <账号> <上级账号> | clear <账号>\n";
    return 2;
  }

  // —— T4.6 通讯录可见性（成员级）：隐藏／敏感字段／白名单例外 ——
  if (sub == "hide" || sub == "unhide") {
    if (argc < 2) {
      std::cerr << "用法：org hide <账号> | org unhide <账号>\n";
      return 2;
    }
    memex::server::VisibilityRow row =
        store.visibility_row("member", argv[1])
            .value_or(memex::server::VisibilityRow{});
    row.scope = "member";
    row.key = argv[1];
    row.hidden = sub == "hide";
    if (!store.set_visibility(row.scope, row.key, row.hidden, row.restrict_scope,
                              row.hide_fields)) {
      std::cerr << "成员不存在或写入失败：" << argv[1] << "\n";
      return 1;
    }
    std::cout << (row.hidden ? "已隐藏成员：" : "已取消隐藏成员：") << argv[1]
              << "（本人与管理员仍可见；白名单例外经 org allow 配置）\n";
    return 0;
  }
  if (sub == "fields") {
    // org fields <账号> --hide title,manager,role | org fields <账号> --clear
    if (argc < 2) {
      std::cerr << "用法：org fields <账号> --hide title,manager[,role] | "
                   "org fields <账号> --clear\n";
      return 2;
    }
    std::string fields;
    bool clear = false;
    for (int i = 2; i < argc; ++i) {
      if (std::string_view(argv[i]) == "--hide" && i + 1 < argc) {
        fields = argv[++i];
      } else if (std::string_view(argv[i]) == "--clear") {
        clear = true;
      } else {
        std::cerr << "未知选项：" << argv[i] << "\n";
        return 2;
      }
    }
    if (clear) fields.clear();
    for (std::size_t start = 0; start <= fields.size();) {
      const std::size_t comma = fields.find(',', start);
      const std::string f =
          fields.substr(start, comma == std::string::npos ? std::string::npos
                                                           : comma - start);
      if (!f.empty() && f != "title" && f != "manager" && f != "role") {
        std::cerr << "敏感字段只支持 title,manager,role：" << f << "\n";
        return 2;
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    memex::server::VisibilityRow row =
        store.visibility_row("member", argv[1])
            .value_or(memex::server::VisibilityRow{});
    row.scope = "member";
    row.key = argv[1];
    row.hide_fields = fields;
    if (!store.set_visibility(row.scope, row.key, row.hidden, row.restrict_scope,
                              row.hide_fields)) {
      std::cerr << "成员不存在或写入失败：" << argv[1] << "\n";
      return 1;
    }
    std::cout << (fields.empty() ? "已清除敏感字段配置：" : "已配置敏感字段：")
              << argv[1]
              << (fields.empty() ? "（全部恢复可见）\n"
                                 : "（对非管理员隐藏：" + fields + "）\n");
    return 0;
  }
  if (sub == "allow" || sub == "disallow") {
    // org allow <查看者账号> --see <成员账号|部门路径>：白名单例外
    if (argc < 2) {
      std::cerr << "用法：org allow <查看者账号> --see <成员账号|部门路径> | "
                   "org disallow <查看者账号> --see <目标>\n";
      return 2;
    }
    const std::string viewer = argv[1];
    std::string target;
    for (int i = 2; i + 1 < argc; ++i) {
      if (std::string_view(argv[i]) == "--see") target = argv[++i];
    }
    if (target.empty()) {
      std::cerr << "缺 --see <成员账号|部门路径>\n";
      return 2;
    }
    if (sub == "disallow") {
      if (!store.remove_visibility_allow(viewer, target)) {
        std::cerr << "白名单删除失败\n";
        return 1;
      }
      std::cout << "已移除白名单：" << viewer << " → " << target << "\n";
      return 0;
    }
    // 校验：查看者须是账号；目标须是账号或部门路径（防静默配错）
    std::map<std::string, std::string> accounts;
    for (const auto& [acct, label] : store.account_list()) {
      accounts.emplace(acct, label);
    }
    if (accounts.count(viewer) == 0) {
      std::cerr << "查看者账号不存在：" << viewer << "\n";
      return 1;
    }
    bool target_ok = accounts.count(target) != 0;
    if (!target_ok) {
      for (const auto& [id, path] : store.department_list()) {
        (void)id;
        if (path == target) {
          target_ok = true;
          break;
        }
      }
    }
    if (!target_ok) {
      std::cerr << "目标既非账号也非部门路径：" << target << "\n";
      return 1;
    }
    if (!store.add_visibility_allow(viewer, target)) {
      std::cerr << "白名单写入失败\n";
      return 1;
    }
    std::cout << "已加白名单：" << viewer << " 可见 " << target << "\n";
    return 0;
  }
  if (sub == "visibility") {
    // 已配置行＋白名单一览（配置面自查）
    if (argc >= 2 && std::string_view(argv[1]) == "list") {
      std::cout << "范围\t隐藏\t限看本部门\t敏感字段\n";
      for (const auto& r : store.visibility_list()) {
        const std::string scope =
            r.scope == "member" ? "成员 " + r.key : "部门 " + r.key;
        std::cout << scope << '\t' << (r.hidden ? "是" : "否") << '\t'
                  << (r.restrict_scope ? "是" : "否") << '\t'
                  << (r.hide_fields.empty() ? "（无）" : r.hide_fields) << '\n';
      }
      const auto allows = store.visibility_allows();
      std::cout << "\n白名单例外（" << allows.size() << " 条）\n";
      for (const auto& a : allows) {
        std::cout << a.viewer << " → " << a.target << '\n';
      }
      return 0;
    }
    std::cerr << "用法：org visibility list\n";
    return 2;
  }

  if (sub == "set") {
    if (argc < 2) {
      std::cerr << "用法：org set <账号> [--dept 路径] [--title 职务] "
                   "[--manager 账号|none]\n";
      return 2;
    }
    const std::string account = argv[1];
    // 部分更新：未给出的项沿用现值
    const auto cur = store.member_profile(account);
    std::string dept = cur ? cur->department_path : "";
    std::string title = cur ? cur->title : "";
    std::string manager = cur ? cur->manager : "";
    for (int i = 2; i + 1 < argc; ++i) {
      const std::string_view opt = argv[i];
      if (opt == "--dept") {
        dept = argv[++i];
      } else if (opt == "--title") {
        title = argv[++i];
      } else if (opt == "--manager") {
        manager = argv[++i];
        if (manager == "none") manager.clear();
      }
    }
    const int dept_id =
        dept.empty() ? -1 : store.ensure_department_path(dept);
    if (!dept.empty() && dept_id < 0) {
      std::cerr << "部门路径非法：" << dept << "\n";
      return 1;
    }
    if (!store.set_member_profile(account, dept_id, title, manager)) {
      std::cerr << "建档被拒（账号不存在／上级非法／构成汇报环）\n";
      return 1;
    }
    std::cout << "已更新成员资料：" << account << "\n";
    return 0;
  }

  if (sub == "import") {
    if (argc < 2) {
      std::cerr << "用法：org import <CSV 文件> [--db <库>]\n"
                << "CSV 格式（UTF-8，首行表头）：账号,部门,职务,直属上级\n";
      return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
      std::cerr << "CSV 打不开：" << argv[1] << "\n";
      return 1;
    }
    std::vector<memex::server::OrgImportRow> rows;
    std::string line;
    int line_no = 0;
    std::vector<std::string> parse_errors;
    while (std::getline(in, line)) {
      ++line_no;
      if (line_no == 1) continue; // 表头
      if (line.empty()) continue;
      // 简单 CSV 拆分（无引号转义需求：账号／部门／职务不含逗号）
      std::vector<std::string> cols;
      std::size_t start = 0;
      while (true) {
        const std::size_t comma = line.find(',', start);
        cols.push_back(line.substr(
            start, comma == std::string::npos ? std::string::npos
                                              : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      if (cols.size() != 4) {
        parse_errors.push_back("第 " + std::to_string(line_no) +
                               " 行被拒绝：列数应为 4（实际 " +
                               std::to_string(cols.size()) + "）");
        continue;
      }
      memex::server::OrgImportRow r;
      r.line_no = line_no;
      r.account = cols[0];
      r.dept = cols[1];
      r.title = cols[2];
      r.manager = cols[3];
      rows.push_back(std::move(r));
    }
    const auto result = store.import_members(rows);
    for (const auto& e : parse_errors) std::cout << e << "\n";
    for (const auto& e : result.errors) std::cout << e << "\n";
    std::cout << "导入完成：成功 " << result.imported << " 条，拒绝 "
              << (result.errors.size() + parse_errors.size()) << " 条\n";
    return 0;
  }

  std::cerr << "未知 org 子命令：" << sub << "\n";
  return 2;
}
