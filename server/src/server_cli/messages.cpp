// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"


// messages [账号] [--keyword K] [--since 时刻] [--until 时刻] [--limit N]
//          [--export 文件] [--as 账号]：管理员检索归档（T3.2：条件检索＋
//          导出留证＋查阅日志）。平台-6：--as 账号化审计——账号须持
//          auditor 有效角色（SystemAdmin 不自动可读消息），操作者记该
//          账号；不带 --as＝本地运维信任路径（操作者记系统用户名）。
// 撤回消息原文照常可见并标「已撤回」——留痕纪律：撤回仅置标记不清正文。
int cmd_messages(int argc, char** argv, const std::string& db_path) {
  std::string account;
  std::string keyword;
  std::string export_path;
  std::string as_account; // 平台-6：操作者账号化（审计台账记人、持证校验）
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
    } else if (arg == "--as" && i + 1 < argc) {
      as_account = argv[++i];
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
  const auto now_gate = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
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

  // 平台-6 审计独立角色（蓝图§十六）：SecurityAuditor≠SystemAdmin——
  // 显式账号化（--as）走持证校验，admin 有效角色不自动可读消息；
  // 被拒尝试同样留痕（审计自身被拒可对账）。不带 --as＝本地运维信任
  // 路径（现状口径，操作者记系统用户名）。
  std::string operator_account = cli_operator();
  if (!as_account.empty()) {
    const auto roles = store.effective_roles(as_account, now_gate);
    const bool is_auditor =
        std::find(roles.begin(), roles.end(), "auditor") != roles.end();
    if (!is_auditor) {
      memex::server::AuditReadRow denied;
      denied.op_account = as_account;
      denied.action = "audit.denied";
      denied.filters = filters;
      denied.ts_ms = now_gate;
      store.add_audit_read(denied);
      std::cerr << "被拒：" << as_account
                << " 不持 auditor 有效角色（SystemAdmin 不自动可读消息，"
                   "须经授权授予 auditor）；被拒尝试已留痕\n";
      return 1;
    }
    operator_account = as_account;
  }
  memex::server::MessageSearch q;
  q.account = account;
  q.keyword = keyword;
  q.since_ms = since_ms;
  q.until_ms = until_ms;
  q.limit = limit;
  const auto rows = store.search_messages(q);

  // 归档起点（T4.2／A8）：显示为该账号「实际登录时间」——进入协作态才开始
  // 归档，跨态直连会话不进服务端（无归档则如实显示）。
  std::string archive_origin;
  if (!account.empty()) {
    const auto start_ms = store.archive_start_ms(account);
    if (start_ms > 0) {
      std::time_t secs = static_cast<std::time_t>(start_ms / 1000);
      std::tm tm{};
      local_time(secs, &tm);
      char when[24];
      std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
      archive_origin = std::string("归档自 ") + when +
                       "（协作态登录起；此前直连会话不进归档）";
    } else {
      archive_origin = "归档自 —（该账号暂无归档消息）";
    }
    std::cout << archive_origin << '\n';
  }

  // 过滤条件摘要（进查阅日志；只记条件，不记消息内容）——已在判权闸前算好
  auto format_row = [](const memex::server::ArchivedMessage& m) {
    std::time_t secs = static_cast<std::time_t>(m.ts_ms / 1000);
    std::tm tm{};
    local_time(secs, &tm);
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
  // 平台-6：审计动作规范名（蓝图§十六 message.search/message.export）
  const std::string action =
      export_path.empty() ? "audit.message.search" : "audit.message.export";
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
        << "\n# 操作者：" << operator_account << "\n# 过滤条件："
        << (filters.empty() ? "（全部）" : filters) << "\n";
    if (!archive_origin.empty()) out << "# " << archive_origin << "\n";
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
  audit.op_account = operator_account;
  audit.action = action;
  audit.filters = filters;
  audit.result_count = static_cast<int>(rows.size());
  audit.ts_ms = now;
  store.add_audit_read(audit);
  return 0;
}


// cross [N]：跨态会话日志（T4.2）——时间/双方/时长，不含任何消息内容。
// 状态：已结束（带时长）／进行中（登出等未闭环，时长 0）。
int cmd_cross(int argc, char** argv, const std::string& db_path) {
  int limit = 200;
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
  std::cout << "id\t账号\t对端设备\t对端名称\t建立时间(ms)\t结束时间(ms)\t"
               "时长(ms)\t状态\n";
  for (const auto& r : store.cross_logs(limit)) {
    std::cout << r.id << '\t' << r.account << '\t' << r.peer_device << '\t'
              << r.peer_name << '\t' << r.started_ms << '\t' << r.ended_ms
              << '\t' << r.duration_ms << '\t'
              << (r.ended_ms > 0 ? "已结束" : "进行中") << '\n';
  }
  return 0;
}


// favs <账号>：常用联系人（T4.5）——星标置顶＋最近排序，落服务端换机保留
int cmd_favs(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server favs <账号> [--db <库>]\n";
    return 2;
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  std::cout << "对端\t星标\t最近联系(ms)\n";
  for (const auto& f : store.fav_list(argv[0])) {
    std::cout << f.peer << '\t' << (f.starred ? "是" : "否") << '\t'
              << f.last_ms << '\n';
  }
  return 0;
}
