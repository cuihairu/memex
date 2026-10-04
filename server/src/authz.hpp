// 统一权限模型判权骨架（平台-1，docs/design/权限模型.md）：R23-R26 各面
// 共用的裁决引擎。判权=纯函数：subject(账号)/action(动作)/resource(资源)/
// context(场景键) → 裁决+理由，本类不做 IO 不碰库——规则谓词由各功能面
// 注册（谓词内部自行查元数据层），本类只管冲突规则与理由留痕。
// 冲突规则写死：Explicit Deny > Allow > Inherited > Default Deny（白名单
// 口径：无规则命中即拒）；同优先级内先注册先匹配（确定性，测试锁定）。
// 留痕原则：裁决连同理由由调用方记审计，本类不落库（避免双写漂移）。
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace memex::server {

// 规则效果三档，优先级从高到低：
// - ExplicitDeny  显式拒绝（最高，压过一切 Allow，如隔离文件禁止读取）
// - ExplicitAllow 显式允许（直接授予：资源属主、群主/管理员的管理权）
// - Inherited     继承允许（入群即继承：群成员对群资源的既定能力）
enum class RuleEffect { ExplicitDeny, ExplicitAllow, Inherited };

struct AuthzQuery {
  std::string subject;   // 行使人（账号）
  std::string action;    // 动作，如 file:read / file:upload / file:delete
  std::string resource;  // 资源，如 group:3/file:12、user:alice/file:7、group:3
  std::string context;   // 场景键（自由文本：来源、设备、目标归属等）
};

struct Decision {
  bool allowed{false};
  std::string reason;  // 命中规则名（"deny:quarantined"/"allow:owner"/
                       // "inherited:member"/"default-deny"），审计留痕用
};

class AuthorizationService {
 public:
  using Matcher = std::function<bool(const AuthzQuery&)>;

  // 注册一条规则。name 进 Decision::reason（须可读、稳定——审计依赖）。
  void add_rule(RuleEffect effect, const std::string& name, Matcher match);

  // 裁决：按 ExplicitDeny → ExplicitAllow → Inherited 顺序求首个命中；
  // 全部未命中 → {false, "default-deny"}。
  Decision authorize(const AuthzQuery& q) const;

 private:
  struct Rule {
    RuleEffect effect;
    std::string name;
    Matcher match;
  };
  std::vector<Rule> rules_;  // 注册序即同档匹配序
};

} // namespace memex::server
