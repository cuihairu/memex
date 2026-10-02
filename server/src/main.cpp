// MemexServer 入口：serve（默认）与账号／登录记录管理子命令。
// 管理后台（T3.1）接管运维面之前，账号开通与记录查询走本 CLI。
#include <asio.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <memex/protocol/messages.hpp>

#include "server.hpp"
#include "store.hpp"

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace {

// 服务端默认监听端口。评审报告只固定了直连态端口段（UDP 2425–2436／TCP 2426–2437），
// 服务端监听端口未钉死，此默认值待与网络管理侧确认后写入部署文档。
constexpr std::uint16_t kDefaultPort = 24360;
constexpr const char* kDefaultDb = "memex-server.db";

// db_path 由 main 统一解析（--db 剥离会就地改写 argv，不能再从 argv 复读）
int cmd_serve(int argc, char** argv, const std::string& db_path) {
  std::uint16_t port = kDefaultPort;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
      if (port == 0) {
        std::cerr << "无效端口\n";
        return 2;
      }
    }
  }

  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }

  try {
    asio::io_context io;
    memex::server::CollabServer server(io, store, port);
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](std::error_code, int sig) {
      std::cout << "[MEMEX] 收到信号 " << sig << "，退出" << std::endl;
      io.stop();
    });
    server.start_accept();
    io.run();
  } catch (const std::exception& e) {
    std::cerr << "服务端异常退出：" << e.what() << std::endl;
    return 1;
  }
  return 0;
}

