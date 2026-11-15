// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"


// 机器人（bot）台账：add 建 token（明文仅此一次，库内存 sha256 摘要）／
// list 一览／remove 删除（连带退全部群）／disable|enable 开关／join|leave
// 群成员维护。bot 全名="bot:<name>" 伪账号：加群后收群消息（TEXT 扇出自动
// 入队）、可群发；单聊 to=bot:<name> 直达。收发走 webhook 端口 /bot/*。
int cmd_bot(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server bot add <name> --by <账号> | list | "
                 "remove <name> | disable|enable <name> | join|leave "
                 "<name> <群号> | allow|deny <name> --group <群号>|--dept "
                 "<部门全路径> [--db <库>]\n";
    return 2;
  }
  const std::string_view sub = argv[0];
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }

  if (sub == "add") {
    if (argc < 2) {
      std::cerr << "用法：bot add <name> --by <账号>（谁建的须留痕）\n";
      return 2;
    }
    const std::string name = argv[1];
    std::string by;
    for (int i = 2; i + 1 < argc; i += 2)
      if (std::string_view(argv[i]) == "--by") by = argv[i + 1];
    if (by.empty()) {
      std::cerr << "缺 --by <账号>\n";
      return 2;
    }
    if (!store.find_account(by)) {
      std::cerr << "操作者账号不存在：" << by << "\n";
      return 1;
    }
    if (name.find(':') != std::string::npos) {
      std::cerr << "bot 名不得含 ':'（前缀 bot: 为系统保留）\n";
      return 2;
    }
    const std::string token = std::string("bot_") + memex::server::random_salt_hex();
    const auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch())
                                .count();
    const auto id = store.bot_add(name, memex::server::sha256_hex(token), by,
                                  created_ms);
    if (id == 0) {
      std::cerr << "bot 创建失败（重名或参数非法）：" << name << "\n";
      return 1;
    }
    std::cout << "已创建 bot：" << name << "（全名 bot:" << name << "）\n"
              << "token：" << token << "\n"
              << "（token 仅此一次显示，请立即保存；删除：memex_server bot remove "
              << name << "）\n"
              << "收发示例（与 webhook 同端口 " << memex::server::kDefaultWebhookPort << "）：\n"
              << "  curl -s -H 'Authorization: Bearer " << token << "' \\\n"
              << "    -H 'Content-Type: application/json' \\\n"
              << "    -d '{\"target\":\"<账号|group:N>\",\"text\":\"hello\"}' \\\n"
              << "    http://<服务器>:" << memex::server::kDefaultWebhookPort << "/bot/send\n"
              << "  curl -s -H 'Authorization: Bearer " << token << "' \\\n"
              << "    http://<服务器>:" << memex::server::kDefaultWebhookPort << "/bot/updates\n";
    return 0;
  }

  if (sub == "list") {
    const auto rows = store.bot_list();
    std::cout << "name\t全名\t创建人\ttoken 摘要前缀\t创建时刻\t状态\t"
                 "白名单（空=不设限）\n";
    for (const auto& b : rows) {
      std::time_t secs = static_cast<std::time_t>(b.created_ms / 1000);
      std::tm tm{};
      local_time(secs, &tm);
      char when[24];
      std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
      std::cout << b.name << "\tbot:" << b.name << '\t' << b.created_by
                << '\t' << b.token_hash.substr(0, 12) << '\t' << when << '\t'
                << (b.disabled ? "已禁用" : "启用") << '\t';
      // 白名单摘要：群{1,2} 部门{公司/研发部,…}；两名单全空=不设限
      const auto groups = store.bot_groups_allowed(b.name);
      const auto depts = store.bot_depts_allowed(b.name);
      if (groups.empty() && depts.empty()) {
        std::cout << '-';
      } else {
        std::cout << "群{";
        for (std::size_t i = 0; i < groups.size(); ++i) {
          std::cout << (i ? "," : "") << groups[i];
        }
        std::cout << "} 部门{";
        for (std::size_t i = 0; i < depts.size(); ++i) {
          std::cout << (i ? "," : "") << depts[i];
        }
        std::cout << "}";
      }
      std::cout << '\n';
    }
    std::cout << "共 " << rows.size() << " 个\n";
    return 0;
  }

  // allow / deny <name> --group <群号> | --dept <部门全路径>：
  // 可用范围白名单维护（名单非空才收紧；群名单变动连带退名单外群）
  if (sub == "allow" || sub == "deny") {
    if (argc < 4) {
      std::cerr << "用法：bot " << sub
                << " <name> --group <群号> | --dept <部门全路径>\n";
      return 2;
    }
    const std::string flag = argv[2];
    const std::string name = argv[1];
    const std::string value = argv[3];
    if (flag != "--group" && flag != "--dept") {
      std::cerr << "用法：bot " << sub
                << " <name> --group <群号> | --dept <部门全路径>\n";
      return 2;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (flag == "--group") {
      const auto gid = std::strtoull(value.c_str(), nullptr, 10);
      const bool ok = sub == "allow"
                          ? store.bot_allow_group(name, gid, now)
                          : store.bot_deny_group(name, gid);
      if (!ok) {
        std::cerr << (sub == "allow" ? "加白失败（bot 或群不存在）"
                                     : "deny 失败（bot 不存在）")
                  << "：" << name << " → 群 " << value << "\n";
        return 1;
      }
      std::cout << "bot " << name
                << (sub == "allow" ? " 已加白群 " : " 已移出白名单群 ") << value
                << "（移出名单即退群；名单非空时名单外群连带退群）\n";
      return 0;
    }
    const bool ok = sub == "allow"
                        ? store.bot_allow_dept(name, value, now)
                        : store.bot_deny_dept(name, value);
    if (!ok) {
      std::cerr << (sub == "allow" ? "加白失败（bot 或部门路径不存在）"
                                   : "deny 失败（bot 不存在）")
                << "：" << name << " → " << value << "\n";
      return 1;
    }
    std::cout << "bot " << name
              << (sub == "allow" ? " 已加白部门 " : " 已移出部门白名单 ")
              << value << "\n";
    return 0;
  }

  if (sub == "remove" || sub == "disable" || sub == "enable" || sub == "join" ||
      sub == "leave") {
    if (argc < 2) {
      std::cerr << "用法：bot " << sub << " <name>"
                << (sub == "join" || sub == "leave" ? " <群号>" : "") << "\n";
      return 2;
    }
    const std::string name = argv[1];
    if (sub == "remove") {
      if (!store.bot_remove(name)) {
        std::cerr << "无此 bot：" << name << "\n";
        return 1;
      }
      std::cout << "已删除 bot：" << name << "（已连带退出全部群）\n";
      return 0;
    }
    if (sub == "disable" || sub == "enable") {
      if (!store.bot_set_disabled(name, sub == "disable")) {
        std::cerr << "无此 bot：" << name << "\n";
        return 1;
      }
      std::cout << "已" << (sub == "disable" ? "禁用" : "启用") << " bot："
                << name << "\n";
      return 0;
    }
    // join / leave <name> <群号>
    if (argc < 3) {
      std::cerr << "用法：bot " << sub << " <name> <群号>\n";
      return 2;
    }
    const auto gid = std::strtoull(argv[2], nullptr, 10);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const bool ok = sub == "join" ? store.bot_join_group(name, gid, now)
                                  : store.bot_leave_group(name, gid);
    if (!ok) {
      std::cerr << (sub == "join" ? "加群失败（bot／群不存在，或群不在白名单）"
                                  : "退群失败（bot 不存在或本不在群）")
                << "：" << name << " → 群 " << gid << "\n";
      return 1;
    }
    std::cout << "bot " << name << (sub == "join" ? " 已加入群 " : " 已退出群 ")
              << gid << "\n";
    return 0;
  }

  std::cerr << "未知 bot 子命令：" << sub << "\n"
            << "用法：bot add <name> --by <账号> | list | remove <name> | "
               "disable|enable <name> | join|leave <name> <群号> | "
               "allow|deny <name> --group <群号>|--dept <部门全路径>\n";
  return 2;
}


