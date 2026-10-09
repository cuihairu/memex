// R27-2 Provider SPI 实现：URL 模板/直通链接两个 L1 框架件＋注册表
// 内置预设。策略=解析不出即空串（调用方本地拒），不造半截链接。
#include "engine/task/task_provider.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <QtAlgorithms>

namespace memex::client {
namespace {
// 自定义模板持久化（QSettings 显式 org/app 构造——setPath 重定向不了
// 默认构造，同 TaskProviderStore 口径；模板非凭据明文可存）
constexpr const char* kCustomGroup = "custom_templates";
} // namespace

QString UrlTemplateProvider::detail_url(const QString& key,
                                        const QString& project) const {
  const QString k = key.trimmed();
  if (k.isEmpty()) return QString();
  // 模板缺 {key} 槽=配置错，一律无跳转（不给「整串原样」假链接）
  if (!tpl_.contains(QStringLiteral("{key}"))) return QString();
  QString out = tpl_;
  if (out.contains(QStringLiteral("{project}")) &&
      project.trimmed().isEmpty()) {
    return QString(); // 模板要 project 而未给 → 拒（不造半截链接）
  }
  out.replace(QStringLiteral("{project}"), project.trimmed());
  out.replace(QStringLiteral("{key}"), k);
  // 未知槽残留（如 {site}）→ 拒
  if (out.contains(QLatin1Char('{')) || out.contains(QLatin1Char('}'))) {
    return QString();
  }
  return out;
}

QString UrlPassthroughProvider::detail_url(const QString& key,
                                           const QString& /*project*/) const {
  const QString k = key.trimmed();
  if (!k.startsWith(QStringLiteral("http://")) &&
      !k.startsWith(QStringLiteral("https://"))) {
    return QString(); // 只放行 http(s)，其它形态拒
  }
  return k;
}

TaskProviderRegistry::TaskProviderRegistry() {
  add(new UrlTemplateProvider(
      QStringLiteral("github-issue"), QStringLiteral("GitHub Issue"),
      QStringLiteral("https://github.com/{project}/issues/{key}")));
  add(new UrlTemplateProvider(
      QStringLiteral("github-pr"), QStringLiteral("GitHub PR"),
      QStringLiteral("https://github.com/{project}/pull/{key}")));
  add(new UrlTemplateProvider(
      QStringLiteral("gitlab-issue"), QStringLiteral("GitLab Issue"),
      QStringLiteral("https://gitlab.com/{project}/-/issues/{key}")));
  // Jira 的 project 槽=站点域名（如 xx.atlassian.net）
  add(new UrlTemplateProvider(
      QStringLiteral("jira"), QStringLiteral("Jira"),
      QStringLiteral("https://{project}/browse/{key}")));
  add(new UrlTemplateProvider(
      QStringLiteral("linear"), QStringLiteral("Linear"),
      QStringLiteral("https://linear.app/{project}/issue/{key}")));
  add(new UrlPassthroughProvider());
  reload_custom();
}

TaskProviderRegistry::~TaskProviderRegistry() {
  qDeleteAll(items_);
}

void TaskProviderRegistry::reload_custom(const QString& org,
                                         const QString& app) {
  // 先把旧自定义件从 items_ 摘除再回收——items_ 与 custom_items_ 双表
  // 记账，只清一边就 delete 会留悬垂指针（provider() 扫表 UB＋析构
  // qDeleteAll(items_) 双重释放）
  for (auto* p : custom_items_) items_.removeOne(p);
  qDeleteAll(custom_items_);
  custom_items_.clear();
  for (const auto& t : custom_templates(org, app)) {
    // 与 UrlTemplateProvider 同口径：模板须含 {key} 槽；id 撞内置预设
    // 的拒挂（provider() 先命中预设，挂了也是死件）
    if (t.id.isEmpty() || t.url_template.isEmpty() ||
        !t.url_template.contains(QStringLiteral("{key}")) ||
        provider(t.id) != nullptr) {
      continue;
    }
    auto* p = new UrlTemplateProvider(t.id, t.name, t.url_template);
    custom_items_.append(p);
    items_.append(p);
  }
}

QVector<CustomTemplate> custom_templates(const QString& org,
                                         const QString& app) {
  QVector<CustomTemplate> out;
  QSettings s(org, app);
  s.beginGroup(QLatin1String(kCustomGroup));
  const QJsonDocument doc = QJsonDocument::fromJson(
      s.value(QStringLiteral("list")).toString().toUtf8());
  if (!doc.isArray()) return out;
  for (const auto& v : doc.array()) {
    const auto o = v.toObject();
    out.append({o.value(QStringLiteral("id")).toString(),
                o.value(QStringLiteral("name")).toString(),
                o.value(QStringLiteral("template")).toString()});
  }
  return out;
}

void save_custom_templates(const QVector<CustomTemplate>& list,
                           const QString& org, const QString& app) {
  QSettings s(org, app);
  s.beginGroup(QLatin1String(kCustomGroup));
  QJsonArray arr;
  for (const auto& t : list) {
    QJsonObject o;
    o.insert(QStringLiteral("id"), t.id);
    o.insert(QStringLiteral("name"), t.name);
    o.insert(QStringLiteral("template"), t.url_template);
    arr.append(o);
  }
  s.setValue(QStringLiteral("list"),
             QString::fromUtf8(QJsonDocument(arr).toJson(
                 QJsonDocument::Compact)));
}

void TaskProviderRegistry::add(TaskProvider* p) {
  if (p != nullptr) items_.append(p);
}

const TaskProvider* TaskProviderRegistry::provider(const QString& id) const {
  for (const auto* p : items_) {
    if (p->id() == id) return p;
  }
  return nullptr;
}

QVector<const TaskProvider*> TaskProviderRegistry::providers() const {
  QVector<const TaskProvider*> out;
  for (const auto* p : items_) out.append(p);
  return out;
}

QString TaskProviderRegistry::detail_url(const QString& id,
                                         const QString& key,
                                         const QString& project) const {
  const TaskProvider* p = provider(id);
  return p != nullptr ? p->detail_url(key, project) : QString();
}

} // namespace memex::client
