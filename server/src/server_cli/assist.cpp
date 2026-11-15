// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"

namespace {

// assist 子命令族（平台-11 远程协助 Security Domain 模型，蓝图§二十七/§五）：
//   assist policy set <on|off> [--dept 路径] --by 账号   部门放行开关（默认禁）
//   assist policy list | policy show <账号>              配置行／生效口径
//   assist request <发起人> <受控方> --perms view,keyboard,mouse,clipboard,file
//   assist approve <id> --by 受控方 [--perms 实批子集]    consent 只属受控方
//   assist deny <id> --by 受控方
//   assist start <id> --by 当事方
//   assist end <id> --by 当事方 [--reason 文本]           受控方=撤权即时生效
//   assist show <id> | list [账号] | audit <id>
// 权限名：view keyboard mouse clipboard file（file_transfer 简写 file）
namespace {
int parse_assist_perms(const std::string& csv) {
  if (csv.empty()) return -1;
  using ST = memex::server::ServerStore;
  int mask = 0;
  std::size_t pos = 0;
  while (true) {
    const std::size_t comma = csv.find(',', pos);
    const std::string tok = csv.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (tok == "view") mask |= ST::kAssistView;
    else if (tok == "keyboard") mask |= ST::kAssistKeyboard;
    else if (tok == "mouse") mask |= ST::kAssistMouse;
    else if (tok == "clipboard") mask |= ST::kAssistClipboard;
    else if (tok == "file" || tok == "file_transfer")
      mask |= ST::kAssistFileTransfer;
    else return -1;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return mask;
}

std::string assist_perms_names(int mask) {
  using ST = memex::server::ServerStore;
  std::string out;
  const auto add = [&out](const char* n) {
    if (!out.empty()) out += ',';
    out += n;
  };
  if (mask & ST::kAssistView) add("view");
  if (mask & ST::kAssistKeyboard) add("keyboard");
  if (mask & ST::kAssistMouse) add("mouse");
  if (mask & ST::kAssistClipboard) add("clipboard");
  if (mask & ST::kAssistFileTransfer) add("file");
  return out;
}
} // namespace

} // namespace

int cmd_assist(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server assist policy set|list|show … | "
                 "request <发起人> <受控方> --perms 名单 | approve|deny|start|"
                 "end <id> --by 账号 | show <id> | list [账号] | audit <id>"
                 " [--db <库>]\n";
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

  if (sub == "policy") {
    if (argc < 2) {
      std::cerr << "用法：assist policy set <on|off> [--dept 路径] --by 账号"
                 " | policy list | policy show <账号>\n";
      return 2;
    }
    const std::string_view ps = argv[1];
    if (ps == "set") {
      bool allow = false;
      bool have_allow = false;
      std::string dept, by;
      int idx = 2; // on|off 位置参数紧随 set，其余按旗标对走
      if (idx < argc &&
          (std::string_view(argv[idx]) == "on" ||
           std::string_view(argv[idx]) == "off")) {
        allow = std::string_view(argv[idx]) == "on";
        have_allow = true;
        ++idx;
      }
      for (int i = idx; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (flag == "--dept") dept = argv[i + 1];
        else if (flag == "--by") by = argv[i + 1];
      }
      if (!have_allow || by.empty()) {
        std::cerr << "用法：assist policy set <on|off> [--dept 路径] --by 账号"
                     "（谁配置的须留痕）\n";
        return 2;
      }
      if (!store.assist_policy_set(allow, dept, by, now)) {
        std::cerr << "写入失败（部门须已存在）\n";
        return 1;
      }
      std::cout << "已配置远程协助：" << (dept.empty() ? "（全局）" : dept)
                << " → " << (allow ? "放行" : "禁止") << "（by " << by
                << "）\n";
      return 0;
    }
    if (ps == "list") {
      std::cout << "scope\t放行\t配置人\t配置时刻\n";
      for (const auto& p : store.assist_policy_list()) {
        std::cout << (p.department_path.empty() ? "（全局）"
                                                : p.department_path)
                  << '\t' << (p.allow ? "允许" : "禁止") << '\t'
                  << p.updated_by << '\t' << p.updated_ms << '\n';
      }
      return 0;
    }
    if (ps == "show") {
      if (argc < 3) {
        std::cerr << "用法：assist policy show <账号>\n";
        return 2;
      }
      const bool ok = store.assist_policy_resolve(argv[2]);
      std::cout << argv[2] << " 生效远程协助策略："
                << (ok ? "放行" : "禁止（未配置默认禁止）") << "\n";
      return 0;
    }
    std::cerr << "未知 assist policy 子命令：" << ps << "\n";
    return 2;
  }

  if (sub == "request") {
    if (argc < 4) {
      std::cerr << "用法：assist request <发起人> <受控方> --perms "
                 "view[,keyboard[,mouse[,clipboard[,file]]]]\n";
      return 2;
    }
    const std::string requester = argv[1];
    const std::string target = argv[2];
    std::string perms;
    for (int i = 3; i + 1 < argc; i += 2)
      if (std::string_view(argv[i]) == "--perms") perms = argv[i + 1];
    const int mask = parse_assist_perms(perms);
    if (mask < 0) {
      std::cerr << "权限名非法（可用：view keyboard mouse clipboard file）\n";
      return 2;
    }
    const std::string id = store.assist_request(requester, target, mask, now);
    if (id.empty()) {
      std::cerr << "发起被拒（两账号须存在且不同、双方部门均须放行、"
                   "权限集须合法）\n";
      return 1;
    }
    std::cout << "已发起远程协助会话：" << id << "（" << requester << " → "
              << target << "，申请权限 " << assist_perms_names(mask)
              << "；待受控方批准）\n";
    return 0;
  }

  if (sub == "approve" || sub == "deny" || sub == "start" || sub == "end") {
    if (argc < 3) {
      std::cerr << "用法：assist " << sub << " <id> --by 账号"
                << (sub == "approve" ? " [--perms 实批子集]"
                    : sub == "end" ? " [--reason 文本]" : "")
                << "\n";
      return 2;
    }
    const std::string id = argv[1];
    std::string by, perms, reason;
    for (int i = 2; i + 1 < argc; i += 2) {
      const std::string_view flag = argv[i];
      if (flag == "--by") by = argv[i + 1];
      else if (flag == "--perms") perms = argv[i + 1];
      else if (flag == "--reason") reason = argv[i + 1];
    }
    if (by.empty()) {
      std::cerr << "缺 --by 账号（谁动的须留痕）\n";
      return 2;
    }
    bool ok = false;
    if (sub == "approve") {
      const auto s = store.assist_session(id);
      if (!s) {
        std::cerr << "会话不存在：" << id << "\n";
        return 1;
      }
      int granted = s->requested_mask; // 缺省=全量申请集
      if (!perms.empty()) {
        const int m = parse_assist_perms(perms);
        if (m < 0) {
          std::cerr << "权限名非法（可用：view keyboard mouse clipboard "
                       "file）\n";
          return 2;
        }
        granted = m;
      }
      ok = store.assist_approve(id, by, granted, now);
      if (ok)
        std::cout << "已批准：" << id << "（授出 " << assist_perms_names(granted)
                  << "）\n";
    } else if (sub == "deny") {
      ok = store.assist_deny(id, by, now);
      if (ok) std::cout << "已拒绝：" << id << "\n";
    } else if (sub == "start") {
      ok = store.assist_start(id, by, now);
      if (ok) std::cout << "已开始：" << id << "\n";
    } else {
      ok = store.assist_end(id, by, reason, now);
      if (ok)
        std::cout << "已结束：" << id << (reason.empty() ? "" : "（" + reason + "）")
                  << "\n";
    }
    if (!ok) {
      std::cerr << "操作被拒（状态机不允许／consent 只属受控方／终态不可再动）\n";
      return 1;
    }
    return 0;
  }

  if (sub == "show") {
    if (argc < 2) {
      std::cerr << "用法：assist show <id>\n";
      return 2;
    }
    const auto s = store.assist_session(argv[1]);
    if (!s) {
      std::cerr << "会话不存在：" << argv[1] << "\n";
      return 1;
    }
    std::cout << "会话 " << s->id << "：" << s->requester << " → " << s->target
              << "，状态 " << s->status << "\n"
              << "  申请权限 " << assist_perms_names(s->requested_mask)
              << "，实批 " << assist_perms_names(s->granted_mask) << "\n"
              << "  发起 " << s->requested_ms << "，批准 " << s->approved_ms
              << "，开始 " << s->started_ms << "，结束 " << s->ended_ms << "\n";
    if (!s->end_actor.empty())
      std::cout << "  终态操作 " << s->end_actor << "："
                << (s->end_reason.empty() ? "（无理由）" : s->end_reason)
                << "\n";
    return 0;
  }

  if (sub == "list") {
    const std::string who = argc >= 2 ? argv[1] : "";
    std::cout << "id\t发起\t受控\t状态\t申请\t实批\t发起时刻\n";
    for (const auto& s : store.assist_sessions(who)) {
      std::cout << s.id << '\t' << s.requester << '\t' << s.target << '\t'
                << s.status << '\t' << assist_perms_names(s.requested_mask)
                << '\t' << assist_perms_names(s.granted_mask) << '\t'
                << s.requested_ms << '\n';
    }
    return 0;
  }

  if (sub == "audit") {
    if (argc < 2) {
      std::cerr << "用法：assist audit <id>\n";
      return 2;
    }
    std::cout << "id\t会话\t操作人\t动作\t详情\t时刻\n";
    for (const auto& a : store.assist_audits(argv[1])) {
      std::cout << a.id << '\t'
                << (a.session_id.empty() ? "（策略面）" : a.session_id) << '\t'
                << a.actor << '\t' << a.action << '\t' << a.detail << '\t'
                << a.ts_ms << '\n';
    }
    return 0;
  }

  std::cerr << "未知 assist 子命令：" << sub << "\n";
  return 2;
}
