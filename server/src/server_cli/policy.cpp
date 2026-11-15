// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"


// retention 子命令族（平台-5 留存策略与清除）：
//   retention set <days|indefinite> [--dept 路径] --by 账号
//                                  留存期白名单：30/180/365/1095 或 indefinite
//   retention show [账号]           已配置行（全局在前）；带账号=生效留存期
//   retention purge --before 时刻 --reason 文本 --by 账号 --approved-by 账号
//                   [--policy-days N]
//                                  Retention Purge（双人审批高风险；物理清
//                                  除＋逐条 purged 事件＋批次台账＋审计）
//   retention purges [N]            清除台账（倒序）
int cmd_retention(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server retention set|show|purge|purges … "
                 "[--db <库>]\n";
    return 2;
  }
  const std::string_view sub = argv[0];
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const std::int64_t now = std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now()
                                     .time_since_epoch())
      .count();

  if (sub == "set") {
    if (argc < 2) {
      std::cerr << "用法：org retention set <days|indefinite> [--dept 路径] "
                 "--by 账号\n";
      return 2;
    }
    int days = 0;
    const std::string_view d = argv[1];
    if (d == "indefinite" || d == "0") {
      days = 0;
    } else {
      days = std::atoi(argv[1]);
    }
    std::string dept, by;
    for (int i = 2; i + 1 < argc; i += 2) {
      const std::string_view flag = argv[i];
      if (flag == "--dept") dept = argv[i + 1];
      else if (flag == "--by") by = argv[i + 1];
    }
    if (by.empty()) {
      std::cerr << "缺 --by 账号（谁配置的须留痕）\n";
      return 2;
    }
    if (!store.retention_set(days, dept, by, now)) {
      std::cerr << "写入失败（天数须为 30/180/365/1095/indefinite，"
                   "部门须已存在）\n";
      return 1;
    }
    std::cout << "已配置留存期：" << (dept.empty() ? "（全局）" : dept)
              << " → " << (days == 0 ? "Indefinite" : std::to_string(days) + " 天")
              << "（by " << by << "）\n";
    return 0;
  }

  if (sub == "show") {
    const std::string who = argc >= 2 ? argv[1] : "";
    if (who.empty()) {
      std::cout << "scope\t留存期\t配置人\t配置时刻\n";
      for (const auto& p : store.retention_list()) {
        std::cout << (p.department_path.empty() ? "（全局）"
                                                : p.department_path)
                  << '\t'
                  << (p.retention_days == 0
                          ? "Indefinite"
                          : std::to_string(p.retention_days) + " 天")
                  << '\t' << p.updated_by << '\t' << p.updated_ms << '\n';
      }
      return 0;
    }
    const auto p = store.retention_resolve(who);
    std::cout << who << " 生效留存期："
              << (p.retention_days == 0 ? "Indefinite（永久）"
                                        : std::to_string(p.retention_days) + " 天")
              << "（scope " << (p.department_path.empty() ? "全局/默认"
                                                          : p.department_path)
              << "）\n";
    return 0;
  }

  if (sub == "purge") {
    std::int64_t before = 0;
    std::string reason, by, approved;
    int policy_days = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
      const std::string_view flag = argv[i];
      if (flag == "--before") {
        // 时刻或毫秒时间戳都收
        if (std::all_of(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]),
                        ::isdigit)) {
          before = std::strtoll(argv[i + 1], nullptr, 10);
        } else {
          before = parse_time_arg(argv[i + 1], false); // 日期粒度=当日零点
        }
      } else if (flag == "--reason") reason = argv[i + 1];
      else if (flag == "--by") by = argv[i + 1];
      else if (flag == "--approved-by") approved = argv[i + 1];
      else if (flag == "--policy-days") policy_days = std::atoi(argv[i + 1]);
    }
    if (before <= 0 || reason.empty() || by.empty() || approved.empty()) {
      std::cerr << "用法：retention purge --before <时刻|毫秒> --reason 文本 "
                 "--by 账号 --approved-by 账号 [--policy-days N]\n";
      return 2;
    }
    const std::int64_t id = store.retention_purge(before, reason, by, approved,
                                                  policy_days, now);
    if (id == 0) {
      std::cerr << "清除被拒（双人须两账号存在且不同、理由必填、时间线有效）\n";
      return 1;
    }
    std::cout << "已执行 Retention Purge（台账 id " << id << "）："
              << "清除 " << before << " 之前的归档（by " << by
              << "，批准 " << approved << "）\n";
    return 0;
  }

  if (sub == "purges") {
    const int n = argc >= 2 ? std::atoi(argv[1]) : 50;
    std::cout << "id\t执行人\t批准人\t条数\t期限(天)\t理由\t时刻\n";
    for (const auto& p : store.retention_purges(n)) {
      std::cout << p.id << '\t' << p.purged_by << '\t' << p.approved_by << '\t'
                << p.msg_count << '\t'
                << (p.policy_days == 0 ? "Indefinite"
                                       : std::to_string(p.policy_days))
                << '\t' << p.reason << '\t' << p.purged_ms << '\n';
    }
    return 0;
  }

  std::cerr << "未知 retention 子命令：" << sub << "\n";
  return 2;
}


