#include "authz.hpp"

namespace memex::server {

void AuthorizationService::add_rule(RuleEffect effect, const std::string& name,
                                    Matcher match) {
  rules_.push_back(Rule{effect, name, std::move(match)});
}

Decision AuthorizationService::authorize(const AuthzQuery& q) const {
  // 冲突规则写死（authz.hpp 顶注）：Deny 压 Allow、Allow 压 Inherited、
  // 全未命中即 Default Deny。三桶各取「先注册先匹配」。
  static constexpr const char* kDefaultDeny = "default-deny";
  for (const auto& r : rules_) {
    if (r.effect == RuleEffect::ExplicitDeny && r.match(q)) {
      return {false, "deny:" + r.name};
    }
  }
  for (const auto& r : rules_) {
    if (r.effect == RuleEffect::ExplicitAllow && r.match(q)) {
      return {true, "allow:" + r.name};
    }
  }
  for (const auto& r : rules_) {
    if (r.effect == RuleEffect::Inherited && r.match(q)) {
      return {true, "inherited:" + r.name};
    }
  }
  return {false, kDefaultDeny};
}

} // namespace memex::server
