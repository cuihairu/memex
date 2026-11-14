// serve 面旗标解析验收（parse_serve_args 直打，不起进程）：默认值、端口
// 界（0/负/越界/溢出/杂字符→rc2）、uplink 防护参数（黑/白名单整表替换、
// 空表清空、max-mb 换算与负值拒、二次口令）、S3/LDAP 旗标＋env 兜底
//（旗标恒赢）、LDAP 组→角色映射（旗标与 env 逗号分隔多组、坏形态 rc2）、
// 严格解析腿（未知旗标/位置参数/缺值/布尔带值 rc2；--flag=value 内联）。
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "serve_args.hpp"

using memex::server::kDefaultPort;
using memex::server::kDefaultWebhookPort;
using memex::server::parse_serve_args;
using memex::server::ServeArgs;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond  \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

// 直跑：argv[0]＝首个旗标（与 main 子命令剥离后的 sub_argv 口径一致）
int run(const std::vector<const char*>& args, ServeArgs& out) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (const char* a : args) argv.push_back(const_cast<char*>(a));
  return parse_serve_args(static_cast<int>(argv.size()), argv.data(), out);
}

// env 敏感腿前清场（旗标恒赢与兜底两腿互不串味）
void clear_env() {
  for (const char* n :
       {"MEMEX_S3_ENDPOINT", "MEMEX_S3_BUCKET", "MEMEX_S3_ACCESS_KEY",
        "MEMEX_S3_SECRET_KEY", "MEMEX_LDAP_HOST", "MEMEX_LDAP_USER_BASE",
        "MEMEX_LDAP_BIND_DN_TEMPLATE", "MEMEX_LDAP_BIND_DN",
        "MEMEX_LDAP_BIND_PASSWORD", "MEMEX_LDAP_MEMBEROF_ATTR",
        "MEMEX_LDAP_GROUP_ROLE"}) {
    ::unsetenv(n);
  }
}

} // namespace