// policy 子命令族（T3.4 策略开关，按部门配置、全局兜底）：
//   policy set [--dept 路径] [--allow-anonymous on|off]
//              [--allow-cross-state on|off] [--new-device-approval on|off]
//          不带 --dept 即全局行；未给的开关保留现值（新行为内置默认）。
//   policy list                        已配置行（全局在前）
//   policy show <账号>                 该账号生效策略（部门→上级部门→全局）
int cmd_policy(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server policy set [--dept 路径] [--allow-anonymous"
                 " on|off] [--allow-cross-state on|off] [--new-device-approval"
                 " on|off] [--allow-cross-dept-file on|off] "
                 "[--allow-forward-file on|off]"
                 " | policy list | policy show <账号> [--db <库>]\n";
    return 2;
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const std::string_view sub = argv[0];

  if (sub == "list") {
    std::cout << "范围\t免登录使用\t与未登录设备通信\t新设备登录需审批"
                 "\t直连文件跨部门\t直连文件再转发\n";
    for (const auto& p : store.policy_list()) {
      std::cout << (p.department_path.empty() ? "（全局默认）" : p.department_path)
                << '\t' << (p.allow_anonymous ? "允许" : "禁止") << '\t'
                << (p.allow_cross_state ? "允许" : "禁止") << '\t'
                << (p.new_device_approval ? "需审批" : "免审批") << '\t'
                << (p.allow_cross_dept_file ? "允许" : "禁止") << '\t'
                << (p.allow_forward_file ? "允许" : "禁止") << '\n';
    }
    return 0;
  }
  if (sub == "show") {
    if (argc < 2) {
      std::cerr << "用法：policy show <账号>\n";
      return 2;
    }
    const auto p = store.resolve_policy(argv[1]);
    std::cout << "账号：" << argv[1] << "\n生效范围："
              << (p.department_path.empty() ? "（全局默认）" : p.department_path)
              << "\n免登录使用：" << (p.allow_anonymous ? "允许" : "禁止")
              << "\n与未登录设备通信：" << (p.allow_cross_state ? "允许" : "禁止")
              << "\n新设备登录需审批："
              << (p.new_device_approval ? "需审批" : "免审批")
              << "\n直连文件跨部门："
              << (p.allow_cross_dept_file ? "允许" : "禁止")
              << "\n直连文件再转发："
              << (p.allow_forward_file ? "允许" : "禁止") << "\n";
    return 0;
  }
  if (sub == "set") {
    std::string dept;
    // 基线：目标范围已配置行保留现值，未配置从内置默认起步
    memex::server::PolicyRow row;
    const auto apply_flags = [&](int i) {
      for (; i < argc; ++i) {
        const std::string_view opt = argv[i];
        const auto value_on = [&](const char* name) -> int {
          if (i + 1 >= argc) {
            std::cerr << name << " 缺取值（on|off）\n";
            std::exit(2);
          }
          const std::string_view v = argv[++i];
          if (v == "on") return 1;
          if (v == "off") return 0;
          std::cerr << name << " 取值应为 on|off：" << v << "\n";
          std::exit(2);
        };
        if (opt == "--allow-anonymous") {
          row.allow_anonymous = value_on("--allow-anonymous") != 0;
        } else if (opt == "--allow-cross-state") {
          row.allow_cross_state = value_on("--allow-cross-state") != 0;
        } else if (opt == "--new-device-approval") {
          row.new_device_approval = value_on("--new-device-approval") != 0;
        } else if (opt == "--allow-cross-dept-file") {
          row.allow_cross_dept_file = value_on("--allow-cross-dept-file") != 0;
        } else if (opt == "--allow-forward-file") {
          row.allow_forward_file = value_on("--allow-forward-file") != 0;
        } else if (opt == "--dept") {
          // 预扫描已取走路径值，此处跳过（T3.4 起 --dept 部门行路径
          // 实际不可用——apply_flags 撞上即 exit(2)，本处顺修）
          if (i + 1 >= argc) {
            std::cerr << "--dept 缺路径\n";
            std::exit(2);
          }
          ++i;
        } else {
          std::cerr << "未知选项：" << opt << "\n";
          std::exit(2);
        }
      }
    };
    // 先扫 --dept（决定目标范围），再按目标范围取基线、套开关
    for (int i = 1; i < argc; ++i) {
      if (std::string_view(argv[i]) == "--dept") {
        if (i + 1 >= argc) {
          std::cerr << "--dept 缺路径\n";
          return 2;
        }
        dept = argv[++i];
      }
    }
    for (const auto& p : store.policy_list()) {
      if (p.department_path == dept) row = p;
    }
    apply_flags(1);
    if (!store.set_policy(dept, row.allow_anonymous, row.allow_cross_state,
                          row.new_device_approval, row.allow_cross_dept_file,
                          row.allow_forward_file)) {
      std::cerr << "策略写入失败（部门路径不存在？）：" << dept << "\n";
      return 1;
    }
    std::cout << "已配置策略：" << (dept.empty() ? "（全局默认）" : dept)
              << "（免登录 " << (row.allow_anonymous ? "允许" : "禁止")
              << "／跨态通信 " << (row.allow_cross_state ? "允许" : "禁止")
              << "／新设备 " << (row.new_device_approval ? "需审批" : "免审批")
              << "／直连文件跨部门 "
              << (row.allow_cross_dept_file ? "允许" : "禁止")
              << "／直连文件再转发 "
              << (row.allow_forward_file ? "允许" : "禁止") << "）\n";
    return 0;
  }
  std::cerr << "未知 policy 子命令：" << sub << "\n";
  return 2;
}
