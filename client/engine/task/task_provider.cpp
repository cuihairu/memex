// R27-2 Provider SPI 实现：URL 模板/直通链接两个 L1 框架件＋注册表
// 内置预设。策略=解析不出即空串（调用方本地拒），不造半截链接。
#include "engine/task/task_provider.hpp"

#include <QtAlgorithms>

namespace memex::client {

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
}

TaskProviderRegistry::~TaskProviderRegistry() {
  qDeleteAll(items_);
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
