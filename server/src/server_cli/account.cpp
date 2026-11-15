// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"


// 平台-2 身份绑定（IdentityBinding：外部身份↔本地账号；SSO 面未接前
// 先立模型与维护口）：
//   identity bind <issuer> <subject> <account>   绑定（issuer+subject 唯一）
//   identity unbind <id>                          解绑
//   identity list [账号]                          绑定一览（空=全部）
int cmd_identity(int argc, char** argv, const std::string& db_path) {
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const auto ts =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  if (argc >= 4 && std::string_view(argv[0]) == "bind") {
    const auto id = store.identity_bind(argv[3], argv[1], argv[2], ts);
    if (id == 0) {
      std::cerr << "绑定失败（账号不存在/已绑同源同主体）：" << argv[1]
                << " → " << argv[3] << "\n";
      return 1;
    }
    std::cout << "已绑定：" << argv[1] << "/" << argv[2] << " → " << argv[3]
              << "（id " << id << "）\n";
    return 0;
  }
  if (argc >= 2 && std::string_view(argv[0]) == "unbind") {
    if (!store.identity_unbind(std::atoll(argv[1]))) {
      std::cerr << "解绑失败（无此绑定 id）：" << argv[1] << "\n";
      return 1;
    }
    std::cout << "已解绑 id " << argv[1] << "\n";
    return 0;
  }
  if (argc >= 1 && std::string_view(argv[0]) == "list") {
    const std::string account = argc >= 2 ? argv[1] : "";
    for (const auto& b : store.identity_list(account)) {
      std::cout << b.id << '\t' << b.account << '\t' << b.issuer << '/'
                << b.subject << '\n';
    }
    return 0;
  }
  std::cerr << "用法：memex_server identity bind <issuer> <subject> <账号> | "
               "unbind <id> | list [账号] [--db <库>]\n";
  return 2;
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
