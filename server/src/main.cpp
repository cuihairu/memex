// MemexServer 入口：serve（默认）与账号／登录记录管理子命令。
// 管理后台（T3.1）接管运维面之前，账号开通与记录查询走本 CLI。
#include <asio.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "files_server.hpp"
#include "server.hpp"
#include "storage.hpp"
#include "store.hpp"
#include "webhook.hpp"

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace {

// 服务端默认监听端口。评审报告只固定了直连态端口段（UDP 2425–2436／TCP 2426–2437），
// 服务端监听端口未钉死，此默认值待与网络管理侧确认后写入部署文档。
constexpr std::uint16_t kDefaultPort = 24360;
// webhook 接入独立端口（T4.10）：与消息端口分离，HTTP 面不与长连接混线；
// --webhook-port 0 可整体关闭接入（其余功能不受影响）。
constexpr std::uint16_t kDefaultWebhookPort = 24361;
constexpr const char* kDefaultDb = "memex-server.db";

// R23-5 逗号分隔小写扩展名表（黑/白名单共用；空白项剔除）
std::vector<std::string> split_ext_list(const char* csv) {
  std::vector<std::string> out;
  std::istringstream in(csv);
  std::string item;
  while (std::getline(in, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

// db_path 由 main 统一解析（--db 剥离会就地改写 argv，不能再从 argv 复读）
int cmd_serve(int argc, char** argv, const std::string& db_path) {
  std::uint16_t port = kDefaultPort;
  int webhook_port = kDefaultWebhookPort; // int 才能表达 0＝关闭
  int files_port = 0; // 文件面默认关闭：须显式 --files-port 且给 S3 配置
  // 外网单向 uplink 面（R23-4）默认关闭：须显式 --uplink-port 开启
  //（安全默认：开启即明示暴露范围，见装配处日志）
  int uplink_port = 0;
  // R23-5 外网面防护参数：缺省只让危险扩展黑名单生效，其余显式开启
  memex::server::UplinkPolicy uplink_policy;
  memex::server::S3Config s3;
  s3.region = "auto";
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
      if (port == 0) {
        std::cerr << "无效端口\n";
        return 2;
      }
    } else if (arg == "--webhook-port" && i + 1 < argc) {
      webhook_port = std::atoi(argv[++i]);
      if (webhook_port < 0 || webhook_port > 65535) {
        std::cerr << "无效 webhook 端口（0＝关闭接入）\n";
        return 2;
      }
    } else if (arg == "--files-port" && i + 1 < argc) {
      files_port = std::atoi(argv[++i]);
      if (files_port < 0 || files_port > 65535) {
        std::cerr << "无效文件面端口（0＝关闭文件面）\n";
        return 2;
      }
    } else if (arg == "--uplink-port" && i + 1 < argc) {
      uplink_port = std::atoi(argv[++i]);
      if (uplink_port < 0 || uplink_port > 65535) {
        std::cerr << "无效 uplink 端口（0＝关闭外网入口）\n";
        return 2;
      }
    } else if (arg == "--uplink-ext-denylist" && i + 1 < argc) {
      // R23-5 黑名单整表替换（逗号分隔小写扩展名；空串=清空=不限）
      uplink_policy.ext_denylist.clear();
      for (const auto& e : split_ext_list(argv[++i])) {
        uplink_policy.ext_denylist.push_back(e);
      }
    } else if (arg == "--uplink-ext-allowlist" && i + 1 < argc) {
      // R23-5 白名单模式（非空即启用：名单外一律拒，黑名单失效）
      uplink_policy.ext_allowlist.clear();
      for (const auto& e : split_ext_list(argv[++i])) {
        uplink_policy.ext_allowlist.push_back(e);
      }
    } else if (arg == "--uplink-max-mb" && i + 1 < argc) {
      uplink_policy.max_upload_bytes =
          static_cast<std::int64_t>(std::atoll(argv[++i])) * 1024 * 1024;
      if (uplink_policy.max_upload_bytes < 0) {
        std::cerr << "无效 --uplink-max-mb（须 ≥0，0=沿用全局上限）\n";
        return 2;
      }
    } else if (arg == "--uplink-login-secret" && i + 1 < argc) {
      // R23-5 外网登录二次验证（部署级第二口令；TOTP 另批）
      uplink_policy.login_secret = argv[++i];
    } else if (arg == "--s3-endpoint" && i + 1 < argc) {
      s3.endpoint = argv[++i];
    } else if (arg == "--s3-bucket" && i + 1 < argc) {
      s3.bucket = argv[++i];
    } else if (arg == "--s3-access-key" && i + 1 < argc) {
      s3.access_key = argv[++i];
    } else if (arg == "--s3-secret-key" && i + 1 < argc) {
      s3.secret_key = argv[++i];
    } else if (arg == "--s3-region" && i + 1 < argc) {
      s3.region = argv[++i];
    } else if (arg == "--s3-use-ssl") {
      s3.use_ssl = true;
    }
  }
  // S3 配置环境变量兜底（旗标优先）：容器部署密钥走 env 免进 ps/日志的
  // 命令行；变量名与 e2e 排障用的 MEMEX_S3_ENDPOINT 同族——旗标在场时
  // 该 env 即使被排障脚本改写也不影响服务端（旗标恒赢）
  const auto env_or = [](const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
  };
  if (s3.endpoint.empty()) s3.endpoint = env_or("MEMEX_S3_ENDPOINT");
  if (s3.bucket.empty()) s3.bucket = env_or("MEMEX_S3_BUCKET");
  if (s3.access_key.empty()) s3.access_key = env_or("MEMEX_S3_ACCESS_KEY");
  if (s3.secret_key.empty()) s3.secret_key = env_or("MEMEX_S3_SECRET_KEY");
  // 文件面装配（R23-2）：端口给了但 S3 没配齐 → 明示降级（面整体 503）；
  // S3 配齐但端口没给 → 文件面不开。内网 http + path-style 为默认。
  std::shared_ptr<memex::server::S3Storage> s3_storage;
  const bool s3_ready =
      !s3.endpoint.empty() && !s3.bucket.empty() &&
      !s3.access_key.empty() && !s3.secret_key.empty();
  if (files_port > 0 && s3_ready) {
    s3.path_style = true;
    s3_storage = memex::server::S3Storage::create(s3);
    // 桶引导（幂等，含 head 先查）：容器/compose 部署不该手建桶。对端
    // healthy 与 S3 路由就绪有短窗口差（e2e 同款重试口径）；引导失败明示
    // 且面照起——上传一律 503，健康探针语义（进程活着）不受牵连
    bool bucket_ok = false;
    for (int i = 1; i <= 10 && !(bucket_ok = s3_storage->create_bucket());
         ++i) {
      std::cerr << "[MEMEX] 桶引导未就绪（第 " << i
                << "/10 次，2s 后重试）\n";
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!bucket_ok) {
      std::cerr << "[MEMEX] 桶引导失败：" << s3.bucket
                << "（对象存储不可达/凭据错？）上传一律 503\n";
    }
  } else if (files_port > 0) {
    std::cerr << "[MEMEX] 文件面已给端口但 S3 配置不齐"
                 "（--s3-endpoint/--s3-bucket/--s3-access-key/--s3-secret-key"
                 " 或环境变量 MEMEX_S3_ENDPOINT/MEMEX_S3_BUCKET/"
                 "MEMEX_S3_ACCESS_KEY/MEMEX_S3_SECRET_KEY），起面后一律 503\n";
  }
  // uplink 面（R23-4）依附同一存储实例；S3 未配齐也给起（面内一律 503），
  // 但默认关闭口径不变：不给 --uplink-port 就不创建监听。
  if (uplink_port > 0) {
    std::cout << "[MEMEX] 外网入口已显式开启（--uplink-port " << uplink_port
              << "）：暴露范围=外网单向上传（/uplink/session|upload|mine|"
                 "delete；无任何下载/读取内网数据端点，上传全审计）。"
                 "生产建议该口随隧道单独出网、内网面不出网\n";
    // R23-5 防护状态明示（黑名单缺省生效；其余防护显式开启才显示）
    if (!uplink_policy.ext_allowlist.empty()) {
      std::cout << "[MEMEX] uplink 类型白名单已启用（"
                << uplink_policy.ext_allowlist.size()
                << " 项；名单外一律拒收）\n";
    } else if (!uplink_policy.ext_denylist.empty()) {
      std::cout << "[MEMEX] uplink 类型黑名单生效（"
                << uplink_policy.ext_denylist.size()
                << " 项危险扩展名；--uplink-ext-allowlist 可切白名单模式）\n";
    }
    if (uplink_policy.max_upload_bytes > 0) {
      std::cout << "[MEMEX] uplink 单文件上限 "
                << uplink_policy.max_upload_bytes / 1024 / 1024 << "MiB\n";
    }
    if (!uplink_policy.login_secret.empty()) {
      std::cout << "[MEMEX] uplink 登录二次验证已启用（请求须带 secondary）\n";
    }
    // 扫描钩子（scan.hpp）缺省直通＝引擎未接入；接 clamd 等引擎时在此
    // 装配 uplink_policy.scanner 并补一行启用明示
  }

  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }

  try {
    asio::io_context io;
    memex::server::CollabServer server(io, store, port);
    // 文件面（R23-2）：独立端口，绑定失败只降级为「文件面未启用」，
    // 消息主通道不受影响；存储实例空＝面内一律 503。
    std::unique_ptr<memex::server::FileServer> files;
    // 会话库两面共享一份（R23-4）：uplink 面启用时才显式建共享句柄，
    // scope 闸认得出跨面令牌；纯内网部署缺省自建，行为不变。
    std::shared_ptr<memex::server::FileSessions> file_sessions;
    if (files_port > 0) {
      try {
        if (uplink_port > 0) {
          file_sessions = memex::server::make_file_sessions();
        }
        files = std::make_unique<memex::server::FileServer>(
            io, store, s3_storage, static_cast<std::uint16_t>(files_port),
            /*uplink_mode=*/false, file_sessions);
        // 工具结果卡片回群（R25-2）：走 deliver_notice 同一投递面（NOTICE
        // 信封＝离线入队＋归档＋在线扇出）；单 io_context 线程模型不加锁。
        files->set_notice([&server](const std::string& target,
                                    const std::string& title,
                                    const std::string& content, int urgency) {
          (void)memex::server::deliver_notice(server, target, title, content,
                                              urgency, "");
        });
      } catch (const std::exception& e) {
        std::cerr << "[MEMEX] 文件面端口绑定失败，文件面未启用"
                     "（消息主通道不受影响）：" << e.what() << std::endl;
      }
    }
    // 外网单向 uplink 面（R23-4）：独立监听口（物理面分离——隧道出网只
    // 出这个口）；绑定失败只降级为「外网入口未启用」并明示。
    std::unique_ptr<memex::server::FileServer> uplink;
    if (uplink_port > 0) {
      try {
        uplink = std::make_unique<memex::server::FileServer>(
            io, store, s3_storage, static_cast<std::uint16_t>(uplink_port),
            /*uplink_mode=*/true, file_sessions, uplink_policy);
      } catch (const std::exception& e) {
        std::cerr << "[MEMEX] uplink 端口绑定失败，外网入口未启用"
                     "（内网面不受影响）：" << e.what() << std::endl;
      }
    }
    // webhook 接入（T4.10）：独立端口；端口占用等绑定失败只降级为
    // 「接入未启用」并明示，消息主通道不受影响。
    std::unique_ptr<memex::server::WebhookServer> webhook;
    if (webhook_port > 0) {
      try {
        webhook = std::make_unique<memex::server::WebhookServer>(
            io, server, static_cast<std::uint16_t>(webhook_port));
      } catch (const std::exception& e) {
        std::cerr << "[MEMEX] webhook 端口绑定失败，接入未启用（消息主通道不受影响）："
                  << e.what() << std::endl;
      }
    }
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](std::error_code, int sig) {
      std::cout << "[MEMEX] 收到信号 " << sig << "，退出" << std::endl;
      io.stop();
    });
    server.start_accept();
    if (webhook) webhook->start_accept();
    if (files) files->start_accept();
    if (uplink) uplink->start_accept();
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

// 可移植 strptime（T3.2 时间窗解析用）：MSVC 无此 POSIX 函数（win 腿
// nightly 首编即 C3861）。std::get_time 按格式前缀匹配、不校验尾部残留——
// 与 strptime 口径一致，最长格式在前逐个尝试（"12:34:56" 先中 HH:MM:SS）。
bool parse_time_prefix(const std::string& text, const char* fmt, std::tm* tm) {
  std::istringstream ss(text);
  ss >> std::get_time(tm, fmt);
  return !ss.fail();
}

// localtime_r 的可移植替身：Windows 走 localtime_s（参数序与 POSIX 相反）
void local_time(std::time_t secs, std::tm* out) {
#ifdef _WIN32
  localtime_s(out, &secs);
#else
  localtime_r(&secs, out);
#endif
}

// 时间参数解析（T3.2）："YYYY-MM-DD" 或 "YYYY-MM-DD HH:MM[:SS]"（本地时区）。
// date_only 端补零点／当日末秒，使日期粒度的开闭区间语义正确。失败返回 -1。
std::int64_t parse_time_arg(const std::string& text, bool end_of_day) {
  std::tm tm{};
  const char* fmts[] = {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"};
  bool ok = false;
  for (const char* f : fmts) {
    if (parse_time_prefix(text, f, &tm)) {
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
  audit.op_account = cli_operator();
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
// 通讯录可见性（T4.6）：
//   org hide/unhide <账号>         成员隐藏／恢复（管理员、本人与白名单仍可见）
//   org dept hide/unhide <路径>    部门整树隐藏（部门内自己人仍互见）
//   org dept restrict/unrestrict <路径>  部门限看本部门／解除
//   org fields <账号> --hide title,manager[,role] | --clear  敏感字段脱敏
//   org allow/disallow <查看者> --see <账号|部门路径>         白名单例外
//   org visibility list            已配置行与白名单一览
int cmd_org(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server org dept add <路径> | dept list | "
                 "member <账号> | set … | import <CSV> | hide/unhide <账号> | "
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
              << kDefaultWebhookPort << "/hook/" << token
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
  std::cerr << "用法：group list [--gid N] | set-role <gid> <账号> "
               "<member|admin>\n";
  return 2;
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
    if (cmd == "cross") return cmd_cross(sub_argc, sub_argv, db_path);
    if (cmd == "favs") return cmd_favs(sub_argc, sub_argv, db_path);
    if (cmd == "org") return cmd_org(sub_argc, sub_argv, db_path);
    if (cmd == "group") return cmd_group(sub_argc, sub_argv, db_path);
    if (cmd == "policy") return cmd_policy(sub_argc, sub_argv, db_path);
    if (cmd == "webhook") return cmd_webhook(sub_argc, sub_argv, db_path);
    if (cmd == "storage") return cmd_storage(sub_argc, sub_argv, db_path);
    std::cerr << "未知子命令：" << cmd << "\n"
              << "用法：memex_server [serve [--port N] [--webhook-port N] "
                 "[--db P]] | account add … | "
                 "logins [账号] [--device 指纹前缀] | device … | "
                 "messages [账号] [--keyword K] [--since T] "
                 "[--until T] [--limit N] [--export 文件] | audit [N] | "
                 "cross [N] | favs <账号> | org … | group list|set-role | "
                 "webhook create|list|revoke | "
                 "storage compose|up|down|health | --version | --self-test\n";
    return 2;
  }
  return cmd_serve(0, argv, kDefaultDb);
}
