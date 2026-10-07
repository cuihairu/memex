// R27-3 飞书任务 provider 实现。API 形态（open.feishu.cn，2026-10-08 口径）：
//   POST /open-apis/auth/v3/tenant_access_token/internal
//        {app_id,app_secret} → {code:0, tenant_access_token, expire}
//   POST /open-apis/task/v2/tasks {summary,description}
//        头 Authorization: Bearer <tenant_access_token>
//        → {code:0, data:{task:{guid, summary, url}}}
//   PATCH /open-apis/task/v2/tasks/{guid} {is_completed:true}
// 信封 code!=0=业务失败（msg 透传）。字段名以文档为准（未真连验证，
// 假传输锁我方行为）。
#include "engine/task/feishu_provider.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

namespace memex::client {

QString FeishuTaskProvider::detail_url(const QString& /*key*/,
                                       const QString& /*project*/) const {
  return QString(); // L1 未声明：详情链接走 create 回带的 task.url 字段
}

QUrl FeishuTaskProvider::api_url(const QString& path) const {
  return QUrl(QStringLiteral("https://open.feishu.cn") + path);
}

void FeishuTaskProvider::with_token(
    const std::function<void(const QString&, const QString&)>& next) const {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (!cached_token_.isEmpty() && now < token_expire_ms_) {
    next(cached_token_, QString());
    return;
  }
  const QJsonObject body{{QStringLiteral("app_id"), app_id_},
                         {QStringLiteral("app_secret"), app_secret_}};
  http_->request(
      QStringLiteral("POST"),
      api_url(QStringLiteral(
          "/open-apis/auth/v3/tenant_access_token/internal")),
      {}, QJsonDocument(body).toJson(QJsonDocument::Compact),
      [this, next](int status, const QByteArray& body, const QString& err) {
        if (status != 200) {
          next(QString(), err.isEmpty()
                              ? QStringLiteral("飞书取令牌失败（HTTP %1）")
                                    .arg(status)
                              : err);
          return;
        }
        const auto o = QJsonDocument::fromJson(body).object();
        if (o.value(QStringLiteral("code")).toInt() != 0) {
          next(QString(),
               QStringLiteral("飞书取令牌被拒（%1）")
                   .arg(o.value(QStringLiteral("msg")).toString()));
          return;
        }
        const QString token =
            o.value(QStringLiteral("tenant_access_token")).toString();
        if (token.isEmpty()) {
          next(QString(), QStringLiteral("飞书响应缺 tenant_access_token"));
          return;
        }
        cached_token_ = token;
        const qint64 expire =
            static_cast<qint64>(o.value(QStringLiteral("expire")).toDouble());
        token_expire_ms_ =
            QDateTime::currentMSecsSinceEpoch() +
            (expire > 60 ? expire - 60 : expire) * 1000; // 提前 60s 续
        next(token, QString());
      });
}

void FeishuTaskProvider::create(const QString& title, const QString& note,
                                const TaskWriteFn& done) {
  with_token([this, title, note, done](const QString& token,
                                       const QString& err) {
    if (!err.isEmpty()) {
      done(false, QString(), err);
      return;
    }
    QJsonObject body{{QStringLiteral("summary"), title}};
    if (!note.isEmpty()) body.insert(QStringLiteral("description"), note);
    http_->request(
        QStringLiteral("POST"), api_url(QStringLiteral("/open-apis/task/v2/tasks")),
        {qMakePair(QByteArray("Authorization"),
                   QByteArray("Bearer ") + token.toUtf8())},
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        [done](int status, const QByteArray& body, const QString& err) {
          if (status != 200) {
            done(false, QString(),
                 err.isEmpty() ? QStringLiteral("飞书建任务失败（HTTP %1）")
                                     .arg(status)
                               : err);
            return;
          }
          const auto o = QJsonDocument::fromJson(body).object();
          if (o.value(QStringLiteral("code")).toInt() != 0) {
            done(false, QString(),
                 QStringLiteral("飞书建任务被拒（%1）")
                     .arg(o.value(QStringLiteral("msg")).toString()));
            return;
          }
          const auto task = o.value(QStringLiteral("data"))
                                .toObject()
                                .value(QStringLiteral("task"))
                                .toObject();
          const QString guid = task.value(QStringLiteral("guid")).toString();
          done(!guid.isEmpty(), guid,
               guid.isEmpty() ? QStringLiteral("飞书响应缺任务 guid")
                              : QString());
        });
  });
}

void FeishuTaskProvider::complete(const QString& key,
                                  const TaskWriteFn& done) {
  const QString k = key.trimmed();
  if (k.isEmpty()) {
    done(false, key, QStringLiteral("任务 guid 不合法"));
    return;
  }
  with_token([this, k, done](const QString& token, const QString& err) {
    if (!err.isEmpty()) {
      done(false, k, err);
      return;
    }
    const QJsonObject body{{QStringLiteral("is_completed"), true}};
    http_->request(
        QStringLiteral("PATCH"),
        api_url(QStringLiteral("/open-apis/task/v2/tasks/") + k),
        {qMakePair(QByteArray("Authorization"),
                   QByteArray("Bearer ") + token.toUtf8())},
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        [done, k](int status, const QByteArray& body, const QString& err) {
          if (status != 200) {
            done(false, k,
                 err.isEmpty() ? QStringLiteral("飞书完成任务失败（HTTP %1）")
                                     .arg(status)
                               : err);
            return;
          }
          const auto o = QJsonDocument::fromJson(body).object();
          if (o.value(QStringLiteral("code")).toInt() != 0) {
            done(false, k,
                 QStringLiteral("飞书完成任务被拒（%1）")
                     .arg(o.value(QStringLiteral("msg")).toString()));
            return;
          }
          done(true, k, QString());
        });
  });
}

} // namespace memex::client