// 模型网关（平台三期）管理面：endpoint 登记（注册序=路由优先序，--local
// 标记归档红线专用本地端点）／calls 调用审计台账（一次上游尝试一行）。
// 调用面=POST /v1/chat/completions（OpenAI 兼容），Bearer 用 bot token。
int cmd_model(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server model endpoint add <name> --url "
                 "<http://host[:port][/prefix]> [--key <api_key>] "
                 "[--model <上游模型名>] [--local] --by <账号> | endpoint "
                 "list | endpoint remove <name> | endpoint enable|disable "
                 "<name> | endpoint local <name> on|off | calls [N] "
                 "[--db <库>]\n";
    return 2;
  }
  const std::string_view sub = argv[0];
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }

  if (sub == "endpoint") {
    if (argc < 2) {
      std::cerr << "用法：model endpoint add|list|remove|enable|disable|local…\n";
      return 2;
    }
    const std::string_view es = argv[1];
    if (es == "add") {
      if (argc < 3) {
        std::cerr << "用法：model endpoint add <name> --url <http://…> "
                     "[--key <api_key>] [--model <上游模型名>] [--local] "
                     "--by <账号>\n";
        return 2;
      }
      const std::string name = argv[2];
      memex::server::ModelEndpointRow ep;
      ep.name = name;
      std::string by;
      bool have_local = false;
      for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--url" && i + 1 < argc) ep.base_url = argv[++i];
        else if (arg == "--key" && i + 1 < argc) ep.api_key = argv[++i];
        else if (arg == "--model" && i + 1 < argc) ep.model = argv[++i];
        else if (arg == "--local") { ep.is_local = true; have_local = true; }
        else if (arg == "--by" && i + 1 < argc) by = argv[++i];
        else {
          std::cerr << "未知选项：" << argv[i] << "\n";
          return 2;
        }
      }
      if (ep.base_url.empty() || by.empty()) {
        std::cerr << "缺 --url 或 --by（谁登记的须留痕）\n";
        return 2;
      }
      if (name.find(':') != std::string::npos) {
        std::cerr << "端点名不得含 ':'\n";
        return 2;
      }
      if (ep.base_url.rfind("http://", 0) != 0) {
        std::cerr << "base_url 须以 http:// 开头（内网口径）\n";
        return 2;
      }
      if (!store.find_account(by)) {
        std::cerr << "操作者账号不存在：" << by << "\n";
        return 1;
      }
      ep.created_by = by;
      ep.created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
      const auto id = store.model_endpoint_add(ep);
      if (id == 0) {
        std::cerr << "端点登记失败（重名或参数非法）：" << name << "\n";
        return 1;
      }
      std::cout << "已登记端点：" << name << " → " << ep.base_url
                << (ep.is_local ? "（本地·可承接归档数据）"
                                : "（外部·归档数据红线不路由）")
                << "\n注册序即路由优先序（现第 " << id << " 位）；"
                << "调用：POST /v1/chat/completions（Bearer 用 bot token，"
                << "带 memex_archive_scope:true 只路由本地端点）\n";
      return 0;
    }
    if (es == "list") {
      const auto rows = store.model_endpoints_list();
      std::cout << "序\t名称\tbase_url\t模型\t本地\t状态\t登记人\n";
      int order = 0;
      for (const auto& e : rows) {
        std::cout << ++order << '\t' << e.name << '\t' << e.base_url << '\t'
                  << (e.model.empty() ? "-" : e.model) << '\t'
                  << (e.is_local ? "是" : "否") << '\t'
                  << (e.enabled ? "启用" : "停用") << '\t' << e.created_by
                  << '\n';
      }
      std::cout << "共 " << rows.size() << " 个（归档数据仅本地模型——"
                   "规则写死在网关，不做成配置）\n";
      return 0;
    }
    if (es == "remove" || es == "enable" || es == "disable") {
      if (argc < 3) {
        std::cerr << "用法：model endpoint " << es << " <name>\n";
        return 2;
      }
      const std::string name = argv[2];
      bool ok = false;
      if (es == "remove") ok = store.model_endpoint_remove(name);
      else if (es == "enable") ok = store.model_endpoint_set_enabled(name, true);
      else ok = store.model_endpoint_set_enabled(name, false);
      if (!ok) {
        std::cerr << "无此端点：" << name << "\n";
        return 1;
      }
      std::cout << "已" << (es == "remove" ? "删除" : es == "enable" ? "启用" : "停用")
                << "端点：" << name << "\n";
      return 0;
    }
    if (es == "local") {
      if (argc < 4 ||
          (std::string_view(argv[3]) != "on" &&
           std::string_view(argv[3]) != "off")) {
        std::cerr << "用法：model endpoint local <name> on|off\n";
        return 2;
      }
      if (!store.model_endpoint_set_local(argv[2], std::string_view(argv[3]) == "on")) {
        std::cerr << "无此端点：" << argv[2] << "\n";
        return 1;
      }
      std::cout << "端点 " << argv[2] << " 本地标记 → "
                << (std::string_view(argv[3]) == "on" ? "是（可承接归档数据）"
                                                      : "否") << "\n";
      return 0;
    }
    std::cerr << "未知 model endpoint 子命令：" << es << "\n";
    return 2;
  }

  if (sub == "calls") {
    int limit = 50;
    for (int i = 1; i + 1 < argc; i += 2)
      if (std::string_view(argv[i]) == "--limit")
        limit = std::atoi(argv[i + 1]);
    if (argc >= 2 && std::string_view(argv[1]).find_first_not_of("0123456789") ==
                         std::string::npos) {
      limit = std::atoi(argv[1]);
    }
    std::cout << "id\t调用方\t端点\t模型\t归档\t提示符数\t补全数\t状态\t时延ms\t时刻\n";
    for (const auto& c : store.model_calls_list(limit)) {
      std::time_t secs = static_cast<std::time_t>(c.created_ms / 1000);
      std::tm tm{};
      local_time(secs, &tm);
      char when[24];
      std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
      std::cout << c.id << '\t' << c.caller << '\t'
                << (c.endpoint.empty() ? "-" : c.endpoint) << '\t'
                << (c.model.empty() ? "-" : c.model) << '\t'
                << (c.archive_scope ? "是" : "否") << '\t' << c.prompt_chars
                << '\t' << c.completion_chars << '\t' << c.status << '\t'
                << c.latency_ms << '\t' << when << '\n';
    }
    return 0;
  }

  std::cerr << "未知 model 子命令：" << sub << "\n"
            << "用法：model endpoint add|list|remove|enable|disable|local … | "
               "calls [N]\n";
  return 2;
}


