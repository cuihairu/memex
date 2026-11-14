// R27-3 QtNetwork 传输实现：回调收尾、reply 随 nam 父子链自动回收，
// 回调以实现对象为上下文（对象亡则不回调，无悬垂）。
#include "engine/task/task_http.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace memex::client {

void QtNetworkTaskHttp::request(
    const QString& method, const QUrl& url,
    const QList<QPair<QByteArray, QByteArray>>& headers,
    const QByteArray& body, const HttpFn& done) {
  QNetworkRequest req(url);
  for (const auto& h : headers) req.setRawHeader(h.first, h.second);
  QNetworkReply* reply = [&] {
    const QByteArray m = method.toUtf8();
    if (m == "GET") return nam_->get(req);
    if (m == "POST") return nam_->post(req, body);
    if (m == "PUT") return nam_->put(req, body);
    return nam_->sendCustomRequest(req, m, body); // PATCH 等
  }();
  // 回调以 this（实现对象）为上下文：对象亡→断连不回调（头文件契约
  // 「回调只在发出方存续期内到达」；上下文若挂 reply 上＝实现对象亡后
  // 回包照达，done 捕获的属主指针悬垂）。reply 清理独立连线（上下文
  // reply 自身）：实现对象先亡时 finished 照常 deleteLater，不漏回收。
  QObject::connect(reply, &QNetworkReply::finished, this,
                   [reply, done] {
                     const int status =
                         reply->attribute(
                             QNetworkRequest::HttpStatusCodeAttribute)
                             .toInt();
                     const QString err = reply->error() == QNetworkReply::NoError
                                             ? QString()
                                             : reply->errorString();
                     const QByteArray body_out = reply->readAll();
                     done(status, body_out, err);
                   });
  QObject::connect(reply, &QNetworkReply::finished, reply,
                   &QNetworkReply::deleteLater);
}

} // namespace memex::client
