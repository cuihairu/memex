// memex_server CLI 拆分模块（LAY-006）：本域子命令体自原文件逐字搬移；
// 共享小工具与 cmd_* 声明见 common.hpp。main 按名分发，语义不变。
#include "server_cli/common.hpp"

// db_path 由 main 统一解析（--db 剥离会就地改写 argv，不能再从 argv 复读）
int cmd_serve(int argc, char** argv, const std::string& db_path) {
  // 旗标＋env 兜底解析在 parse_serve_args（单测直打）；此下只做装配
  memex::server::ServeArgs args;
  if (const int rc = memex::server::parse_serve_args(argc, argv, args);
      rc != 0) {
    return rc;
  }
  auto& port = args.port;
  auto& webhook_port = args.webhook_port;
  auto& model_port = args.model_port;
  auto& assistant_bot = args.assistant_bot;
  auto& assistant_token = args.assistant_token;
  auto& files_port = args.files_port;
  auto& uplink_port = args.uplink_port;
  auto& uplink_policy = args.uplink_policy;
  auto& s3 = args.s3;
  auto& ldap_cfg = args.ldap_cfg;
  auto& tool_cred_secret = args.tool_cred_secret;
  auto& task_pull_secret = args.task_pull_secret;
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
  // 需求批⑩ 在线时长自愈：上次进程退出若非全部干净登出（断电/崩溃），
  // 悬空的 online 段统一截断到本次启动点——关机期不计为在线。
  store.trim_dangling_online(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  // 544 LDAP 企业身份装配：host 给了且配置齐才起目录面；绑定 issuer=ldap
  // 的账号登录走目录判登（identity bind 账号 ldap 目录用户名 显式绑定
  // ——不按账号名自动猜）。fail-closed：目录不可达/未配一律拒，不回落
  // 本地口令；未绑定账号本地判登不变（本地账号常备＝应急恢复入口）。
  std::unique_ptr<memex::server::LdapDirectory> ldap;
  if (!ldap_cfg.host.empty()) {
    if (const std::string tls_err = ldap_cfg.tls_posture_error();
        !tls_err.empty()) {
      std::cerr << "[MEMEX] 目录认证未启用：" << tls_err << "\n";
      return 2;
    }
    if (ldap_cfg.ready()) {
      ldap = std::make_unique<memex::server::LdapDirectory>(ldap_cfg);
      std::cout << "[MEMEX] 企业目录认证已启用（" << ldap_cfg.placeholder()
                << "）\n";
      if (ldap_cfg.insecure_tls)
        std::cerr << "[MEMEX] 警告：--ldap-tls-insecure 已开——目录证书不校验"
                     "（自签场景专用；生产建议入受信链）\n";
      if (ldap_cfg.allow_plain)
        std::cerr << "[MEMEX] 警告：--ldap-allow-plain 已开——bind 口令明网"
                     "明文（仅限测试/受控内网）\n";
      if (!ldap_cfg.auto_bind || !ldap_cfg.auto_provision) {
        std::cerr << "[MEMEX] 首登策略：自动绑定="
                  << (ldap_cfg.auto_bind ? "开" : "关")
                  << "／影子账号=" << (ldap_cfg.auto_provision ? "开" : "关")
                  << "（默认全关＝只认 identity bind 显式绑定）\n";
      }
      bool maps_admin = false;
      for (const auto& [g, role] : ldap_cfg.group_roles)
        if (role == "admin") maps_admin = true;
      if (maps_admin)
        std::cerr << "[MEMEX] 警告：组→角色映射含 admin（显式配置允许，"
                     "属运维决定——连接器本身永不隐式授管理员）\n";
      if (ldap_cfg.kick_disabled && ldap_cfg.recheck_seconds > 0 &&
          (ldap_cfg.bind_dn.empty() || ldap_cfg.disabled_attr.empty()))
        std::cerr << "[MEMEX] 目录禁用踢下线：存量会话复检仅在服务账号模式"
                     "＋禁用检测配置齐时生效（模板模式无凭据自查，只拒新登录）\n";
    } else {
      std::cerr << "[MEMEX] --ldap-host 已给但目录配置不齐（须 --ldap-user-base"
                   "＋绑定路径：--ldap-bind-dn-template 或 --ldap-bind-dn＋口令"
                   "）——目录认证未启用，登录全走本地\n";
    }
  }

  try {
    asio::io_context io;
    memex::server::CollabServer server(io, store, port, ldap.get());
    server.start_ldap_sweep(); // 544 目录禁用×存量会话（未配目录=空转）
    // 文件面（R23-2）：独立端口，绑定失败只降级为「文件面未启用」，
    // 消息主通道不受影响；存储实例空＝面内一律 503。
    std::unique_ptr<memex::server::FileServer> files;
    // R27-5 任务源拉取器（配置面在文件面 /files/task-sources，故随文件
    // 面装配；io.run() 之后析构＝在途拉取回调不会越过实例）
    std::unique_ptr<memex::server::TaskPuller> puller;
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
            /*uplink_mode=*/false, file_sessions,
            memex::server::UplinkPolicy{}, ldap.get());
        // 工具结果卡片回群（R25-2）：走 deliver_notice 同一投递面（NOTICE
        // 信封＝离线入队＋归档＋在线扇出）；单 io_context 线程模型不加锁。
        // 任务源拉取器复用同一 lambda（R27-5：新落/有变化的任务同一投递
        // 面回推，客户端 30s 拉取降为兜底）。
        const auto notice_fn = [&server](const std::string& target,
                                         const std::string& title,
                                         const std::string& content,
                                         int urgency) {
          (void)memex::server::deliver_notice(server, target, title, content,
                                              urgency, "");
        };
        files->set_notice(notice_fn);
        // 群工具结果卡片回群（R26）：普通 TEXT 群消息（群会话气泡点开
        // 可见明细；离线入队＋归档＋在线扇出与手发消息同路）；from=操作者。
        files->set_group_text([&server](const std::string& target,
                                        const std::string& from,
                                        const std::string& text) {
          (void)memex::server::deliver_group_text(server, target, from, text);
        });
        // R25-4 凭据面：主密钥派生 GCM 密钥（空=未启用，凭据路由 503）
        files->set_tool_cred_secret(tool_cred_secret);
        // R27-5 任务源拉取器：默认关硬口径三道闸（主密钥/admin 开关/单源
        // 开关逐道短路＝零外呼）；主密钥未给＝不武装不建传输线程。
        puller = std::make_unique<memex::server::TaskPuller>(io, store);
        puller->set_key(memex::server::derive_task_pull_key(task_pull_secret));
        puller->set_notice(notice_fn);
        puller->start();
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
            /*uplink_mode=*/true, file_sessions, uplink_policy, ldap.get());
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
