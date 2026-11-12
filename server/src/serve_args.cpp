// serve 面旗标解析实现（自 main.cpp cmd_serve 机械搬移；代码逐字保留，
// 仅把 cmd_serve 局部变量改为 ServeArgs& 的引用别名）。
#include "serve_args.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace memex::server {

namespace {

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

} // namespace

int parse_serve_args(int argc, char** argv, ServeArgs& out) {
  out.s3.region = "auto";
  auto& port = out.port;
  auto& webhook_port = out.webhook_port;
  auto& model_port = out.model_port;
  auto& assistant_bot = out.assistant_bot;
  auto& assistant_token = out.assistant_token;
  auto& files_port = out.files_port;
  auto& uplink_port = out.uplink_port;
  auto& uplink_policy = out.uplink_policy;
  auto& s3 = out.s3;
  auto& ldap_cfg = out.ldap_cfg;
  auto& tool_cred_secret = out.tool_cred_secret;
  auto& task_pull_secret = out.task_pull_secret;
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
    } else if (arg == "--task-pull-secret" && i + 1 < argc) {
      // R27-5 任务源拉取面主密钥（PBKDF2 同水位派生；不给=拉取面未启用
      // =零外呼，配置路由 503）
      task_pull_secret = argv[++i];
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
    } else if (arg == "--ldap-host" && i + 1 < argc) {
      ldap_cfg.host = argv[++i];
    } else if (arg == "--ldap-port" && i + 1 < argc) {
      const int p = std::atoi(argv[++i]);
      if (p <= 0 || p > 65535) {
        std::cerr << "无效 --ldap-port\n";
        return 2;
      }
      ldap_cfg.port = static_cast<std::uint16_t>(p);
    } else if (arg == "--ldap-ssl") {
      // ldaps:// 整连接 TLS（证书主机名校验必开）；StartTLS 不支持
      ldap_cfg.use_ssl = true;
    } else if (arg == "--ldap-timeout-ms" && i + 1 < argc) {
      ldap_cfg.timeout_ms = std::atoi(argv[++i]);
    } else if (arg == "--ldap-user-base" && i + 1 < argc) {
      ldap_cfg.user_base = argv[++i];
    } else if (arg == "--ldap-user-attr" && i + 1 < argc) {
      ldap_cfg.user_attr = argv[++i];
    } else if (arg == "--ldap-bind-dn-template" && i + 1 < argc) {
      // 模板直绑（OpenLDAP 常用）：{u} 替换用户名
      ldap_cfg.bind_dn_template = argv[++i];
    } else if (arg == "--ldap-bind-dn" && i + 1 < argc) {
      // 服务账号先查后绑（AD 常用）；口令走 --ldap-bind-password 或
      // env MEMEX_LDAP_BIND_PASSWORD（env 优先——旗标进 ps）
      ldap_cfg.bind_dn = argv[++i];
    } else if (arg == "--ldap-bind-password" && i + 1 < argc) {
      ldap_cfg.bind_password = argv[++i];
    } else if (arg == "--ldap-memberof-attr" && i + 1 < argc) {
      ldap_cfg.memberof_attr = argv[++i];
    } else if (arg == "--ldap-disabled-attr" && i + 1 < argc) {
      // 禁用检测三元组（可选）：AD 停用位＝attr userAccountControl、
      // rule 1.2.840.113556.1.4.803、value 2
      ldap_cfg.disabled_attr = argv[++i];
    } else if (arg == "--ldap-disabled-rule" && i + 1 < argc) {
      ldap_cfg.disabled_rule = argv[++i];
    } else if (arg == "--ldap-disabled-value" && i + 1 < argc) {
      ldap_cfg.disabled_value = argv[++i];
    } else if (arg == "--ldap-group-role" && i + 1 < argc) {
      // 组→角色显式映射（可重复）：--ldap-group-role 组DN=角色名。
      // 映射表外组一概忽略；表里不配 admin 就永不授管理员。
      const std::string kv = argv[++i];
      const auto eq = kv.find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 == kv.size()) {
        std::cerr << "无效 --ldap-group-role（须 组=角色）\n";
        return 2;
      }
      ldap_cfg.group_roles.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (arg == "--ldap-starttls") {
      // 明连 389 后升级 TLS（AD/OpenLDAP 常态）；与 --ldap-ssl 互斥
      ldap_cfg.starttls = true;
    } else if (arg == "--ldap-allow-plain") {
      // 明文 bind 显式放行（默认拒——密码不明文红线；仅限测试/受控内网）
      ldap_cfg.allow_plain = true;
    } else if (arg == "--ldap-tls-insecure") {
      // 自签证书：跳过证书校验（启动明示警告）
      ldap_cfg.insecure_tls = true;
    } else if (arg == "--ldap-auto-bind") {
      // 已有本地账号无绑定：目录认证成功即自动绑定（默认关）
      ldap_cfg.auto_bind = true;
    } else if (arg == "--ldap-auto-provision") {
      // 账号不存在：目录认证成功即建影子账号（默认关；本地口令随机
      // 不可知＝LDAP-only）
      ldap_cfg.auto_provision = true;
    } else if (arg == "--ldap-kick-disabled" && i + 1 < argc) {
      // 目录禁用 × 存量会话策略（默认 1=踢下线；0=仅拒新登录）
      ldap_cfg.kick_disabled = std::atoi(argv[++i]) != 0;
    } else if (arg == "--ldap-recheck-seconds" && i + 1 < argc) {
      // 存量会话复检间隔（默认 60s；0=不复检；仅服务模式可查）
      ldap_cfg.recheck_seconds = std::atoi(argv[++i]);
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
  // LDAP env 兜底（旗标优先）：host/port/user-base/bind-dn-template/
  // bind-dn/bind-password/memberof-attr/group-role（组=角色，逗号分隔）；
  // 服务账号口令建议只走 env（不进 ps）
  if (ldap_cfg.host.empty()) ldap_cfg.host = env_or("MEMEX_LDAP_HOST");
  if (ldap_cfg.user_base.empty())
    ldap_cfg.user_base = env_or("MEMEX_LDAP_USER_BASE");
  if (ldap_cfg.bind_dn_template.empty())
    ldap_cfg.bind_dn_template = env_or("MEMEX_LDAP_BIND_DN_TEMPLATE");
  if (ldap_cfg.bind_dn.empty()) ldap_cfg.bind_dn = env_or("MEMEX_LDAP_BIND_DN");
  if (ldap_cfg.bind_password.empty())
    ldap_cfg.bind_password = env_or("MEMEX_LDAP_BIND_PASSWORD");
  if (ldap_cfg.memberof_attr.empty())
    ldap_cfg.memberof_attr = env_or("MEMEX_LDAP_MEMBEROF_ATTR");
  if (const char* gr = std::getenv("MEMEX_LDAP_GROUP_ROLE");
      gr != nullptr && *gr != '\0') {
    for (std::size_t i = 0;;) {
      const auto comma = std::string_view(gr).find(',', i);
      const std::string kv(comma == std::string_view::npos
                               ? std::string_view(gr).substr(i)
                               : std::string_view(gr).substr(i, comma - i));
      const auto eq = kv.find('=');
      if (eq != std::string::npos && eq > 0 && eq + 1 < kv.size())
        ldap_cfg.group_roles.emplace_back(kv.substr(0, eq),
                                          kv.substr(eq + 1));
      if (comma == std::string_view::npos) break;
      i = comma + 1;
    }
  }
  return 0;
}

} // namespace memex::server
