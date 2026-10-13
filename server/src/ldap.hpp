// LDAP/AD 可选企业身份后端（评审「外部建议三步走」第②步）：最小 LDAP
// 客户端——BER 编解码＋bind/search，不引 openldap 重依赖（业务层自写
// 路线，依赖清单见 third_party/README.md）。
//
// 定位（todo 544）：可插拔认证后端，不改轻量部署与免登录直连的独立信任
// 模型——不配 --ldap-host 服务端行为与旧版逐字节一致。
//
// 红线口径：
// - fail-closed：目录不可用＝拒绝登录，绝不回落本地口令（回落＝绕过目录）；
// - 密码不明文：口令只在 bind 报文里走一次，不落日志/库/LoginRecord；
// - 组→角色显式映射：配置外组一概忽略，映射表不含 admin 时永不授管理员；
// - 本地账号常备：未绑 issuer=ldap 的账号走原本地判登（应急恢复入口）。
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "store.hpp" // IdentityBinding 完整类型（绑定查找返回值）

namespace memex::server {

struct LdapConfig {
  std::string host;
  std::uint16_t port{389};
  bool use_ssl{false};   // ldaps:// 整连接 TLS（636 口径）
  bool starttls{false};  // 明连后升级 TLS（RFC 4511 §4.14，389 口径）
  bool allow_plain{false}; // 明文 bind 显式放行（默认拒——密码不明文红线；
                           // 仅限测试/受控内网；启动日志明示）
  bool insecure_tls{false}; // 自签证书：跳过证书校验（启动明示警告）
  int timeout_ms{4000};

  // 用户定位：subtree 搜 user_base 下 user_attr==用户名 → 恰 1 条目。
  std::string user_base;      // 如 "ou=people,dc=corp"
  std::string user_attr{"uid"}; // AD 常用 sAMAccountName
  // 绑定两条路径（二选一）：
  // ① 模板直绑（OpenLDAP 常用）：{u} 替换为用户名。
  std::string bind_dn_template; // 如 "uid={u},ou=people,dc=corp"
  // ② 服务账号先查后绑（AD 常用：DN 拼不出，须先查）。
  std::string bind_dn;       // 服务账号 DN
  std::string bind_password; // env MEMEX_LDAP_BIND_PASSWORD 注入优先
  // 组：从用户条目 memberOf 属性读（AD 标配；OpenLDAP 需 memberof
  // overlay——属性名可改）。目录无 memberOf 时映射恒空＝仅认证不授角色。
  std::string memberof_attr{"memberOf"};
  // 禁用检测（可选）：base=用户 DN 的 extensibleMatch 三元组——AD 停用位
  // 为 attr=userAccountControl, rule=1.2.840.113556.1.4.803, value=2。
  std::string disabled_attr;
  std::string disabled_rule;
  std::string disabled_value;
  // 组→角色显式映射（组名大小写不敏感；映射表外组一律普通成员——
  // 不做任何自动授权；映射表不写 admin 就永不授管理员）。
  std::vector<std::pair<std::string, std::string>> group_roles;

  // —— 首登绑定与影子账号（默认全关＝显式绑定口径，见 route_authentication）——
  // 已有本地账号无绑定时：目录认证成功即自动 identity_bind
  //（issuer=ldap://host，subject=用户 DN）。关＝只认 CLI 显式绑定。
  bool auto_bind{false};
  // 账号不存在时：目录认证成功即建影子账号（默认关，显式开启才建；
  // 本地口令=随机不可知＝LDAP-only，本地判登实际封死）。
  bool auto_provision{false};

  // —— 目录禁用 × 存量会话策略（默认踢下线，可配）——
  // 复检发现目录侧禁用 → 在线会话踢下线（默认 true）。
  bool kick_disabled{true};
  // 存量会话复检间隔秒（0=不复检；仅服务模式可查——模板模式无凭据
  // 自查，启动时明示该策略不生效）。
  int recheck_seconds{60};

