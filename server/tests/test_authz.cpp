// 判权骨架验收（平台-1）：冲突规则 Explicit Deny > Allow > Inherited >
// Default Deny 写死、同档内最近作用域优先（计划书§八）、同深同档先注册
// 先匹配（确定性）、理由可留痕。
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

  // —— §八 作用域与继承（计划书 line235）：同档内最近作用域优先 ——
  {
    AuthorizationService az;
    // 三级部门链：公司/技术中心/游戏部；谓词恒真，只考作用域解近
    const auto all = [](const AuthzQuery&) { return true; };
    az.add_scoped_rule(RuleEffect::ExplicitAllow, "hq-allow", "公司", all);
    az.add_scoped_rule(RuleEffect::ExplicitAllow, "tc-allow", "公司/技术中心",
                       all);
    az.add_scoped_rule(RuleEffect::ExplicitDeny, "gd-deny",
                       "公司/技术中心/游戏部", all);

    // 最深作用域 Deny 命中 → 压过两层 Allow（档序全局优先，与深度无关）
    Decision d = az.authorize(q("alice", "file:read", "group:1/file:9", "",
                                "公司/技术中心/游戏部"));
    CHECK(!d.allowed);
    CHECK(d.reason == "deny:gd-deny");

    // 技术中心层：gd-deny 离链不命中，tc-allow 近于 hq-allow → 最近作用域赢
    d = az.authorize(q("alice", "file:read", "group:1/file:9", "",
                       "公司/技术中心"));
    CHECK(d.allowed);
    CHECK(d.reason == "allow:tc-allow");

    // 公司层：仅 hq-allow 在链上
    d = az.authorize(q("alice", "file:read", "group:1/file:9", "", "公司"));
    CHECK(d.allowed);
    CHECK(d.reason == "allow:hq-allow");

    // 离链部门（公司/市场部）：gd-deny 不命中，hq-allow 覆盖 → 允许
    d = az.authorize(q("alice", "file:read", "group:1/file:9", "",
                       "公司/市场部"));
    CHECK(d.allowed);
    CHECK(d.reason == "allow:hq-allow");

    // 查询无作用域：带作用域规则全不覆盖 → Default Deny
    d = az.authorize(q("alice", "file:read", "group:1/file:9", ""));
    CHECK(!d.allowed);
    CHECK(d.reason == "default-deny");

    // 全局规则（无作用域）恒覆盖，深度 0 输给链上更深的 scoped 规则
    az.add_rule(RuleEffect::ExplicitAllow, "global-allow", all);
    d = az.authorize(q("alice", "file:read", "group:1/file:9", ""));
    CHECK(d.allowed);
    CHECK(d.reason == "allow:global-allow");
    d = az.authorize(q("alice", "file:read", "group:1/file:9", "", "公司"));
    CHECK(d.allowed);
    CHECK(d.reason == "allow:hq-allow");

    // 同深同档先注册先匹配：两条同作用域 Allow 都命中取先注册者
    AuthorizationService az2;
    az2.add_scoped_rule(RuleEffect::ExplicitAllow, "first", "公司/技术中心",
                        all);
    az2.add_scoped_rule(RuleEffect::ExplicitAllow, "second", "公司/技术中心",
                        all);
    d = az2.authorize(q("alice", "file:read", "group:1/file:9", "",
                        "公司/技术中心"));
    CHECK(d.allowed);
    CHECK(d.reason == "allow:first");

    // 前缀相似非链上：公司/技术 不是 公司/技术中心的祖先（须整段+分隔符）
    AuthorizationService az3;
    az3.add_scoped_rule(RuleEffect::ExplicitAllow, "tc-allow", "公司/技术中心",
                        all);
    d = az3.authorize(q("alice", "file:read", "group:1/file:9", "",
                        "公司/技术"));
    CHECK(!d.allowed);
    CHECK(d.reason == "default-deny");
  }

  // —— 数据过滤与单点同源：filter_allowed 逐条过同一套作用域规则桶 ——
  {
    AuthorizationService az;
    const auto all = [](const AuthzQuery&) { return true; };
    az.add_scoped_rule(RuleEffect::ExplicitAllow, "hq-allow", "公司", all);
    az.add_scoped_rule(RuleEffect::ExplicitDeny, "gd-deny",
                       "公司/技术中心/游戏部", all);
    const auto query_for = [](std::size_t i) {
      static const char* kScopes[] = {"公司", "公司/技术中心/游戏部"};
      return AuthzQuery{"alice", "file:read", "group:1/file:" + std::to_string(i),
                        "", kScopes[i]};
    };
    const auto hits = az.filter_allowed(2, query_for);
    CHECK(hits.size() == 1);
    CHECK(hits[0].index == 0);
    CHECK(hits[0].reason == "allow:hq-allow");
  }

  if (g_failures == 0) {
    std::cout << "test_authz: all checks passed\n";
    return 0;
  }
  std::cout << "test_authz: " << g_failures << " check(s) FAILED\n";
  return 1;
}