int main() {
  clear_env();

  // —— 默认值（裸 serve＝argc 0 口径）——
  {
    ServeArgs a;
    CHECK(run({}, a) == 0);
    CHECK(a.port == kDefaultPort);
    CHECK(a.webhook_port == kDefaultWebhookPort);
    CHECK(a.model_port == 0);
    CHECK(a.files_port == 0);
    CHECK(a.uplink_port == 0);
    CHECK(a.s3.region == "auto");
    CHECK(a.s3.endpoint.empty());
    CHECK(a.uplink_policy.ext_denylist.size() == 23); // 缺省危险扩展集
    CHECK(a.uplink_policy.ext_allowlist.empty());
    CHECK(a.uplink_policy.max_upload_bytes == 0);
    CHECK(a.ldap_cfg.host.empty());
    CHECK(a.ldap_cfg.port == 389);
    CHECK(a.assistant_bot.empty());
    CHECK(a.tool_cred_secret.empty());
    CHECK(a.task_pull_secret.empty());
  }

  // —— 消息/webhook/模型/文件/uplink 端口旗标＋界 ——
  {
    ServeArgs a;
    CHECK(run({"--port", "24370", "--webhook-port", "0", "--model-port",
               "24362", "--files-port", "24380", "--uplink-port", "24390"},
              a) == 0);
    CHECK(a.port == 24370);
    CHECK(a.webhook_port == 0); // 0＝整体关闭
    CHECK(a.model_port == 24362);
    CHECK(a.files_port == 24380);
    CHECK(a.uplink_port == 24390);
  }
  {
    ServeArgs a;
    CHECK(run({"--port", "0"}, a) == 2); // 0＝无效消息端口
    CHECK(run({"--webhook-port", "65536"}, a) == 2);
    CHECK(run({"--webhook-port", "-1"}, a) == 2);
    CHECK(run({"--model-port", "-2"}, a) == 2);
    CHECK(run({"--files-port", "70000"}, a) == 2);
    // 严格解析（codex-761）：非数字不再 atoi→0 静默当「接受为关闭」，
    // 溢出不再回绕（atoi("99999999999")→截断值）——一律 rc2
    CHECK(run({"--uplink-port", "abc"}, a) == 2);
    CHECK(run({"--uplink-port", "12ab"}, a) == 2);
    CHECK(run({"--uplink-port", ""}, a) == 2); // 空串＝缺值语义
    CHECK(run({"--port", "99999999999"}, a) == 2); // 溢出不回绕
    CHECK(run({"--port", "-1"}, a) == 2); // 负值（uint16_t 静默换 65535 死）
    CHECK(run({"--ldap-timeout-ms", "-5"}, a) == 2);
    CHECK(run({"--ldap-recheck-seconds", "-1"}, a) == 2);
  }

  // —— 严格解析腿（codex-761）：未知旗标/位置参数/缺值/布尔带值 rc2；
  //    --flag=value 内联形态全旗标可用 ——
  {
    ServeArgs a;
    CHECK(run({"--prot", "24370"}, a) == 2); // 未知旗标（拼写错不静默漏配）
    CHECK(run({"extra"}, a) == 2);           // 位置参数（sub_argv 无裸词）
    CHECK(run({"--port"}, a) == 2);          // 缺值（尾旗标不再静默跳过）
    CHECK(run({"--s3-endpoint"}, a) == 2);   // 缺值（串旗标同口径）
    CHECK(run({"--s3-use-ssl=true"}, a) == 2); // 布尔旗标带值
    CHECK(run({"--ldap-starttls=1"}, a) == 2);
    // 内联形态（值旗标全支持）
    CHECK(run({"--port=24371", "--files-port=24381"}, a) == 0);
    CHECK(a.port == 24371);
    CHECK(a.files_port == 24381);
    CHECK(run({"--s3-endpoint=http://x:9000", "--uplink-max-mb=2",
               "--ldap-group-role=g=admin", "--assistant=bot-x"},
              a) == 0);
    CHECK(a.s3.endpoint == "http://x:9000");
    CHECK(a.uplink_policy.max_upload_bytes == 2 * 1024 * 1024);
    CHECK(a.ldap_cfg.group_roles.size() == 1);
    CHECK(a.assistant_bot == "bot-x");
    CHECK(run({"--uplink-ext-denylist=exe,dll"}, a) == 0);
    CHECK(a.uplink_policy.ext_denylist.size() == 2);
    // argv 契约腿（codex-765）：必须传剥离子命令后的段——argv[0] 带
    // 程序名（未剥离的完整 argv 误用形态）按位置参数 rc2 拒
    CHECK(run({"memex_server", "--port", "24370"}, a) == 2);
  }

  // —— 群内助手与凭据/拉取密钥旗标 ——
  {
    ServeArgs a;
    CHECK(run({"--assistant", "巡检机器人", "--assistant-token", "tok-1",
               "--tool-cred-secret", "cred-s", "--task-pull-secret",
               "pull-s"},
              a) == 0);
    CHECK(a.assistant_bot == "巡检机器人");
    CHECK(a.assistant_token == "tok-1");
    CHECK(a.tool_cred_secret == "cred-s");
    CHECK(a.task_pull_secret == "pull-s");
  }

  // —— uplink 防护参数：黑/白名单整表替换、空表清空、max-mb、二次口令 ——
  {
    ServeArgs a;
    CHECK(run({"--uplink-ext-denylist", "exe,dll"}, a) == 0);
    CHECK(a.uplink_policy.ext_denylist.size() == 2);
    CHECK(a.uplink_policy.ext_denylist[0] == "exe");
    CHECK(run({"--uplink-ext-denylist", "exe,,dll"}, a) == 0);
    CHECK(a.uplink_policy.ext_denylist.size() == 2); // 空白项剔除
    CHECK(run({"--uplink-ext-denylist", ""}, a) == 0);
    CHECK(a.uplink_policy.ext_denylist.empty()); // 空串=清空=不限
    CHECK(run({"--uplink-ext-allowlist", "pdf,png"}, a) == 0);
    CHECK(a.uplink_policy.ext_allowlist.size() == 2); // 白名单模式启用
  }
  {
    ServeArgs a;
    CHECK(run({"--uplink-max-mb", "5"}, a) == 0);
    CHECK(a.uplink_policy.max_upload_bytes == 5 * 1024 * 1024);
    CHECK(run({"--uplink-max-mb", "0"}, a) == 0);
    CHECK(a.uplink_policy.max_upload_bytes == 0);
    CHECK(run({"--uplink-max-mb", "-1"}, a) == 2);
    CHECK(run({"--uplink-login-secret", "s3cret"}, a) == 0);
    CHECK(a.uplink_policy.login_secret == "s3cret");
  }

  // —— S3 旗标 ——
  {
    ServeArgs a;
    CHECK(run({"--s3-endpoint", "http://127.0.0.1:9000", "--s3-bucket",
               "b1", "--s3-access-key", "ak", "--s3-secret-key", "sk",
               "--s3-region", "us-east-1", "--s3-use-ssl"},
              a) == 0);
    CHECK(a.s3.endpoint == "http://127.0.0.1:9000");
    CHECK(a.s3.bucket == "b1");
    CHECK(a.s3.access_key == "ak");
    CHECK(a.s3.secret_key == "sk");
    CHECK(a.s3.region == "us-east-1"); // 旗标覆盖 auto
    CHECK(a.s3.use_ssl);
    CHECK(a.s3.path_style == true); // S3 兼容层默认 path-style
  }

  // —— S3 env 兜底＋旗标恒赢 ——
  {
    ServeArgs a;
    ::setenv("MEMEX_S3_ENDPOINT", "http://env:9000", 1);
    ::setenv("MEMEX_S3_BUCKET", "env-b", 1);
    ::setenv("MEMEX_S3_ACCESS_KEY", "env-ak", 1);
    ::setenv("MEMEX_S3_SECRET_KEY", "env-sk", 1);
    CHECK(run({}, a) == 0);
    CHECK(a.s3.endpoint == "http://env:9000");
    CHECK(a.s3.bucket == "env-b");
    CHECK(a.s3.access_key == "env-ak");
    CHECK(a.s3.secret_key == "env-sk");
    CHECK(run({"--s3-endpoint", "http://flag:9000"}, a) == 0);
    CHECK(a.s3.endpoint == "http://flag:9000"); // 旗标恒赢
    CHECK(a.s3.bucket == "env-b"); // 其余仍走 env
    clear_env();
  }

  // —— LDAP 旗标全参数面 ——
  {
    ServeArgs a;
    CHECK(run({"--ldap-host", "ldap.corp", "--ldap-port", "636",
               "--ldap-ssl", "--ldap-timeout-ms", "5000", "--ldap-user-base",
               "ou=people,dc=corp", "--ldap-user-attr", "sAMAccountName",
               "--ldap-bind-dn-template", "uid={u},ou=people,dc=corp",
               "--ldap-bind-dn", "cn=svc", "--ldap-bind-password", "pw",
               "--ldap-memberof-attr", "memberOf", "--ldap-disabled-attr",
               "userAccountControl", "--ldap-disabled-rule",
               "1.2.840.113556.1.4.803", "--ldap-disabled-value", "2",
               "--ldap-group-role", "admins=admin",
               "--ldap-group-role", "ops=operator",
               "--ldap-starttls", "--ldap-allow-plain",
               "--ldap-tls-insecure", "--ldap-auto-bind",
               "--ldap-auto-provision", "--ldap-kick-disabled", "0",
               "--ldap-recheck-seconds", "30"},
              a) == 0);
    CHECK(a.ldap_cfg.host == "ldap.corp");
    CHECK(a.ldap_cfg.port == 636);
    CHECK(a.ldap_cfg.use_ssl);
    CHECK(a.ldap_cfg.timeout_ms == 5000);
    CHECK(a.ldap_cfg.user_base == "ou=people,dc=corp");
    CHECK(a.ldap_cfg.user_attr == "sAMAccountName");
    CHECK(a.ldap_cfg.bind_dn_template == "uid={u},ou=people,dc=corp");
    CHECK(a.ldap_cfg.bind_dn == "cn=svc");
    CHECK(a.ldap_cfg.bind_password == "pw");
    CHECK(a.ldap_cfg.memberof_attr == "memberOf");
    CHECK(a.ldap_cfg.disabled_attr == "userAccountControl");
    CHECK(a.ldap_cfg.disabled_rule == "1.2.840.113556.1.4.803");
    CHECK(a.ldap_cfg.disabled_value == "2");
    CHECK(a.ldap_cfg.group_roles.size() == 2);
    if (a.ldap_cfg.group_roles.size() == 2) {
      CHECK(a.ldap_cfg.group_roles[0].first == "admins");
      CHECK(a.ldap_cfg.group_roles[0].second == "admin");
      CHECK(a.ldap_cfg.group_roles[1].first == "ops");
      CHECK(a.ldap_cfg.group_roles[1].second == "operator");
    }
    CHECK(a.ldap_cfg.starttls);
    CHECK(a.ldap_cfg.allow_plain);
    CHECK(a.ldap_cfg.insecure_tls);
    CHECK(a.ldap_cfg.auto_bind);
    CHECK(a.ldap_cfg.auto_provision);
    CHECK(a.ldap_cfg.kick_disabled == false); // 显式 0=仅拒新登录
    CHECK(a.ldap_cfg.recheck_seconds == 30);
  }
  {
    ServeArgs a;
    CHECK(run({"--ldap-kick-disabled", "1"}, a) == 0);
    CHECK(a.ldap_cfg.kick_disabled == true);
    CHECK(run({"--ldap-port", "0"}, a) == 2);
    CHECK(run({"--ldap-port", "70000"}, a) == 2);
  }

  // —— LDAP 组→角色坏形态 rc2 ——
  {
    ServeArgs a;
    CHECK(run({"--ldap-group-role", "noequals-sign"}, a) == 2);
    CHECK(run({"--ldap-group-role", "=admin"}, a) == 2); // 组空
    CHECK(run({"--ldap-group-role", "group="}, a) == 2); // 角色空
  }

  // —— LDAP env 兜底（含组映射逗号分隔多组）＋旗标恒赢 ——
  {
    ServeArgs a;
    ::setenv("MEMEX_LDAP_HOST", "env.corp", 1);
    ::setenv("MEMEX_LDAP_USER_BASE", "ou=env,dc=corp", 1);
    ::setenv("MEMEX_LDAP_BIND_DN_TEMPLATE", "uid={u},ou=env,dc=corp", 1);
    ::setenv("MEMEX_LDAP_BIND_DN", "cn=env-svc", 1);
    ::setenv("MEMEX_LDAP_BIND_PASSWORD", "env-pw", 1);
    ::setenv("MEMEX_LDAP_MEMBEROF_ATTR", "envMemberOf", 1);
    ::setenv("MEMEX_LDAP_GROUP_ROLE", "g1=admin,b=op=user", 1);
    CHECK(run({}, a) == 0);
    CHECK(a.ldap_cfg.host == "env.corp");
    CHECK(a.ldap_cfg.user_base == "ou=env,dc=corp");
    CHECK(a.ldap_cfg.bind_dn_template == "uid={u},ou=env,dc=corp");
    CHECK(a.ldap_cfg.bind_dn == "cn=env-svc");
    CHECK(a.ldap_cfg.bind_password == "env-pw");
    // memberof_attr 默认 "memberOf" 非空→env 兜底不生效（原版行为逐字
    // 保留；env 仅在旗标显式置空后才可注入）
    CHECK(a.ldap_cfg.memberof_attr == "memberOf");
    CHECK(a.ldap_cfg.group_roles.size() == 2);
    if (a.ldap_cfg.group_roles.size() == 2) {
      CHECK(a.ldap_cfg.group_roles[0].first == "g1");
      CHECK(a.ldap_cfg.group_roles[0].second == "admin");
      CHECK(a.ldap_cfg.group_roles[1].first == "b");
      CHECK(a.ldap_cfg.group_roles[1].second == "op=user"); // 值含=不截断
    }
    CHECK(run({"--ldap-host", "flag.corp"}, a) == 0);
    CHECK(a.ldap_cfg.host == "flag.corp"); // 旗标恒赢
    clear_env();
  }
  // env 组映射空串＝不加映射（坏组段静默忽略，与原口径一致）
  {
    ServeArgs a;
    ::setenv("MEMEX_LDAP_GROUP_ROLE", "", 1);
    CHECK(run({}, a) == 0);
    CHECK(a.ldap_cfg.group_roles.empty());
    clear_env();
  }

  if (g_failures == 0) std::cout << "serve_args: all ok\n";
  else std::cout << "serve_args: " << g_failures << " failures\n";
  return g_failures == 0 ? 0 : 1;
}
