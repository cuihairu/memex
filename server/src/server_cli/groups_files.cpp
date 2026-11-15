// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"


// R23-2 群角色运维面：群一览／管理员任免（权限模型的群主动作；协议/UI
// 未及之前先走 CLI）。角色改动即时生效：文件面等各判权消费方按库现值判。
int cmd_group(int argc, char** argv, const std::string& db_path) {
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "打不开库：" << db_path << "\n";
    return 1;
  }
  if (argc >= 1 && std::string_view(argv[0]) == "list") {
    std::uint64_t only = 0;
    for (int i = 1; i + 1 < argc + 1 && i < argc; ++i) {
      if (std::string_view(argv[i]) == "--gid" && i + 1 < argc) {
        only = std::strtoull(argv[++i], nullptr, 10);
      }
    }
    std::cout << "群id\t名称\t群主\t成员数\t管理员\n";
    for (const auto& g : store.groups_list()) {
      if (only != 0 && g.group_id != only) continue;
      std::string admins;
      for (const auto& m : g.members) {
        if (store.group_role(g.group_id, m) == "admin") {
          admins += (admins.empty() ? "" : ",") + m;
        }
      }
      std::cout << g.group_id << '\t' << g.name << '\t' << g.owner << '\t'
                << g.members.size() << '\t'
                << (admins.empty() ? "-" : admins) << '\n';
    }
    return 0;
  }
  if (argc >= 4 && std::string_view(argv[0]) == "set-role") {
    const std::uint64_t gid = std::strtoull(argv[1], nullptr, 10);
    const std::string account = argv[2];
    const std::string role = argv[3];
    const auto info = store.group_info(gid);
    if (!info) {
      std::cerr << "无此群：" << gid << "\n";
      return 1;
    }
    if (account == info->owner) {
      std::cerr << "群主角色不可改（owner 身份在 groups.owner）\n";
      return 2;
    }
    if (!store.group_set_role(gid, account, role)) {
      std::cerr << "任免失败（须 member|admin，且账号须已是群成员）\n";
      return 1;
    }
    std::cout << "已设群 " << gid << " 成员 " << account << " 角色="
              << role << "\n";
    return 0;
  }
  // 平台-12 群能力开关（权限模型「群能力管理员配全」）：每个能力可独立
  // 停/启；未配置=现行（允许）。配置变更落查阅台账（谁/何时/改了什么）。
  if (argc >= 1 && std::string_view(argv[0]) == "capability") {
    const std::int64_t now = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::system_clock::now()
                                       .time_since_epoch())
        .count();
    if (argc >= 5 && std::string_view(argv[1]) == "set") {
      const std::uint64_t gid = std::strtoull(argv[2], nullptr, 10);
      const std::string name = argv[3];
      const std::string_view onoff = argv[4];
      if (onoff != "on" && onoff != "off") {
        std::cerr << "用法：group capability set <gid> <名> on|off --by 账号"
                     "（名须在词汇表内）\n";
        return 2;
      }
      std::string by;
      for (int i = 5; i + 1 < argc; i += 2)
        if (std::string_view(argv[i]) == "--by") by = argv[i + 1];
      if (by.empty()) {
        std::cerr << "缺 --by 账号（谁配置的须留痕）\n";
        return 2;
      }
      if (!store.group_capability_set(gid, name, onoff == "on", by, now)) {
        std::cerr << "写入失败（群须存在；能力名须在词汇表：";
        for (const auto& n : memex::server::ServerStore::group_capability_names())
          std::cerr << n << ' ';
        std::cerr << "）\n";
        return 1;
      }
      std::cout << "已配置群 " << gid << " 能力 " << name << " → "
                << (onoff == "on" ? "启用" : "停用") << "（by " << by
                << "）\n";
      return 0;
    }
    if (argc >= 3 && std::string_view(argv[1]) == "list") {
      const std::uint64_t gid = std::strtoull(argv[2], nullptr, 10);
      std::cout << "能力\t生效\t配置人\t配置时刻\n";
      for (const auto& c : store.group_capabilities_list(gid)) {
        std::cout << c.capability << '\t' << (c.enabled ? "启用" : "停用")
                  << '\t' << c.updated_by << '\t' << c.updated_ms << '\n';
      }
      std::cout << "（未列出的能力＝未配置＝现行允许）\n";
      return 0;
    }
    std::cerr << "用法：group capability set <gid> <名> on|off --by 账号 | "
                 "capability list <gid>\n";
    return 2;
  }
  std::cerr << "用法：group list [--gid N] | set-role <gid> <账号> "
               "<member|admin> | capability set|list …\n";
  return 2;
}


