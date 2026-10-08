// MemexServer 入口：serve（默认）与账号／登录记录管理子命令。
// 管理后台（T3.1）接管运维面之前，账号开通与记录查询走本 CLI。
#include <asio.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
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
#include "group_assistant.hpp"
#include "model_gateway.hpp"
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
// 模型网关（平台三期）默认关闭：须显式 --model-port 开启（上游端点未
// 登记时网关空转无意义；安全默认=不暴露）。
constexpr std::uint16_t kDefaultModelPort = 24362;
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
  int model_port = 0; // 模型网关默认关闭：--model-port N 显式开启
  std::string assistant_bot;   // 群内智能助手（平台三期）：bot 名（空=关）
  std::string assistant_token; // 该 bot 明文 token（建议 env
                               // MEMEX_ASSISTANT_TOKEN——旗标值进 ps）
  int files_port = 0; // 文件面默认关闭：须显式 --files-port 且给 S3 配置
  // 外网单向 uplink 面（R23-4）默认关闭：须显式 --uplink-port 开启
  //（安全默认：开启即明示暴露范围，见装配处日志）
  int uplink_port = 0;
  // R23-5 外网面防护参数：缺省只让危险扩展黑名单生效，其余显式开启
  memex::server::UplinkPolicy uplink_policy;
  memex::server::S3Config s3;
  s3.region = "auto";
  // R25-4 凭据面主密钥（缺省空＝凭据面未启用；建议随部署 env 走
  // ——旗标进 ps，值仅服务端内存内派生）
  std::string tool_cred_secret;
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
    } else if (arg == "--model-port" && i + 1 < argc) {
      model_port = std::atoi(argv[++i]);
      if (model_port < 0 || model_port > 65535) {
        std::cerr << "无效模型网关端口（0＝关闭网关）\n";
        return 2;
      }
    } else if (arg == "--assistant" && i + 1 < argc) {
      // 群内智能助手（平台三期）：以 bot 身份轮询四指令；回群走回环
      // /bot/send（须 webhook 口开启）
      assistant_bot = argv[++i];
    } else if (arg == "--assistant-token" && i + 1 < argc) {
      assistant_token = argv[++i];
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
    } else if (arg == "--tool-cred-secret" && i + 1 < argc) {
      // R25-4 凭据面主密钥（服务端派生 GCM 密钥；不给=凭据面未启用）
      tool_cred_secret = argv[++i];
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
        // 群工具结果卡片回群（R26）：普通 TEXT 群消息（群会话气泡点开
        // 可见明细；离线入队＋归档＋在线扇出与手发消息同路）；from=操作者。
        files->set_group_text([&server](const std::string& target,
                                        const std::string& from,
                                        const std::string& text) {
          (void)memex::server::deliver_group_text(server, target, from, text);
        });
        // R25-4 凭据面：主密钥派生 GCM 密钥（空=未启用，凭据路由 503）
        files->set_tool_cred_secret(tool_cred_secret);
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
    // 模型网关（平台三期）：独立端口独立线程——上游 LLM 调用为秒级阻塞，
    // 不与消息面抢共享 io_context；自有库连接开同库（busy_timeout 兜写锁）。
    // 绑定失败只降级为「网关未启用」并明示。
    std::unique_ptr<memex::server::ModelGatewayServer> gateway;
    std::thread gateway_thread;
    if (model_port > 0) {
      try {
        gateway = std::make_unique<memex::server::ModelGatewayServer>(
            static_cast<std::uint16_t>(model_port), db_path);
        gateway->start_accept();
        gateway_thread = std::thread([&gateway] { gateway->run(); });
      } catch (const std::exception& e) {
        gateway.reset();
        std::cerr << "[MEMEX] 模型网关端口绑定失败，网关未启用（消息主通道"
                     "不受影响）：" << e.what() << std::endl;
      }
    }
    // 群内智能助手（平台三期）：独立线程轮询 bot 收信队列，四指令
    // （@助手/@纪要/@整理/@检索）；回群走回环 /bot/send（webhook 口须
    // 开启），模型调用走同进程网关回环（未启用则指令回「模型未启用」，
    // @检索 不依赖模型照常可用）。
    std::unique_ptr<memex::server::GroupAssistantWorker> assistant;
    if (!assistant_bot.empty()) {
      if (assistant_token.empty()) {
        const char* env = std::getenv("MEMEX_ASSISTANT_TOKEN");
        if (env) assistant_token = env;
      }
      if (webhook_port <= 0) {
        std::cerr << "[MEMEX] 群助手需要回环投递口：--webhook-port 未开启，"
                     "助手未启用" << std::endl;
      } else if (assistant_token.empty()) {
        std::cerr << "[MEMEX] 群助手缺 token：--assistant-token 或 env "
                     "MEMEX_ASSISTANT_TOKEN，助手未启用" << std::endl;
      } else if (!store.bot_by_name(assistant_bot)) {
        std::cerr << "[MEMEX] 群助手 bot 不存在：" << assistant_bot
                  << "（先 memex_server bot add 并 join 群），助手未启用"
                  << std::endl;
      } else {
        try {
          assistant =
              std::make_unique<memex::server::GroupAssistantWorker>(
                  assistant_bot, assistant_token,
                  static_cast<std::uint16_t>(webhook_port),
                  model_port > 0
                      ? "http://127.0.0.1:" + std::to_string(model_port) +
                            "/v1" // 网关 OpenAI 兼容路由在 /v1 下
                      : std::string{},
                  db_path, /*poll_ms=*/1000);
          assistant->start();
          std::cout << "[MEMEX] 群内智能助手已启动（bot:" << assistant_bot
                    << "，指令：@助手/@纪要/@整理/@检索）" << std::endl;
        } catch (const std::exception& e) {
          assistant.reset();
          std::cerr << "[MEMEX] 群助手启动失败（消息主通道不受影响）："
                    << e.what() << std::endl;
        }
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
    if (assistant) assistant->stop(); // 先停助手（回环依赖 webhook/网关口）
    if (gateway) gateway->stop();
    if (gateway_thread.joinable()) gateway_thread.join();
  } catch (const std::exception& e) {
    std::cerr << "服务端异常退出：" << e.what() << std::endl;
    return 1;
  }
  return 0;
}

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

// 机器人（bot）台账：add 建 token（明文仅此一次，库内存 sha256 摘要）／
// list 一览／remove 删除（连带退全部群）／disable|enable 开关／join|leave
// 群成员维护。bot 全名="bot:<name>" 伪账号：加群后收群消息（TEXT 扇出自动
// 入队）、可群发；单聊 to=bot:<name> 直达。收发走 webhook 端口 /bot/*。
int cmd_bot(int argc, char** argv, const std::string& db_path) {
  if (argc < 1) {
    std::cerr << "用法：memex_server bot add <name> --by <账号> | list | "
                 "remove <name> | disable|enable <name> | join|leave "
                 "<name> <群号> [--db <库>]\n";
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
              << "收发示例（与 webhook 同端口 " << kDefaultWebhookPort << "）：\n"
              << "  curl -s -H 'Authorization: Bearer " << token << "' \\\n"
              << "    -H 'Content-Type: application/json' \\\n"
              << "    -d '{\"target\":\"<账号|group:N>\",\"text\":\"hello\"}' \\\n"
              << "    http://<服务器>:" << kDefaultWebhookPort << "/bot/send\n"
              << "  curl -s -H 'Authorization: Bearer " << token << "' \\\n"
              << "    http://<服务器>:" << kDefaultWebhookPort << "/bot/updates\n";
    return 0;
  }

  if (sub == "list") {
    const auto rows = store.bot_list();
    std::cout << "name\t全名\t创建人\ttoken 摘要前缀\t创建时刻\t状态\n";
    for (const auto& b : rows) {
      std::time_t secs = static_cast<std::time_t>(b.created_ms / 1000);
      std::tm tm{};
      local_time(secs, &tm);
      char when[24];
      std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
      std::cout << b.name << "\tbot:" << b.name << '\t' << b.created_by
                << '\t' << b.token_hash.substr(0, 12) << '\t' << when << '\t'
                << (b.disabled ? "已禁用" : "启用") << '\n';
    }
    std::cout << "共 " << rows.size() << " 个\n";
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
      std::cerr << (sub == "join" ? "加群失败（bot 或群不存在）"
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
               "disable|enable <name> | join|leave <name> <群号>\n";
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
    if (cmd == "identity") return cmd_identity(sub_argc, sub_argv, db_path);
    if (cmd == "logins") return cmd_logins(sub_argc, sub_argv, db_path);
    if (cmd == "device") return cmd_device(sub_argc, sub_argv, db_path);
    if (cmd == "messages") return cmd_messages(sub_argc, sub_argv, db_path);
    if (cmd == "retention") return cmd_retention(sub_argc, sub_argv, db_path);
    if (cmd == "audit") return cmd_audit(sub_argc, sub_argv, db_path);
    if (cmd == "cross") return cmd_cross(sub_argc, sub_argv, db_path);
    if (cmd == "favs") return cmd_favs(sub_argc, sub_argv, db_path);
    if (cmd == "org") return cmd_org(sub_argc, sub_argv, db_path);
    if (cmd == "group") return cmd_group(sub_argc, sub_argv, db_path);
    if (cmd == "policy") return cmd_policy(sub_argc, sub_argv, db_path);
    if (cmd == "assist") return cmd_assist(sub_argc, sub_argv, db_path);
    if (cmd == "bot") return cmd_bot(sub_argc, sub_argv, db_path);
    if (cmd == "model") return cmd_model(sub_argc, sub_argv, db_path);
    if (cmd == "webhook") return cmd_webhook(sub_argc, sub_argv, db_path);
    if (cmd == "storage") return cmd_storage(sub_argc, sub_argv, db_path);
    std::cerr << "未知子命令：" << cmd << "\n"
              << "用法：memex_server [serve [--port N] [--webhook-port N] "
                 "[--model-port N] [--assistant <bot名> "
                 "[--assistant-token T]] "
                 "[--db P]] | account add … | "
                 "logins [账号] [--device 指纹前缀] | device … | "
                 "messages [账号] [--keyword K] [--since T] "
                 "[--until T] [--limit N] [--export 文件] | audit [N] | "
                 "cross [N] | favs <账号> | org … | group list|set-role | "
                 "webhook create|list|revoke | "
                 "bot add|list|remove|disable|enable|join|leave | "
                 "model endpoint add|list|remove|enable|disable|local | calls | "
                 "assist policy|request|approve|deny|start|end|show|list|audit"
                 " | "
                 "storage compose|up|down|health | --version | --self-test\n";
    return 2;
  }
  return cmd_serve(0, argv, kDefaultDb);
}
