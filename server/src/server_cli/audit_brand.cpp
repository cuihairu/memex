// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"


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
    local_time(secs, &tm);
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
// 平台-3 组织三关联（权威表多值/带时间窗，member_profiles 单值列作镜像）：
//   org membership add|remove <账号> <部门路径> | list <账号>
//                                  多部门归属（org_memberships）
//   org role grant <账号> <role> [--scope 路径] [--from 毫秒]
//       [--until 毫秒] [--by 操作者] | revoke <id> | list [账号]
//                                  追加授权台账（org_role_assignments；
//                                  有效角色=基础∪窗内授权）
//   org reporting set <账号> <上级账号> | clear <账号>
//                                  汇报线（org_reporting_lines；环防御）
// 通讯录可见性（T4.6）：
//   org hide/unhide <账号>         成员隐藏／恢复（管理员、本人与白名单仍可见）
//   org dept hide/unhide <路径>    部门整树隐藏（部门内自己人仍互见）
//   org dept restrict/unrestrict <路径>  部门限看本部门／解除
//   org fields <账号> --hide title,manager[,role] | --clear  敏感字段脱敏
//   org allow/disallow <查看者> --see <账号|部门路径>         白名单例外
//   org visibility list            已配置行与白名单一览
// 品牌物料（设计稿 docs/design/品牌物料.md）：运维兜底面（store 直写，
// 不经 HTTP）；PNG 校验与路由层同口径（magic/512KiB/IHDR≤2048²）
int cmd_branding(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server branding show --db <库>\n"
              << "      memex_server branding set --db <库> [--company 名称]"
                 " [--accent '#rrggbb'] [--slogan 文案] [--logo FILE.png]"
                 " [--splash FILE.png] [--clear-logo] [--clear-splash]\n";
    return 2;
  }
  const std::string_view sub = argv[0];
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const auto now =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  if (sub == "show") {
    const auto b = store.branding_get();
    std::cout << "company_name: " << (b.company_name.empty() ? "（未配）"
                                                             : b.company_name)
              << "\n"
              << "accent: " << (b.accent.empty() ? "（未配）" : b.accent)
              << "\n"
              << "slogan: " << (b.slogan.empty() ? "（未配）" : b.slogan)
              << "\n"
              << "logo: " << (b.logo.empty() ? "（未配）"
                                             : std::to_string(b.logo.size()) +
                                                   " 字节")
              << "\n"
              << "splash: " << (b.splash.empty() ? "（未配）"
                                                 : std::to_string(b.splash.size()) +
                                                       " 字节")
              << "\n"
              << "version: " << b.version << "\n";
    return 0;
  }
  if (sub != "set") {
    std::cerr << "未知子命令：" << sub << "（show | set）\n";
    return 2;
  }
  std::optional<std::string> company, accent, slogan;
  bool clear_logo = false, clear_splash = false;
  std::string logo_path, splash_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view opt = argv[i];
    if (opt == "--company") company = argv[++i];
    else if (opt == "--accent") accent = argv[++i];
    else if (opt == "--slogan") slogan = argv[++i];
    else if (opt == "--logo") logo_path = argv[++i];
    else if (opt == "--splash") splash_path = argv[++i];
    else if (opt == "--clear-logo") clear_logo = true;
    else if (opt == "--clear-splash") clear_splash = true;
    else {
      std::cerr << "未知选项：" << opt << "\n";
      return 2;
    }
  }
  if (accent.has_value() && !accent->empty()) {
    bool ok = accent->size() == 7 && (*accent)[0] == '#';
    for (std::size_t i = 1; ok && i < 7; ++i) {
      const char c = (*accent)[i];
      ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
    }
    if (!ok) {
      std::cerr << "accent 须为 #rrggbb 形态\n";
      return 1;
    }
  }
  // PNG 读入＋校验（与路由层同口径；CLI 失败非零退出明示原因）
  const auto read_png = [](const std::string& path,
                           std::vector<unsigned char>* out) -> std::string {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "素材文件打不开：" + path;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    static const unsigned char kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                               0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 8 ||
        std::memcmp(bytes.data(), kPngMagic, 8) != 0) {
      return "素材须为 PNG 格式：" + path;
    }
    if (bytes.size() > 512 * 1024) return "素材超大小上限（512KiB）：" + path;
    if (bytes.size() >= 24) {
      const auto be32 = [&](std::size_t off) {
        return (static_cast<std::uint32_t>(bytes[off]) << 24) |
               (static_cast<std::uint32_t>(bytes[off + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[off + 2]) << 8) |
               static_cast<std::uint32_t>(bytes[off + 3]);
      };
      if (be32(16) == 0 || be32(20) == 0 || be32(16) > 2048 ||
          be32(20) > 2048) {
        return "素材尺寸超限（≤2048×2048）：" + path;
      }
    }
    *out = std::move(bytes);
    return "";
  };
  if (!logo_path.empty()) {
    std::vector<unsigned char> bytes;
    if (const std::string err = read_png(logo_path, &bytes); !err.empty()) {
      std::cerr << err << "\n";
      return 1;
    }
    store.branding_set_logo(bytes, now);
    std::cout << "logo 已更新\n";
  }
  if (!splash_path.empty()) {
    std::vector<unsigned char> bytes;
    if (const std::string err = read_png(splash_path, &bytes); !err.empty()) {
      std::cerr << err << "\n";
      return 1;
    }
    store.branding_set_splash(bytes, now);
    std::cout << "splash 已更新\n";
  }
  if (clear_logo) {
    store.branding_clear_logo();
    std::cout << "logo 已清除\n";
  }
  if (clear_splash) {
    store.branding_clear_splash();
    std::cout << "splash 已清除\n";
  }
  if (company.has_value() || accent.has_value() || slogan.has_value()) {
    const std::int64_t v = store.branding_set(company, accent, slogan, now);
    if (v < 0) {
      std::cerr << "品牌配置写入失败\n";
      return 1;
    }
    std::cout << "品牌文本已更新（version " << v << "）\n";
  }
  return 0;
}
