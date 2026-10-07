// R27-3 GitHub Issues provider 实现。REST 形态：
//   GET  /repos/{o}/{r}/issues?state=open&per_page=50 → [{number,title,
//        html_url,state,pull_request?...}]（含 PR 须滤）
//   POST /repos/{o}/{r}/issues {title,body} → 201 {number,html_url}
//   PATCH /repos/{o}/{r}/issues/{n} {state:"closed"} → 200
// 认证：Authorization: Bearer <PAT>＋Accept: application/vnd.github+json
// ＋X-GitHub-Api-Version；User-Agent 必带（GitHub 默认拒无 UA 客户端）。
#include "engine/task/github_provider.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

namespace memex::client {
namespace {
QList<QPair<QByteArray, QByteArray>> gh_headers(const QString& token) {
  QList<QPair<QByteArray, QByteArray>> h;
  if (!token.isEmpty()) {
    h.append(qMakePair(QByteArray("Authorization"),
                       QByteArray("Bearer ") + token.toUtf8()));
  }
  h.append(qMakePair(QByteArray("Accept"),
                     QByteArray("application/vnd.github+json")));
  h.append(qMakePair(QByteArray("X-GitHub-Api-Version"),
                     QByteArray("2022-11-28")));
  h.append(qMakePair(QByteArray("User-Agent"), QByteArray("memex-client")));
  return h;
}
} // namespace

QString GitHubIssuesProvider::detail_url(const QString& key,
                                         const QString& project) const {
  const QString k = key.trimmed();
  bool numeric = false;
  k.toLongLong(&numeric);
  if (!numeric || k.isEmpty()) return QString(); // 键须为 issue 号
  const QString r = project.trimmed().isEmpty() ? repo_ : project.trimmed();
  if (r.isEmpty() || !r.contains(QLatin1Char('/'))) return QString();
  return QStringLiteral("https://github.com/%1/issues/%2").arg(r, k);
}

QUrl GitHubIssuesProvider::api_url(const QString& path) const {
  return QUrl(QStringLiteral("https://api.github.com/repos/") + repo_ + path);
}

void GitHubIssuesProvider::list(const TaskListFn& done) const {
  QUrl url = api_url(QStringLiteral("/issues"));
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("state"), QStringLiteral("open"));
  q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("50"));
  url.setQuery(q);
  http_->request(
      QStringLiteral("GET"), url, gh_headers(token_), {},
      [this, done](int status, const QByteArray& body, const QString& err) {
        if (status != 200) {
          done(false, {}, err.isEmpty()
                            ? QStringLiteral("GitHub 拉取失败（HTTP %1）")
                                  .arg(status)
                            : err);
          return;
        }
        QJsonParseError pe;
        const auto doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
          done(false, {}, QStringLiteral("GitHub 响应不是合法清单"));
          return;
        }
        QVector<ExternalTask> out;
        for (const auto& v : doc.array()) {
          const auto o = v.toObject();
          if (o.contains(QStringLiteral("pull_request"))) continue; // 滤 PR
          ExternalTask t;
          t.provider_id = id();
          t.key = QString::number(
              static_cast<qint64>(o.value(QStringLiteral("number")).toDouble()));
          t.project = repo_;
          t.title = o.value(QStringLiteral("title")).toString();
          t.detail_url = o.value(QStringLiteral("html_url")).toString();
          t.done = o.value(QStringLiteral("state")).toString() ==
                   QStringLiteral("closed");
          if (!t.key.isEmpty() && !t.detail_url.isEmpty()) out.append(t);
        }
        done(true, out, QString());
      });
}

void GitHubIssuesProvider::create(const QString& title, const QString& note,
                                  const TaskWriteFn& done) {
  QJsonObject body{{QStringLiteral("title"), title}};
  if (!note.isEmpty()) body.insert(QStringLiteral("body"), note);
  http_->request(
      QStringLiteral("POST"), api_url(QStringLiteral("/issues")),
      gh_headers(token_),
      QJsonDocument(body).toJson(QJsonDocument::Compact),
      [done](int status, const QByteArray& body, const QString& err) {
        if (status != 201) {
          done(false, QString(),
               err.isEmpty() ? QStringLiteral("GitHub 建任务失败（HTTP %1）")
                                   .arg(status)
                             : err);
          return;
        }
        const auto o = QJsonDocument::fromJson(body).object();
        const qint64 n =
            static_cast<qint64>(o.value(QStringLiteral("number")).toDouble());
        done(n > 0, n > 0 ? QString::number(n) : QString(),
             n > 0 ? QString() : QStringLiteral("GitHub 响应缺 issue 号"));
      });
}

void GitHubIssuesProvider::complete(const QString& key,
                                    const TaskWriteFn& done) {
  bool numeric = false;
  key.trimmed().toLongLong(&numeric);
  if (!numeric) {
    done(false, key, QStringLiteral("issue 号不合法"));
    return;
  }
  QJsonObject body{{QStringLiteral("state"), QStringLiteral("closed")}};
  http_->request(
      QStringLiteral("PATCH"),
      api_url(QStringLiteral("/issues/") + key.trimmed()),
      gh_headers(token_),
      QJsonDocument(body).toJson(QJsonDocument::Compact),
      [done, key](int status, const QByteArray&, const QString& err) {
        if (status != 200) {
          done(false, key,
               err.isEmpty() ? QStringLiteral("GitHub 关单失败（HTTP %1）")
                                   .arg(status)
                             : err);
          return;
        }
        done(true, key, QString());
      });
}

} // namespace memex::client
