// 判权骨架验收（平台-1）：冲突规则 Explicit Deny > Allow > Inherited >
// Default Deny 写死、同档先注册先匹配（确定性）、理由可留痕。
// 骨架是纯函数引擎（不做 IO）——带库查询的真实策略在 FileServer 面测。
#include <iostream>
#include <string>

#include "authz.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond    \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

using memex::server::AuthzQuery;
using memex::server::AuthorizationService;
using memex::server::Decision;
using memex::server::RuleEffect;

AuthzQuery q(const std::string& subject, const std::string& action,
             const std::string& resource, const std::string& ctx = "") {
  return AuthzQuery{subject, action, resource, ctx};
}

} // namespace

int main() {
  // —— Default Deny：无任何规则命中即拒（白名单口径）——
  {
    AuthorizationService az; // 空规则集
    const Decision d = az.authorize(q("alice", "file:read", "group:1/file:9"));
    CHECK(!d.allowed);
    CHECK(d.reason == "default-deny");
  }

  // —— 优先级：Deny 压 Allow 压 Inherited ——
  {
    AuthorizationService az;
    // 谓词以「场景键含标记」模拟多规则并中（真实策略按字段判，见下段）
    az.add_rule(RuleEffect::ExplicitDeny, "quarantined",
                [](const AuthzQuery& x) {
                  return x.context.find("quarantined") != std::string::npos;
                });
    az.add_rule(RuleEffect::ExplicitAllow, "owner",
                [](const AuthzQuery& x) {
                  return x.context.find("owner") != std::string::npos;
                });
    az.add_rule(RuleEffect::Inherited, "member",
                [](const AuthzQuery& x) {
                  return x.context.find("member") != std::string::npos;
                });

    // 全部命中 → Deny 赢，理由=命中的 deny 规则
    Decision d = az.authorize(q("alice", "file:read", "group:1/file:9",
                                "quarantined-owner-member"));
    CHECK(!d.allowed);
    CHECK(d.reason == "deny:quarantined");

    // Deny 未中、Allow 中 → Allow 赢（Inherited 同中也不翻盘）
    d = az.authorize(q("alice", "file:read", "group:1/file:9",
                       "owner-member"));
    CHECK(d.allowed);
    CHECK(d.reason == "allow:owner");

    // 只剩 Inherited → 继承允许
    d = az.authorize(q("alice", "file:read", "group:1/file:9", "member"));
    CHECK(d.allowed);
    CHECK(d.reason == "inherited:member");

    // 全未中 → Default Deny
    d = az.authorize(q("alice", "file:read", "group:1/file:9", ""));
    CHECK(!d.allowed);
    CHECK(d.reason == "default-deny");
  }

  // —— 同档先注册先匹配：两条 Allow 都命中时返回先注册者 ——
  {
    AuthorizationService az;
    az.add_rule(RuleEffect::ExplicitAllow, "first",
                [](const AuthzQuery&) { return true; });
    az.add_rule(RuleEffect::ExplicitAllow, "second",
                [](const AuthzQuery&) { return true; });
    const Decision d = az.authorize(q("s", "a", "r"));
    CHECK(d.allowed && d.reason == "allow:first");
  }

  // —— 谓词按字段判：action/resource 维度各自成立 ——
  {
    AuthorizationService az;
    az.add_rule(RuleEffect::ExplicitAllow, "group-admin",
                [](const AuthzQuery& x) {
                  return x.action != "file:read" &&
                         x.resource.rfind("group:", 0) == 0 &&
                         x.subject == "boss";
                });
    // 管理动作＋群资源＋群主 → 允许
    CHECK(az.authorize(q("boss", "file:delete", "group:3/file:9")).allowed);
    // 读动作不在此规则内 → Default Deny（读走 Inherited 规则，另行注册）
    const Decision d = az.authorize(q("boss", "file:read", "group:3/file:9"));
    CHECK(!d.allowed && d.reason == "default-deny");
    // 同动作他人 → 拒
    CHECK(!az.authorize(q("mallory", "file:delete", "group:3/file:9")).allowed);
  }

  if (g_failures == 0) {
    std::cout << "test_authz: all checks passed\n";
    return 0;
  }
  std::cout << "test_authz: " << g_failures << " check(s) FAILED\n";
  return 1;
}
