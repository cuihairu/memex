#include "authz.hpp"

namespace memex::server {
namespace {

// 作用域深度：空=全局（0），否则路径段数（公司/技术中心 → 2）。
std::size_t scope_depth(const std::string& scope) {
  if (scope.empty()) {
    return 0;
  }
  std::size_t depth = 1;
  for (char c : scope) {
    if (c == '/') {
      ++depth;
    }
  }
  return depth;
}

// 规则作用域是否覆盖查询作用域：全局规则恒覆盖；非空规则要求查询作用域
// 等于该作用域或在其辖下（前缀+分隔符）——离链不命中（计划书§八）。
bool scope_covers(const std::string& rule_scope, const std::string& q_scope) {
  if (rule_scope.empty()) {
    return true;
  }
  if (q_scope.empty() || q_scope.size() < rule_scope.size()) {
    return false;
  }
  if (q_scope.compare(0, rule_scope.size(), rule_scope) != 0) {
    return false;
  }
  return q_scope.size() == rule_scope.size() ||
         q_scope[rule_scope.size()] == '/';
}

} // namespace

void AuthorizationService::add_rule(RuleEffect effect, const std::string& name,
                                    Matcher match) {
  rules_.push_back(Rule{effect, name, std::string(), std::move(match)});
}

void AuthorizationService::add_scoped_rule(RuleEffect effect,
                                           const std::string& name,
                                           const std::string& scope,
                                           Matcher match) {
  rules_.push_back(Rule{effect, name, scope, std::move(match)});
}

Decision AuthorizationService::authorize(const AuthzQuery& q) const {
  // 冲突规则写死（authz.hpp 顶注）：Deny 压 Allow 压 Inherited、全未命中
  // 即 Default Deny；档间次序全局优先（深作用域 Deny 压浅作用域 Allow）。
  // 每档单遍扫描：谓词命中且规则作用域在查询链上者为候选，取最近作用域
  // （同深先注册——严格大于才换人，保序）。
  static constexpr const char* kDefaultDeny = "default-deny";
  const std::pair<const char*, RuleEffect> kTiers[] = {
      {"deny:", RuleEffect::ExplicitDeny},
      {"allow:", RuleEffect::ExplicitAllow},
      {"inherited:", RuleEffect::Inherited},
  };
  for (const auto& tier : kTiers) {
    const Rule* best = nullptr;
    std::size_t best_depth = 0;
    for (const auto& r : rules_) {
      if (r.effect != tier.second || !r.match(q) ||
          !scope_covers(r.scope, q.scope)) {
        continue;
      }
      const std::size_t depth = scope_depth(r.scope);
      if (best == nullptr || depth > best_depth) {
        best = &r;
        best_depth = depth;
      }
    }
    if (best != nullptr) {
      return {tier.second != RuleEffect::ExplicitDeny,
              std::string(tier.first) + best->name};
    }
  }
  return {false, kDefaultDeny};
}

std::vector<AuthorizationService::FilterHit>
AuthorizationService::filter_allowed(
    std::size_t count,
    const std::function<AuthzQuery(std::size_t)>& query_for) const {
  std::vector<FilterHit> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const Decision d = authorize(query_for(i));
    if (d.allowed) {
      out.push_back(FilterHit{i, d.reason});
    }
  }
  return out;
}

} // namespace memex::server