// 评审 P0-3（todo 452）：对象存储字节面一致性核对——R23 文件元数据 ↔
// 对象字节双面核对。备份恢复预案（docs/src/guide/backup-restore.md）的
// 恢复后核对工具；只读不改（孤儿清理属运维决策，工具不代删）。
int cmd_files(int argc, char** argv, const std::string& db_path) {
  if (argc < 1 || std::string_view(argv[0]) != "verify") {
    std::cerr << "用法：files verify [--s3-endpoint E --s3-bucket B "
                 "--s3-access-key A --s3-secret-key K] [--s3-region R]"
                 " [--s3-use-ssl]\n"
              << "  核对 R23 文件元数据 ↔ 对象字节：缺对象/大小不符/"
                 "桶清单失败→退出码 1；孤儿对象只告警\n"
              << "  S3 旗标缺省回落环境变量 MEMEX_S3_ENDPOINT/MEMEX_S3_BUCKET"
                 "/MEMEX_S3_ACCESS_KEY/MEMEX_S3_SECRET_KEY（与 serve 同族）\n";
    return 2;
  }
  memex::server::S3Config s3;
  s3.path_style = true;
  for (int i = 1; i < argc; ++i) {
    const std::string_view opt = argv[i];
    if (opt == "--s3-endpoint" && i + 1 < argc) s3.endpoint = argv[++i];
    else if (opt == "--s3-bucket" && i + 1 < argc) s3.bucket = argv[++i];
    else if (opt == "--s3-access-key" && i + 1 < argc) s3.access_key = argv[++i];
    else if (opt == "--s3-secret-key" && i + 1 < argc) s3.secret_key = argv[++i];
    else if (opt == "--s3-region" && i + 1 < argc) s3.region = argv[++i];
    else if (opt == "--s3-use-ssl") s3.use_ssl = true;
    else {
      std::cerr << "未知选项：" << opt << "\n";
      return 2;
    }
  }
  // 环境变量兜底（旗标优先）——与 serve 同族变量名，容器部署密钥走 env
  const auto env_or = [](const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
  };
  if (s3.endpoint.empty()) s3.endpoint = env_or("MEMEX_S3_ENDPOINT");
  if (s3.bucket.empty()) s3.bucket = env_or("MEMEX_S3_BUCKET");
  if (s3.access_key.empty()) s3.access_key = env_or("MEMEX_S3_ACCESS_KEY");
  if (s3.secret_key.empty()) s3.secret_key = env_or("MEMEX_S3_SECRET_KEY");
  if (s3.region.empty()) s3.region = "auto";
  if (s3.endpoint.empty() || s3.bucket.empty() || s3.access_key.empty() ||
      s3.secret_key.empty()) {
    std::cerr << "S3 配置不齐（--s3-endpoint/--s3-bucket/--s3-access-key/"
                 "--s3-secret-key 或对应 MEMEX_S3_* 环境变量）\n";
    return 2;
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "开库失败：" << db_path << "\n";
    return 1;
  }
  auto storage = memex::server::S3Storage::create(s3);
  if (!storage) {
    std::cerr << "对象存储连接失败：" << s3.endpoint << "\n";
    return 1;
  }
  const auto rep = memex::server::verify_file_objects(store, *storage);
  std::cout << "受检元数据行（normal）：" << rep.checked
            << "　桶内对象：" << rep.objects << "\n";
  for (const auto& k : rep.missing) std::cout << "缺对象：" << k << "\n";
  for (const auto& m : rep.mismatches) {
    std::cout << "大小不符：" << m.key << " 元数据=" << m.expected
              << " 实际=" << m.actual << "\n";
  }
  for (const auto& k : rep.orphans) {
    std::cout << "孤儿对象（无元数据引用，仅告警）：" << k << "\n";
  }
  if (!rep.objects_listed) {
    // 桶内清单中途失败：孤儿核对残缺——「一致」结论不成立（假绿堵死）
    std::cerr << "桶内对象清单不完整（list 中途失败）：孤儿核对未完成，"
                 "本次核对不判通过\n";
    return 1;
  }
  if (!rep.missing.empty() || !rep.mismatches.empty()) {
    std::cout << "不一致：缺 " << rep.missing.size() << " 条、大小不符 "
              << rep.mismatches.size() << " 条\n";
    return 1;
  }
  std::cout << "一致：元数据与对象字节核对通过（孤儿 " << rep.orphans.size()
            << " 条）\n";
  return 0;
}


