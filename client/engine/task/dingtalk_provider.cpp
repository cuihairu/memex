// R27-3 钉钉待办 provider 实现。API 形态（open.dingtalk.com，2026-10-08
// 核实端点）：
//   POST /v1.0/oauth2/accessToken {appKey,appSecret}
//        → {accessToken, expireIn}（头无鉴权；默认 7200s）
//   GET  /v1.0/todo/users/{unionId}/tasks?maxResults=50&status=RUNNING
//        头 x-acs-dingtalk-access-token
//        → {totalCount, tasks:[{id,subject,description,dueTime,
//           detailUrl:{url},status}]}（detailUrl 可缺）
//   POST /v1.0/todo/users/{unionId}/tasks {subject,description,
//        detailUrl:{url}} → {id}
//   PUT  /v1.0/todo/users/{unionId}/tasks/{taskId} {isDone:true}
// 字段名以文档为准（未真连验证，假传输锁我方行为）。
#include "engine/task/dingtalk_provider.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

namespace memex::client {

QString DingtalkTodoProvider::detail_url(const QString& /*key*/,
                                         const QString& /*project*/) const {
  return QString(); // L1 未声明：无稳定 web 详情页形态可从键构造
}

QUrl DingtalkTodoProvider::api_url(const QString& path) const {
  return QUrl(QStringLiteral("https://api.dingtalk.com") + path);
}

void DingtalkTodoProvider::with_token(
    const std::function<void(const QString&, const QString&)>& next) const {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (!cached_token_.isEmpty() && now < token_expire_ms_) {
    next(cached_token_, QString());
    return;
  }
  const QJsonObject body{{QStringLiteral("appKey"), app_key_},
                         {QStringLiteral("appSecret"), app_secret_}};
  http_->request(
      QStringLiteral("POST"), api_url(QStringLiteral("/v1.0/oauth2/accessToken")),
      {}, QJsonDocument(body).toJson(QJsonDocument::Compact),
      [this, next](int status, const QByteArray& body, const QString& err) {
        if (status != 200) {
          next(QString(), err.isEmpty()
                              ? QStringLiteral("钉钉取令牌失败（HTTP %1）")
                                    .arg(status)
                              : err);
          return;
        }
        const auto o = QJsonDocument::fromJson(body).object();
        const QString token =
            o.value(QStringLiteral("accessToken")).toString();
        const qint64 expire =
            static_cast<qint64>(o.value(QStringLiteral("expireIn")).toDouble());
        if (token.isEmpty()) {
          next(QString(), QStringLiteral("钉钉响应缺 accessToken"));
          return;
        }
        cached_token_ = token;
        token_expire_ms_ =
            QDateTime::currentMSecsSinceEpoch() +
            (expire > 60 ? expire - 60 : expire) * 1000; // 提前 60s 续
        next(token, QString());
      });
}

void DingtalkTodoProvider::list(const TaskListFn& done) const {
  with_token([this, done](const QString& token, const QString& err) {
    if (!err.isEmpty()) {
      done(false, {}, err);
      return;
    }
    QUrl url = api_url(QStringLiteral("/v1.0/todo/users/") + union_id_ +
                       QStringLiteral("/tasks"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
    url.setQuery(q);
    http_->request(
        QStringLiteral("GET"), url,
        {qMakePair(QByteArray("x-acs-dingtalk-access-token"),
                   token.toUtf8())},
        {},
        [this, done](int status, const QByteArray& body, const QString& err) {
          if (status != 200) {
            done(false, {},
                 err.isEmpty() ? QStringLiteral("钉钉拉取失败（HTTP %1）")
                                     .arg(status)
                               : err);
            return;
          }
          const auto doc = QJsonDocument::fromJson(body);
          if (!doc.isObject() || !doc.object().contains(QStringLiteral("tasks"))) {
            done(false, {}, QStringLiteral("钉钉响应不是合法清单"));
            return;
          }
          QVector<ExternalTask> out;
          const auto items = doc.object().value(QStringLiteral("tasks")).toArray();
          for (const auto& v : items) {
            const auto o = v.toObject();
            ExternalTask t;
            t.provider_id = id();
            t.key = o.value(QStringLiteral("id")).toString();
            t.title = o.value(QStringLiteral("subject")).toString();
            const auto du = o.value(QStringLiteral("detailUrl")).toObject();
            t.detail_url = du.value(QStringLiteral("url")).toString();
            t.done = o.value(QStringLiteral("status")).toString() ==
                     QStringLiteral("FINISHED");
            if (!t.key.isEmpty()) out.append(t); // detailUrl 可缺（行仍在）
          }
          done(true, out, QString());
        });
  });
}

void DingtalkTodoProvider::create(const QString& title, const QString& note,
                                  const TaskWriteFn& done) {
  with_token([this, title, note, done](const QString& token,
                                       const QString& err) {
    if (!err.isEmpty()) {
      done(false, QString(), err);
      return;
    }
    QJsonObject body{{QStringLiteral("subject"), title}};
    if (!note.isEmpty()) body.insert(QStringLiteral("description"), note);
    http_->request(
        QStringLiteral("POST"),
        api_url(QStringLiteral("/v1.0/todo/users/") + union_id_ +
                QStringLiteral("/tasks")),
        {qMakePair(QByteArray("x-acs-dingtalk-access-token"),
                   token.toUtf8())},
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        [done](int status, const QByteArray& body, const QString& err) {
          if (status != 200) {
            done(false, QString(),
                 err.isEmpty() ? QStringLiteral("钉钉建待办失败（HTTP %1）")
                                     .arg(status)
                               : err);
            return;
          }
          const QString id = QJsonDocument::fromJson(body)
                                 .object()
                                 .value(QStringLiteral("id"))
                                 .toString();
          done(!id.isEmpty(), id, id.isEmpty()
                                      ? QStringLiteral("钉钉响应缺待办 id")
                                      : QString());
        });
  });
}

void DingtalkTodoProvider::complete(const QString& key,
                                    const TaskWriteFn& done) {
  const QString k = key.trimmed();
  if (k.isEmpty()) {
    done(false, key, QStringLiteral("待办 id 不合法"));
    return;
  }
  with_token([this, k, done](const QString& token, const QString& err) {
    if (!err.isEmpty()) {
      done(false, k, err);
      return;
    }
    const QJsonObject body{{QStringLiteral("isDone"), true}};
    http_->request(
        QStringLiteral("PUT"),
        api_url(QStringLiteral("/v1.0/todo/users/") + union_id_ +
                QStringLiteral("/tasks/") + k),
        {qMakePair(QByteArray("x-acs-dingtalk-access-token"),
                   token.toUtf8())},
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        [done, k](int status, const QByteArray&, const QString& err) {
          if (status != 204 && status != 200) {
            done(false, k,
                 err.isEmpty() ? QStringLiteral("钉钉完成待办失败（HTTP %1）")
                                     .arg(status)
                               : err);
            return;
          }
          done(true, k, QString());
        });
  });
}

} // namespace memex::client