  // host＋用户定位＋一条绑定路径齐才装配。
  bool ready() const;
  // TLS 姿态校验（启动口）：use_ssl/starttls/allow_plain 三态恰一，
  // 返回空串=通过。
  std::string tls_posture_error() const;
  // 脱敏回显（bind_password 替换为固定掩码）——日志/诊断专用。
  std::string placeholder() const;
};

// 判定结果。Unavailable≠BadCredentials——fail-closed 但回因不同，巡检
// 可凭 result 串区分「口令错」与「目录不可达」。
struct LdapVerdict {
  enum class Outcome {
    Ok,
    BadCredentials,
    Disabled,
    Unavailable,
    NotConfigured,
  };
  Outcome outcome{Outcome::NotConfigured};
  std::vector<std::string> groups; // Ok 时：用户条目 memberOf 值原样
  std::string user_dn;             // 定位到的用户 DN（首登自动绑定 subject）
  std::string detail;              // 诊断（永不携带口令）
};

class LdapDirectory {
public:
  explicit LdapDirectory(LdapConfig cfg);

  // 同步一次完整认证（单连接：服务绑定→定位→禁用检测→用户绑定）。
  // 任何网络层失败（连接/超时/协议错）＝Unavailable。内置熔断：一次
  // Unavailable 后 cooldown_ms 内直接 fail-fast（collab io 单线程——
  // 目录挂掉时若无熔断，每次登录都拖住全会话一个超时时长）。
  LdapVerdict verify_user(const std::string& username,
                          const std::string& password);

  // 显式映射：组名 → 角色名（配置外组忽略；去重保序）。
  std::vector<std::string> map_roles(
      const std::vector<std::string>& groups) const;

  // 存量会话复检（目录禁用策略）：服务账号绑定＋禁用三元组查询
  // subject（用户 DN）。affirmative Disabled 才判禁用；Unavailable
  // （目录不可达）不误踢——宁可不踢也不能全员误伤。模板模式无凭据
  // 自查，返回 Ok 并在 detail 说明（调用方据此跳过）。
  LdapVerdict check_disabled(const std::string& subject);

  const LdapConfig& config() const { return cfg_; }
  // 熔断测试口：清冷却（测试逐腿独立）。
  void reset_breaker() { unavailable_until_ms_ = 0; }
  void set_cooldown_ms(std::int64_t ms) { cooldown_ms_ = ms; }

private:
  // 直连网络的一次完整认证（verify_user 外包熔断与配置检查）。
  LdapVerdict do_verify_user(const std::string& username,
                             const std::string& password);
  // 熔断＋配置预检（verify_user / check_disabled 共用）。
  bool precheck(LdapVerdict& v);

  LdapConfig cfg_;
  std::int64_t cooldown_ms_{30000};
  std::int64_t unavailable_until_ms_{0}; // 熔断过期点（epoch ms）
};

// —— 共享判登路由（消息面 session.cpp 与文件面 files_server.cpp 同源）——
// 账号绑定了 issuer=ldap 的身份 → 目录判登（未配目录也 fail-closed）；
// 未绑定 → Local＝调用方走原本地口令判登（平台-2 credentials 表，路径
// 不动）。密码永不进 reason/rec_result/日志。
struct AuthRoute {
  enum class Path {
    Local,          // 未绑定目录身份——调用方既有本地判登
    LdapOk,         // 目录认证通过（roles＝显式映射出的角色，已去重）
    LdapBad,        // 目录认证不过（凭据错/目录无此用户）
    LdapDisabled,   // 目录侧已禁用（禁用检测命中）
    LdapUnavailable,// 目录不可达/超时——fail-closed，不回落本地
  };
  Path path{Path::Local};
  std::vector<std::string> roles;
};
AuthRoute route_authentication(ServerStore& store, LdapDirectory* ldap,
                               const std::string& account,
                               const std::string& password);

// 账号的目录绑定（issuer 为 "ldap" 或 "ldap://host" 形态；无则空）。
// 判登路由与存量会话复检共用。
std::optional<ServerStore::IdentityBinding> find_ldap_binding(
    ServerStore& store, const std::string& account);

// 组→角色落库（入参＝route_authentication 映射出的角色；幂等：在窗
// 授权已存在则跳过；granted_by="ldap-connector" 台账可查来源）。返回
// 本次新授角色数。
int sync_ldap_roles(ServerStore& store, const std::string& account,
                    const std::vector<std::string>& roles,
                    std::int64_t now_ms);

} // namespace memex::server
