// serve 面旗标解析实现（自 main.cpp cmd_serve 抽出）：严格解析口径——
// 数字旗标一律严格十进制（std::atoi 静默回绕会让配置拼写错误静默改变
// 端口与安全开关）；未知旗标/位置参数/缺值/布尔带值一律 rc2 不静默；
// 全旗标支持 --flag=value 内联形态（codex-761）。
#include "serve_args.hpp"

#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace memex::server {

namespace {

// R23-5 逗号分隔小写扩展名表（黑/白名单共用；空白项剔除）
std::vector<std::string> split_ext_list(std::string_view csv) {
  std::vector<std::string> out;
  std::istringstream in{std::string(csv)};
  std::string item;
  while (std::getline(in, item, ',')) {
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

// 严格十进制 int 解析（codex-761）：仅可选负号＋全数字，空串/杂字符/
// 溢出一律 nullopt——atoi 的静默回绕（"abc"→0、"99999999999"→截断）
// 会把拼写错误的值悄悄变成另一个合法配置
std::optional<int> parse_int(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (s.size() == 1) return std::nullopt;
  }
  long long v = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') return std::nullopt;
    v = v * 10 + (c - '0');
    if (v > 2147483648LL) return std::nullopt; // 超 int 量级即拒（不回绕）
  }
  return static_cast<int>(neg ? -v : v);
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
  // --flag=value 内联形态状态（每轮循环刷新；布尔旗标带值在分支内拒）
  bool has_inline = false;
  std::string_view inline_val;
  int i = 0;
  // 取值旗标统一取值器：内联形态优先；否则吃下一参（循环头已 ++i，
  // i 此刻指向旗标后第一参）；缺值 rc2（旧口径「尾旗标缺值静默跳过」
  // ＝安全开关/端口悄悄走默认，不许可）
  const auto take = [&](std::string_view name, std::string_view* v) {
    if (has_inline) {
      *v = inline_val;
      return true;
    }
    if (i < argc) {
      *v = argv[i++];
      return true;
    }
    std::cerr << "缺少 " << name << " 的值\n";
    return false;
  };
  std::string_view v;
  while (i < argc) {
    const char* raw = argv[i]; // 全量原文（含 =value；报错文案用）
    ++i; // 先推进：取值器再吃下一参（分支内 argv[i] 均指旗标原文 raw）
    std::string_view arg = raw;
    has_inline = false;
    inline_val = {};
    if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
      inline_val = arg.substr(eq + 1);
      arg = arg.substr(0, eq);
      has_inline = true;
    }
    // 位置参数（不以 -- 开头）不静默：sub_argv 口径下不该有裸词
    if (arg.rfind("--", 0) != 0) {
      std::cerr << "意外位置参数: " << raw << "\n";
      return 2;
    }
    if (arg == "--port") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n <= 0 || *n > 65535) {
        std::cerr << "无效端口\n";
        return 2;
      }
      port = static_cast<std::uint16_t>(*n);
    } else if (arg == "--webhook-port") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n < 0 || *n > 65535) {
        std::cerr << "无效 webhook 端口（0＝关闭接入）\n";
        return 2;
      }
      webhook_port = *n;
    } else if (arg == "--model-port") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n < 0 || *n > 65535) {
        std::cerr << "无效模型网关端口（0＝关闭网关）\n";
        return 2;
      }
      model_port = *n;
    } else if (arg == "--assistant") {
      // 群内智能助手（平台三期）：以 bot 身份轮询四指令；回群走回环
      // /bot/send（须 webhook 口开启）
      if (!take(arg, &v)) return 2;
      assistant_bot = std::string(v);
    } else if (arg == "--assistant-token") {
      if (!take(arg, &v)) return 2;
      assistant_token = std::string(v);
    } else if (arg == "--files-port") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n < 0 || *n > 65535) {
        std::cerr << "无效文件面端口（0＝关闭文件面）\n";
        return 2;
      }
      files_port = *n;
    } else if (arg == "--uplink-port") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n < 0 || *n > 65535) {
        std::cerr << "无效 uplink 端口（0＝关闭外网入口）\n";
        return 2;
      }
      uplink_port = *n;
    } else if (arg == "--uplink-ext-denylist") {
      // R23-5 黑名单整表替换（逗号分隔小写扩展名；空串=清空=不限）
      if (!take(arg, &v)) return 2;
      uplink_policy.ext_denylist = split_ext_list(v);
    } else if (arg == "--uplink-ext-allowlist") {
      // R23-5 白名单模式（非空即启用：名单外一律拒，黑名单失效）
      if (!take(arg, &v)) return 2;
      uplink_policy.ext_allowlist = split_ext_list(v);
    } else if (arg == "--uplink-max-mb") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n < 0) {
        std::cerr << "无效 --uplink-max-mb（须 ≥0，0=沿用全局上限）\n";
        return 2;
      }
      uplink_policy.max_upload_bytes =
          static_cast<std::int64_t>(*n) * 1024 * 1024;
    } else if (arg == "--uplink-login-secret") {
      // R23-5 外网登录二次验证（部署级第二口令；TOTP 另批）
      if (!take(arg, &v)) return 2;
      uplink_policy.login_secret = std::string(v);
    } else if (arg == "--tool-cred-secret") {
      // R25-4 凭据面主密钥（服务端派生 GCM 密钥；不给=凭据面未启用）
      if (!take(arg, &v)) return 2;
      tool_cred_secret = std::string(v);
    } else if (arg == "--task-pull-secret") {
      // R27-5 任务源拉取面主密钥（PBKDF2 同水位派生；不给=拉取面未启用
      // =零外呼，配置路由 503）
      if (!take(arg, &v)) return 2;
      task_pull_secret = std::string(v);
    } else if (arg == "--s3-endpoint") {
      if (!take(arg, &v)) return 2;
      s3.endpoint = std::string(v);
    } else if (arg == "--s3-bucket") {
      if (!take(arg, &v)) return 2;
      s3.bucket = std::string(v);
    } else if (arg == "--s3-access-key") {
      if (!take(arg, &v)) return 2;
      s3.access_key = std::string(v);
    } else if (arg == "--s3-secret-key") {
      if (!take(arg, &v)) return 2;
      s3.secret_key = std::string(v);
    } else if (arg == "--s3-region") {
      if (!take(arg, &v)) return 2;
      s3.region = std::string(v);
    } else if (arg == "--s3-use-ssl") {
      // 布尔旗标不带值（--s3-use-ssl=true 一律 rc2——布尔语义无值可带）
      if (has_inline) {
        std::cerr << "布尔旗标不带值: " << raw << "\n";
        return 2;
      }
      s3.use_ssl = true;
    } else if (arg == "--ldap-host") {
      if (!take(arg, &v)) return 2;
      ldap_cfg.host = std::string(v);
    } else if (arg == "--ldap-port") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n <= 0 || *n > 65535) {
        std::cerr << "无效 --ldap-port\n";
        return 2;
      }
      ldap_cfg.port = static_cast<std::uint16_t>(*n);
    } else if (arg == "--ldap-ssl") {
      // ldaps:// 整连接 TLS（证书主机名校验必开）；StartTLS 不支持
      if (has_inline) {
        std::cerr << "布尔旗标不带值: " << raw << "\n";
        return 2;
      }
      ldap_cfg.use_ssl = true;
    } else if (arg == "--ldap-timeout-ms") {
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n < 0) {
        std::cerr << "无效 --ldap-timeout-ms\n";
        return 2;
      }
      ldap_cfg.timeout_ms = *n;
    } else if (arg == "--ldap-user-base") {
      if (!take(arg, &v)) return 2;
      ldap_cfg.user_base = std::string(v);
    } else if (arg == "--ldap-user-attr") {
      if (!take(arg, &v)) return 2;
      ldap_cfg.user_attr = std::string(v);
    } else if (arg == "--ldap-bind-dn-template") {
      // 模板直绑（OpenLDAP 常用）：{u} 替换用户名
      if (!take(arg, &v)) return 2;
      ldap_cfg.bind_dn_template = std::string(v);
    } else if (arg == "--ldap-bind-dn") {
      // 服务账号先查后绑（AD 常用）；口令走 --ldap-bind-password 或
      // env MEMEX_LDAP_BIND_PASSWORD（env 优先——旗标进 ps）
      if (!take(arg, &v)) return 2;
      ldap_cfg.bind_dn = std::string(v);
    } else if (arg == "--ldap-bind-password") {
      if (!take(arg, &v)) return 2;
      ldap_cfg.bind_password = std::string(v);
    } else if (arg == "--ldap-memberof-attr") {
      if (!take(arg, &v)) return 2;
      ldap_cfg.memberof_attr = std::string(v);
    } else if (arg == "--ldap-disabled-attr") {
      // 禁用检测三元组（可选）：AD 停用位＝attr userAccountControl、
      // rule 1.2.840.113556.1.4.803、value 2
      if (!take(arg, &v)) return 2;
      ldap_cfg.disabled_attr = std::string(v);
    } else if (arg == "--ldap-disabled-rule") {
      if (!take(arg, &v)) return 2;
      ldap_cfg.disabled_rule = std::string(v);
    } else if (arg == "--ldap-disabled-value") {
      if (!take(arg, &v)) return 2;
      ldap_cfg.disabled_value = std::string(v);
    } else if (arg == "--ldap-group-role") {
      // 组→角色显式映射（可重复）：--ldap-group-role 组DN=角色名。
      // 映射表外组一概忽略；表里不配 admin 就永不授管理员。
      if (!take(arg, &v)) return 2;
      const std::string kv(v);
      const auto eq = kv.find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 == kv.size()) {
        std::cerr << "无效 --ldap-group-role（须 组=角色）\n";
        return 2;
      }
      ldap_cfg.group_roles.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (arg == "--ldap-starttls") {
      // 明连 389 后升级 TLS（AD/OpenLDAP 常态）；与 --ldap-ssl 互斥
      if (has_inline) {
        std::cerr << "布尔旗标不带值: " << raw << "\n";
        return 2;
      }
      ldap_cfg.starttls = true;
    } else if (arg == "--ldap-allow-plain") {
      // 明文 bind 显式放行（默认拒——密码不明文红线；仅限测试/受控内网）
      if (has_inline) {
        std::cerr << "布尔旗标不带值: " << raw << "\n";
        return 2;
      }
      ldap_cfg.allow_plain = true;
    } else if (arg == "--ldap-tls-insecure") {
      // 自签证书：跳过证书校验（启动明示警告）
      if (has_inline) {
        std::cerr << "布尔旗标不带值: " << raw << "\n";
        return 2;
      }
      ldap_cfg.insecure_tls = true;
    } else if (arg == "--ldap-auto-bind") {
      // 已有本地账号无绑定：目录认证成功即自动绑定（默认关）
      if (has_inline) {
        std::cerr << "布尔旗标不带值: " << raw << "\n";
        return 2;
      }
      ldap_cfg.auto_bind = true;
    } else if (arg == "--ldap-auto-provision") {
      // 账号不存在：目录认证成功即建影子账号（默认关；本地口令随机
      // 不可知＝LDAP-only）
      if (has_inline) {
        std::cerr << "布尔旗标不带值: " << raw << "\n";
        return 2;
      }
      ldap_cfg.auto_provision = true;
    } else if (arg == "--ldap-kick-disabled") {
      // 目录禁用 × 存量会话策略（默认 1=踢下线；0=仅拒新登录）
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n) {
        std::cerr << "无效 --ldap-kick-disabled（0/1）\n";
        return 2;
      }
      ldap_cfg.kick_disabled = *n != 0;
    } else if (arg == "--ldap-recheck-seconds") {
      // 存量会话复检间隔（默认 60s；0=不复检；仅服务模式可查）
      if (!take(arg, &v)) return 2;
      const auto n = parse_int(v);
      if (!n || *n < 0) {
        std::cerr << "无效 --ldap-recheck-seconds\n";
        return 2;
      }
      ldap_cfg.recheck_seconds = *n;
    } else {
      // 未知旗标不静默：拼写错误（--prot）悄悄漏配＝端口/安全开关走默认
      std::cerr << "未知旗标: " << raw << "\n";
      return 2;
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