// R23-1 RustFS compose 集成面：生成 compose／起停容器／健康检查。
// 客户端永不直连对象存储——本子命令只管部署编排，不做任何数据面操作。
int cmd_storage(int argc, char** argv, const std::string& /*db_path*/) {
  if (argc < 1) {
    std::cerr << "用法：storage compose [--out 路径] [--data-dir D] "
                 "[--api-port N] [--console-port N] [--access-key AK] "
                 "[--secret-key SK]\n"
              << "      storage up <compose.yml> | down <compose.yml> | "
                 "health <endpoint> [--timeout 秒]\n";
    return 2;
  }
  const std::string_view sub = argv[0];
  if (sub == "compose") {
    std::string out = "rustfs-compose.yml", data_dir = "/var/lib/memex/rustfs";
    std::string ak = "minioadmin", sk = "minioadmin";
    int api_port = 9000, console_port = 9001;
    for (int i = 1; i + 1 < argc; ++i) {
      const std::string_view opt = argv[i];
      if (opt == "--out") out = argv[++i];
      else if (opt == "--data-dir") data_dir = argv[++i];
      else if (opt == "--api-port") api_port = std::atoi(argv[++i]);
      else if (opt == "--console-port") console_port = std::atoi(argv[++i]);
      else if (opt == "--access-key") ak = argv[++i];
      else if (opt == "--secret-key") sk = argv[++i];
      else {
        std::cerr << "未知选项：" << opt << "\n";
        return 2;
      }
    }
    if (!memex::server::RustFSCompose::write_compose_file(
            out, data_dir, ak, sk, api_port, console_port)) {
      std::cerr << "写 compose 失败：" << out << "\n";
      return 1;
    }
    std::cout << "已生成 " << out << "（data=" << data_dir
              << " api=" << api_port << " console=" << console_port << "）\n"
              << "注意：rustfs 容器以 uid 10001 运行，起容器前须\n"
              << "  chown 10001:10001 " << data_dir << "\n"
              << "下一步：memex_server storage up " << out << "\n";
    if (memex::server::is_default_credential(ak, sk)) {
      std::cout << "改口令：先 storage down，再用 --access-key/--secret-key\n"
                << "  重新生成（勿对生产用默认凭据；server 端 env 须同名对齐）\n";
    }
    return 0;
  }
  if (sub == "up" || sub == "down") {
    if (argc < 2) {
      std::cerr << "用法：storage " << sub << " <compose.yml>\n";
      return 2;
    }
    const bool ok = sub == "up"
                        ? memex::server::RustFSCompose::up(argv[1])
                        : memex::server::RustFSCompose::down(argv[1]);
    if (!ok) {
      std::cerr << "docker compose 失败（宿主须有 docker compose v2）\n";
      return 1;
    }
    std::cout << (sub == "up" ? "已启动" : "已停止") << "（compose："
              << argv[1] << "）\n";
    if (sub == "up") {
      std::cout << "健康检查：memex_server storage health "
                   "http://127.0.0.1:<api-port>\n";
    }
    return 0;
  }
  if (sub == "health") {
    if (argc < 2) {
      std::cerr << "用法：storage health <endpoint> [--timeout 秒]\n";
      return 2;
    }
    int timeout = 30;
    for (int i = 2; i + 1 < argc; ++i) {
      if (std::string_view(argv[i]) == "--timeout") {
        timeout = std::atoi(argv[++i]);
      } else {
        std::cerr << "未知选项：" << argv[i] << "\n";
        return 2;
      }
    }
    if (!memex::server::RustFSCompose::health_check(argv[1], timeout)) {
      std::cerr << "健康检查未通过：" << argv[1] << "\n";
      return 1;
    }
    std::cout << "健康：" << argv[1] << "\n";
    return 0;
  }
  std::cerr << "未知 storage 子命令：" << sub << "\n";
  return 2;
}


// archive status|seal|verify：归档完整性账（453 远期加固）运维面。
//   status  块数／开尾事件数／链头摘要
//   seal    开尾收尾封账（定期运维动作；满 64 条落库时同事务自动封）
//   verify  全账校验（断点／位置如实打印；留痕 audit_reads；
//           完好 exit 0、已断 exit 1——巡检脚本可挂）
int cmd_archive(int argc, char** argv, const std::string& db_path) {
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "打不开库：" << db_path << "\n";
    return 1;
  }
  const std::string action =
      argc >= 1 ? std::string(argv[0]) : std::string("status");
  if (action == "status") {
    const auto blocks = store.archive_chain_list();
    const auto v = store.archive_chain_verify();
    std::cout << "封账块数：" << v.blocks << "\n";
    std::cout << "开尾（未封事件）：" << v.open_tail << " 条\n";
    if (!blocks.empty())
      std::cout << "链头：" << blocks.back().chain_hash << "\n";
    else
      std::cout << "链头：（空账）\n";
    return 0;
  }
  if (action == "seal") {
    if (!store.archive_chain_seal()) {
      std::cerr << "封账失败（无新事件或写入失败）\n";
      return 1;
    }
    const auto blocks = store.archive_chain_list();
    std::cout << "已封账：共 " << blocks.size() << " 块";
    if (!blocks.empty())
      std::cout << "，链头 " << blocks.back().chain_hash;
    std::cout << "\n";
    return 0;
  }
  if (action == "verify") {
    const auto v = store.archive_chain_verify();
    memex::server::AuditReadRow audit;
    audit.op_account = cli_operator();
    audit.action = "audit.archive.verify";
    audit.filters = "blocks=" + std::to_string(v.blocks) +
                    ",open_tail=" + std::to_string(v.open_tail) +
                    ",events=" + std::to_string(v.events);
    audit.result_count = static_cast<int>(v.blocks);
    audit.ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    store.add_audit_read(audit);
    if (v.ok) {
      std::cout << "归档完整性校验：完好（封账 " << v.blocks << " 块，覆盖 "
                << v.events << " 条事件，开尾 " << v.open_tail
                << " 条未封）\n";
      return 0;
    }
    std::cerr << "归档完整性校验：已断——" << v.broken_at << "\n";
    return 1;
  }
  std::cerr << "用法：memex_server archive status|seal|verify [--db 库]\n";
  return 2;
}