// webhook 接入台账（T4.10）：create 建 token（明文仅此一次，库内存 sha256
// 摘要）／list 一览／revoke 吊销。目标＝账号（个人）或 group:<群号>（按群独立）。
int cmd_webhook(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server webhook create --target <账号|group:N> "
                 "[--name 备注] | list | revoke <id> [--db <库>]\n";
    return 2;
  }
  const std::string_view sub = argv[0];
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }

  if (sub == "create") {
    std::string target;
    std::string name;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "--target" && i + 1 < argc) {
        target = argv[++i];
      } else if (arg == "--name" && i + 1 < argc) {
        name = argv[++i];
      } else {
        std::cerr << "未知选项：" << argv[i] << "\n";
        return 2;
      }
    }
    if (target.empty()) {
      std::cerr << "缺 --target <账号|group:N>\n";
      return 2;
    }
    // 建即校验目标（防错绑到不存在的账号／群）
    if (target.rfind("group:", 0) == 0) {
      const auto gid = static_cast<std::uint64_t>(
          std::strtoull(target.c_str() + 6, nullptr, 10));
      if (!store.group_info(gid)) {
        std::cerr << "目标群不存在：" << target << "\n";
        return 1;
      }
    } else if (!store.find_account(target)) {
      std::cerr << "目标账号不存在：" << target << "\n";
      return 1;
    }
    const std::string token = std::string("whk_") + memex::server::random_salt_hex();
    const auto created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch())
                                .count();
    const auto id =
        store.webhook_create(memex::server::sha256_hex(token), target, name,
                             created_ms);
    if (id == 0) {
      std::cerr << "webhook 台账写入失败\n";
      return 1;
    }
    std::cout << "已创建 webhook（id " << id << "，目标 " << target << "）\n"
              << "token：" << token << "\n"
              << "（token 仅此一次显示，请立即保存；吊销："
                 "memex_server webhook revoke "
              << id << "）\n"
              << "curl 示例：\n"
                 "  curl -X POST http://<服务器>:"
              << memex::server::kDefaultWebhookPort << "/hook/" << token
              << " -H 'Content-Type: application/json' \\\n"
                 "    -d '{\"title\":\"通知标题\",\"content\":\"通知内容\","
                 "\"urgency\":\"important\"}'\n";
    return 0;
  }

  if (sub == "list") {
    const auto rows = store.webhook_list();
    std::cout << "id\t目标\t备注\ttoken 摘要前缀\t创建时间\t状态\n";
    for (const auto& w : rows) {
      std::time_t secs = static_cast<std::time_t>(w.created_ms / 1000);
      std::tm tm{};
      local_time(secs, &tm);
      char when[24];
      std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
      std::cout << w.id << '\t' << w.target << '\t'
                << (w.name.empty() ? "-" : w.name) << '\t'
                << w.token_hash.substr(0, 12) << '\t' << when << '\t'
                << (w.revoked ? "已吊销" : "有效") << '\n';
    }
    std::cout << "共 " << rows.size() << " 条\n";
    return 0;
  }

  if (sub == "revoke") {
    if (argc < 2) {
      std::cerr << "用法：webhook revoke <id>\n";
      return 2;
    }
    const auto id = std::atoll(argv[1]);
    if (id <= 0) {
      std::cerr << "无效 id：" << argv[1] << "\n";
      return 2;
    }
    if (!store.webhook_revoke(id)) {
      std::cerr << "无此 id 或已吊销：" << id << "\n";
      return 1;
    }
    std::cout << "已吊销 webhook id " << id << "\n";
    return 0;
  }

  std::cerr << "未知 webhook 子命令：" << sub << "\n"
            << "用法：webhook create --target <账号|group:N> [--name 备注] | "
               "list | revoke <id>\n";
  return 2;
}
