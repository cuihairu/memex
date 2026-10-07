// R27-2 外部任务 Provider SPI：能力声明制（L1 跳转/L2 只读/L3 双向），
// 工具只实现自己支持的级别。L1=带 detailUrl 打开外部详情（URL 模板即可，
// 成本最低）；L2=拉外部任务列表展示；L3=memex 内创建/完成回写外部。
// 「跳转链接是基本件」：声明含 L1 的 provider，对合法键必须给出非空
// detail URL——解析不出即本地拒（不发网不落库，宁缺毋滥造坏链）。
// 设计稿：docs/design/个人任务清单与外部工具接入.md §二。
#pragma once

#include <QString>
#include <QVector>

namespace memex::client {

// 能力位（位或组合；capabilities() 返回 int 便于跨边界传递）
enum TaskProviderCaps {
  kCapL1Jump = 1,  // 跳转外部详情（detailUrl 必带）
  kCapL2Read = 2,  // 拉外部任务列表（只读）
  kCapL3Write = 4, // memex 内创建/完成回写外部
};

enum class TaskProviderAuth {
  kNone,  // 无凭据（L1 模板/直通链接）
  kToken, // 令牌（PAT/tenant token 等；凭据走加密面，随 R27-3 实做）
  kOAuth, // OAuth 授权流（随 R27-3 实做）
};

// 外部任务条目（L2 拉取的形态；L1 手工登记同构）
struct ExternalTask {
  QString provider_id;
  QString key;        // 外部键（issue 号/任务 ID）
  QString project;    // 可选前缀槽（仓库 org/repo、站点域名等）
  QString title;
  QString detail_url; // L1 必带
};

class TaskProvider {
 public:
  virtual ~TaskProvider() = default;

  virtual QString id() const = 0;
  virtual QString name() const = 0;
  // 凭据形态（默认无凭据；token/oauth 随 R27-3 各 provider 实做）
  virtual TaskProviderAuth auth_kind() const {
    return TaskProviderAuth::kNone;
  }
  // 能力声明（位或 TaskProviderCaps）
  virtual int capabilities() const = 0;

  // L1 必带：外部键（＋可选 project 槽）→ 外部详情 URL。
  // 键为空/模板缺槽/不合法回空串（调用方据此本地拒）。
  virtual QString detail_url(const QString& key,
                             const QString& project = QString()) const = 0;

  // L2/L3 默认不支持：能力声明未开的级别，基类直接回「不可用」，
  // 不抛异常不空转。
  virtual QVector<ExternalTask> list() const { return {}; }
  virtual bool create(const QString& /*title*/, const QString& /*note*/) {
    return false;
  }
  virtual bool complete(const QString& /*key*/) { return false; }

  bool can_jump() const { return (capabilities() & kCapL1Jump) != 0; }
  bool can_read() const { return (capabilities() & kCapL2Read) != 0; }
  bool can_write() const { return (capabilities() & kCapL3Write) != 0; }
};

// URL 模板 provider（L1 框架件）：模板含 {key}（必含）与 {project}
//（可选站点/仓库槽）。键直替不编码——预设模板的键域（issue 号/Jira 键）
// 不含保留字符；任意字符场景走直通链接 provider。
class UrlTemplateProvider : public TaskProvider {
 public:
  UrlTemplateProvider(QString id, QString name, QString url_template,
                      int caps = kCapL1Jump)
      : id_(std::move(id)), name_(std::move(name)),
        tpl_(std::move(url_template)), caps_(caps) {}

  QString id() const override { return id_; }
  QString name() const override { return name_; }
  int capabilities() const override { return caps_; }
  QString detail_url(const QString& key,
                     const QString& project = QString()) const override;

 private:
  QString id_;
  QString name_;
  QString tpl_;
  int caps_;
};

// 直通链接 provider（id="url"）：键即完整 http(s) 链接——任意工具
// 「粘贴链接即登记」，框架的最小可用件（跳转链接是基本件的兜底形态）。
class UrlPassthroughProvider : public TaskProvider {
 public:
  QString id() const override { return QStringLiteral("url"); }
  QString name() const override { return QStringLiteral("直通链接"); }
  int capabilities() const override { return kCapL1Jump; }
  QString detail_url(const QString& key,
                     const QString& project = QString()) const override;
};

// Provider 注册表：内置 L1 预设（GitHub Issue/PR、GitLab、Jira、Linear、
// 直通链接）＋ add() 挂自定义模板/后续真 provider（设置页与凭据面随
// R27-3 落地时接）。登记键的书写约定：「project#键」或完整链接
//（无 # = 整串为键），解析归调用方。
class TaskProviderRegistry {
 public:
  TaskProviderRegistry(); // 装配内置预设
  ~TaskProviderRegistry();
  TaskProviderRegistry(const TaskProviderRegistry&) = delete;
  TaskProviderRegistry& operator=(const TaskProviderRegistry&) = delete;

  void add(TaskProvider* p); // 接管所有权
  const TaskProvider* provider(const QString& id) const; // 未知=nullptr
  QVector<const TaskProvider*> providers() const;        // 装配序
  // 便利：id＋键（＋可选 project）直解 URL；provider 未知/无跳转/
  // 键不合法=空串
  QString detail_url(const QString& id, const QString& key,
                     const QString& project = QString()) const;

 private:
  QVector<TaskProvider*> items_;
};

} // namespace memex::client