int cmd_account(int argc, char** argv, const std::string& db_path) {
  // account add <账号> <口令> [--name 显示名] [--role admin|member]
  // account list：成员维护面（账号／展示名／角色）
  if (argc >= 1 && std::string_view(argv[0]) == "list") {
    memex::server::ServerStore store;
    if (!store.open(db_path)) {
      std::cerr << "本地库打开失败：" << db_path << "\n";
      return 1;
    }
    std::cout << "账号\t展示名（角色）\n";
    for (const auto& [acct, label] : store.account_list()) {
      std::cout << acct << '\t' << label << '\n';
    }
    return 0;
  }
  if (argc < 3 || std::string_view(argv[0]) != "add") {
    std::cerr << "用法：memex_server account add <账号> <口令> [--name 显示名] "
                 "[--role admin|member] | account list [--db <库>]\n";
    return 2;
  }
  const std::string account = argv[1];
  const std::string password = argv[2];
  std::string display_name = account;
  std::string role = "member";
  for (int i = 3; i + 1 < argc; ++i) {
    const std::string_view opt = argv[i];
    if (opt == "--name") {
      display_name = argv[++i];
    } else if (opt == "--role") {
      role = argv[++i];
    }
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  if (!store.create_account(account, password, display_name, role)) {
    std::cerr << "建号失败（账号已存在或写库失败）：" << account << "\n";
    return 1;
  }
  std::cout << "已建号：" << account << "（" << display_name << "，" << role
            << "）\n";
  return 0;
}

int cmd_logins(int argc, char** argv, const std::string& db_path) {
  // logins [账号] [--device 指纹前缀]：全量可查（不带账号=全部，倒序）
  std::string account;
  std::string fp_prefix;
  for (int i = 0; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--device" && i + 1 < argc) {
      fp_prefix = argv[++i];
    } else {
      account = argv[i];
    }
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const auto rows = store.login_records(account, fp_prefix);
  std::cout << "id\t账号\t结果\t设备类型\t设备名\t指纹前8\t来源\t版本\t时间(ms)\n";
  for (const auto& r : rows) {
    std::cout << r.id << '\t' << r.account << '\t' << r.result << '\t'
              << r.kind << '\t' << r.name << '\t'
              << r.fingerprint.substr(0, 8) << '\t' << r.source_ip << '\t'
              << r.version << '\t' << r.ts_ms << '\n';
  }
  return 0;
}

// device 子命令族（T3.3 设备台账）：
//   device list                          台账（指纹／类型／名称／责任人／状态／活跃）
//   device set <指纹前缀> --owner 账号    责任人登记
//   device disable <指纹前缀>            停用（该设备登录即拒）
//   device enable <指纹前缀>             重新启用
//   device unbind <指纹前缀>             解绑（清责任人并停用）
int cmd_device(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server device list | set <指纹前缀> --owner 账号 | "
                 "disable <前缀> | enable <前缀> | unbind <前缀> [--db <库>]\n";
    return 2;
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const std::string_view sub = argv[0];

  if (sub == "list") {
    std::cout << "指纹(前16)\t类型\t名称\t责任人\t状态\t首见(ms)\t最近活跃(ms)\n";
    for (const auto& d : store.device_list()) {
      std::cout << d.fingerprint.substr(0, 16) << '\t' << d.kind << '\t'
                << d.name << '\t'
                << (d.owner_account.empty() ? "（未登记）" : d.owner_account)
                << '\t' << (d.enabled ? "启用" : "停用") << '\t'
                << d.first_seen_ms << '\t' << d.last_seen_ms << '\n';
    }
    return 0;
  }

  // 其余子命令都要指纹前缀；统一解析（≥8 位，多义拒绝并提示补长）
  if (argc < 2) {
    std::cerr << "用法：memex_server device set <前缀> --owner 账号 | "
                 "disable <前缀> | enable <前缀> | unbind <前缀>\n";
    return 2;
  }
  const std::string prefix = argv[1];
  const auto [fp, ambiguous] = store.device_by_prefix(prefix);
  if (fp.empty()) {
    if (ambiguous) {
      std::cerr << "指纹前缀多义，请补长后重试：" << prefix << "\n";
      return 1;
    }
    std::cerr << "前缀过短（≥8 位）或无此设备：" << prefix << "\n";
    return 1;
  }

  if (sub == "set") {
    std::string owner;
    for (int i = 2; i + 1 < argc; ++i) {
      if (std::string_view(argv[i]) == "--owner") owner = argv[++i];
    }
    if (owner.empty()) {
      std::cerr << "用法：device set <前缀> --owner <账号>\n";
      return 2;
    }
    if (!store.set_device_owner(fp, owner)) {
      std::cerr << "责任人登记失败（账号不存在或设备不存在）：" << owner << "\n";
      return 1;
    }
    std::cout << "已登记责任人：" << fp.substr(0, 16) << "… → " << owner << "\n";
    return 0;
  }
  if (sub == "disable" || sub == "enable") {
    const bool enable = sub == "enable";
    if (!store.set_device_enabled(fp, enable)) {
      std::cerr << (enable ? "启用" : "停用") << "失败（设备不存在）\n";
      return 1;
    }
    std::cout << (enable ? "已启用：" : "已停用：") << fp.substr(0, 16) << "…\n";
    return 0;
  }
  if (sub == "unbind") {
    if (!store.unbind_device(fp)) {
      std::cerr << "解绑失败（设备不存在）\n";
      return 1;
    }
    std::cout << "已解绑并停用：" << fp.substr(0, 16) << "…\n";
    return 0;
  }
  std::cerr << "未知 device 子命令：" << sub << "\n";
  return 2;
}

// 时间参数解析（T3.2）："YYYY-MM-DD" 或 "YYYY-MM-DD HH:MM[:SS]"（本地时区）。
// date_only 端补零点／当日末秒，使日期粒度的开闭区间语义正确。失败返回 -1。
std::int64_t parse_time_arg(const std::string& text, bool end_of_day) {
  std::tm tm{};
  const char* fmts[] = {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"};
  bool ok = false;
  for (const char* f : fmts) {
    if (strptime(text.c_str(), f, &tm) != nullptr) {
      ok = true;
      break;
    }
  }
  if (!ok) return -1;
  if (end_of_day) {
    if (tm.tm_hour == 0 && tm.tm_min == 0 && tm.tm_sec == 0) {
      tm.tm_hour = 23;
      tm.tm_min = 59;
      tm.tm_sec = 59;
    }
  }
  tm.tm_isdst = -1;
  return static_cast<std::int64_t>(mktime(&tm)) * 1000;
}

// 查阅操作者：管理 CLI 由运维在本机执行，取系统用户名留痕；拿不到记 "cli"。
std::string cli_operator() {
  const char* user = std::getenv("USER");
  if (!user || !*user) user = std::getenv("LOGNAME");
  return (user && *user) ? std::string(user) : std::string("cli");
}

// messages [账号] [--keyword K] [--since 时刻] [--until 时刻] [--limit N]
//          [--export 文件]：管理员检索归档（T3.2：条件检索＋导出留证＋查阅日志）。
// 撤回消息原文照常可见并标「已撤回」——留痕纪律：撤回仅置标记不清正文。
int cmd_messages(int argc, char** argv, const std::string& db_path) {
  std::string account;
  std::string keyword;
  std::string export_path;
  std::int64_t since_ms = 0, until_ms = 0;
  int limit = 200;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--limit" && i + 1 < argc) {
      limit = std::atoi(argv[++i]);
      if (limit <= 0) {
        std::cerr << "无效 limit\n";
        return 2;
      }
    } else if (arg == "--keyword" && i + 1 < argc) {
      keyword = argv[++i];
    } else if (arg == "--export" && i + 1 < argc) {
      export_path = argv[++i];
    } else if (arg == "--since" && i + 1 < argc) {
      since_ms = parse_time_arg(argv[++i], false);
      if (since_ms < 0) {
        std::cerr << "无效 --since（应为 YYYY-MM-DD[ HH:MM[:SS]]）\n";
        return 2;
      }
    } else if (arg == "--until" && i + 1 < argc) {
      until_ms = parse_time_arg(argv[++i], true);
      if (until_ms < 0) {
        std::cerr << "无效 --until（应为 YYYY-MM-DD[ HH:MM[:SS]]）\n";
        return 2;
      }
    } else {
      account = argv[i];
    }
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  memex::server::MessageSearch q;
  q.account = account;
  q.keyword = keyword;
  q.since_ms = since_ms;
  q.until_ms = until_ms;
  q.limit = limit;
  const auto rows = store.search_messages(q);

  // 过滤条件摘要（进查阅日志；只记条件，不记消息内容）
  std::string filters;
  auto append_filter = [&filters](const std::string& kv) {
    if (!filters.empty()) filters += " ";
    filters += kv;
  };
  if (!account.empty()) append_filter("账号=" + account);
  if (!keyword.empty()) append_filter("关键词=" + keyword);
  if (since_ms > 0) append_filter("起=" + std::to_string(since_ms));
  if (until_ms > 0) append_filter("止=" + std::to_string(until_ms));

  auto format_row = [](const memex::server::ArchivedMessage& m) {
    std::time_t secs = static_cast<std::time_t>(m.ts_ms / 1000);
    std::tm tm{};
    localtime_r(&secs, &tm);
    char when[24];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    std::ostringstream line;
    line << m.msg_id << '\t' << m.from_account << '\t' << m.to_account << '\t'
         << memex::protocol::msg_type_name(
                static_cast<memex::protocol::MsgType>(m.type))
         << '\t' << (m.recalled ? "已撤回" : "正常") << '\t' << when << '\t'
         << m.text;
    return line.str();
  };

  const std::string header = "msg_id\t发送方\t接收方\t类型\t状态\t时间\t正文";
  const std::string action = export_path.empty() ? "检索" : "导出";
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  if (!export_path.empty()) {
    // 导出留证：完整结果写文件（含导出时间／操作者／条件，便于对账）
    std::ofstream out(export_path, std::ios::trunc);
    if (!out) {
      std::cerr << "导出文件无法写入：" << export_path << "\n";
      return 1;
    }
    out << "# Memex 归档导出\n# 导出时间(ms)：" << now
        << "\n# 操作者：" << cli_operator() << "\n# 过滤条件："
        << (filters.empty() ? "（全部）" : filters) << "\n";
    for (const auto& m : rows) out << format_row(m) << '\n';
    out << "共 " << rows.size() << " 条\n";
  } else {
    std::cout << header << '\n';
    for (const auto& m : rows) std::cout << format_row(m) << '\n';
  }
  std::cout << "共 " << rows.size() << " 条\n";
  if (!export_path.empty()) {
    std::cout << "已导出至：" << export_path << "\n";
  }

  // 查阅行为记日志（每次检索／导出都落一条；audit 子命令可查）
  memex::server::AuditReadRow audit;
  audit.op_account = cli_operator();
  audit.action = action;
  audit.filters = filters;
  audit.result_count = static_cast<int>(rows.size());
  audit.ts_ms = now;
  store.add_audit_read(audit);
  return 0;
}

// audit [N]：查阅日志（倒序）——谁、何时、检索还是导出、用了什么条件、命中几条
int cmd_audit(int argc, char** argv, const std::string& db_path) {
  int limit = 100;
  if (argc >= 1) {
    limit = std::atoi(argv[0]);
    if (limit <= 0) {
      std::cerr << "无效条数\n";
      return 2;
    }
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  std::cout << "时间\t操作者\t动作\t过滤条件\t命中条数\n";
  for (const auto& r : store.audit_reads(limit)) {
    std::time_t secs = static_cast<std::time_t>(r.ts_ms / 1000);
    std::tm tm{};
    localtime_r(&secs, &tm);
    char when[24];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    std::cout << when << '\t' << r.op_account << '\t' << r.action << '\t'
              << (r.filters.empty() ? "（全部）" : r.filters) << '\t'
              << r.result_count << '\n';
  }
  return 0;
}

// org 子命令族（T2.6 组织架构）：
//   org dept add <路径>            建部门（"公司/研发部/客户端组" 逐级创建）
//   org dept list                  部门树（全路径列表）
//   org member <账号>              成员详情＋直属上级链路逐级上溯
//   org set <账号> [--dept 路径] [--title 职务] [--manager 账号|none]
//   org import <CSV>               批量导入（账号,部门,职务,直属上级；
//                                  错误行校验拒绝并报告行号）
int cmd_org(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server org dept add <路径> | dept list | "
                 "member <账号> | set … | import <CSV> [--db <库>]\n";
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
    std::cerr << "用法：org dept add <路径> | org dept list\n";
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
    if (!p) {
      std::cerr << "成员未建档：" << argv[1] << "\n";
      return 1;
    }
    std::cout << "账号：" << p->account << "（" << p->display_name << "）\n"
              << "部门：" << (p->department_path.empty() ? "（未分配）"
                                                         : p->department_path)
              << "\n职务：" << (p->title.empty() ? "（无）" : p->title) << "\n"
              << "直属上级：" << (p->manager.empty() ? "（无）" : p->manager)
              << "\n";
    const auto chain = store.manager_chain(argv[1]);
    if (!chain.empty()) {
      std::cout << "上级链路（逐级上溯）：";
      for (std::size_t i = 0; i < chain.size(); ++i) {
        std::cout << (i ? " → " : "") << chain[i];
      }
      std::cout << "\n";
    }
    return 0;
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
                 " on|off] | policy list | policy show <账号> [--db <库>]\n";
    return 2;
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const std::string_view sub = argv[0];

  if (sub == "list") {
    std::cout << "范围\t免登录使用\t与未登录设备通信\t新设备登录需审批\n";
    for (const auto& p : store.policy_list()) {
      std::cout << (p.department_path.empty() ? "（全局默认）" : p.department_path)
                << '\t' << (p.allow_anonymous ? "允许" : "禁止") << '\t'
                << (p.allow_cross_state ? "允许" : "禁止") << '\t'
                << (p.new_device_approval ? "需审批" : "免审批") << '\n';
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
              << (p.new_device_approval ? "需审批" : "免审批") << "\n";
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
                          row.new_device_approval)) {
      std::cerr << "策略写入失败（部门路径不存在？）：" << dept << "\n";
      return 1;
    }
    std::cout << "已配置策略：" << (dept.empty() ? "（全局默认）" : dept)
              << "（免登录 " << (row.allow_anonymous ? "允许" : "禁止")
              << "／跨态通信 " << (row.allow_cross_state ? "允许" : "禁止")
              << "／新设备 " << (row.new_device_approval ? "需审批" : "免审批")
              << "）\n";
    return 0;
  }
  std::cerr << "未知 policy 子命令：" << sub << "\n";
  return 2;
}

int self_test() {
  asio::io_context io;
  asio::ip::tcp::acceptor a(
      io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  std::cout << "self-test ok: listen 127.0.0.1:" << a.local_endpoint().port()
            << ", protocol " << memex::protocol::msg_type_name(
                                   memex::protocol::v1::HELLO)
            << std::endl;
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view cmd = argv[1];
    if (cmd == "--version") {
      std::cout << "memex-server " << MEMEX_VERSION << std::endl;
      return 0;
    }
    if (cmd == "--self-test") return self_test();

    // 子命令统一支持 --db 覆盖库路径
    std::string db_path = kDefaultDb;
    int sub_argc = 0;
    char** sub_argv = argv + 2;
    char** sub_end = argv + argc;
    for (char** p = sub_argv; p < sub_end; ++p) {
      if (std::string_view(*p) == "--db" && p + 1 < sub_end) {
        db_path = *++p;
      } else {
        sub_argv[sub_argc++] = *p;
      }
    }

    if (cmd == "serve") return cmd_serve(sub_argc, sub_argv, db_path);
    if (cmd == "account") return cmd_account(sub_argc, sub_argv, db_path);
    if (cmd == "logins") return cmd_logins(sub_argc, sub_argv, db_path);
    if (cmd == "device") return cmd_device(sub_argc, sub_argv, db_path);
    if (cmd == "messages") return cmd_messages(sub_argc, sub_argv, db_path);
    if (cmd == "audit") return cmd_audit(sub_argc, sub_argv, db_path);
    if (cmd == "org") return cmd_org(sub_argc, sub_argv, db_path);
    if (cmd == "policy") return cmd_policy(sub_argc, sub_argv, db_path);
    std::cerr << "未知子命令：" << cmd << "\n"
              << "用法：memex_server [serve [--port N] [--db P]] | account add … | "
                 "logins [账号] [--device 指纹前缀] | device … | "
                 "messages [账号] [--keyword K] [--since T] "
                 "[--until T] [--limit N] [--export 文件] | audit [N] | org … | "
                 "--version | --self-test\n";
    return 2;
  }
  return cmd_serve(0, argv, kDefaultDb);
}
