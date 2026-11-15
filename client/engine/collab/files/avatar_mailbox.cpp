// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {

// —— 用户头像（需求批⑫）——

void FilesClient::avatar_upload(int size, const QByteArray& png_bytes) {
  const QString op = QStringLiteral("avatar.upload");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  // size 进 query（字节已按档编码 PNG；magic 与上限校验在服务端）
  QNetworkRequest req(QUrl(base_url() +
                           QStringLiteral("/files/avatar/upload?size=") +
                           QString::number(size)));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("image/png"));
  QNetworkReply* rep = nam_->post(req, png_bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep, op, size] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
    if (rep->error() != QNetworkReply::NoError || status < 200 ||
        status >= 300) {
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(op, status, msg);
      return;
    }
    emit avatar_uploaded(size, static_cast<qint64>(
                                   obj.value(QStringLiteral("ver"))
                                       .toDouble()));
  });
}

void FilesClient::avatar_download(const QString& account, int size) {
  const QString op = QStringLiteral("avatar.download");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QUrl url(base_url() + QStringLiteral("/files/avatar/download"));
  QUrlQuery q;
  if (!account.isEmpty()) {
    q.addQueryItem(QStringLiteral("account"), account);
  }
  q.addQueryItem(QStringLiteral("size"), QString::number(size));
  url.setQuery(q);
  QNetworkRequest req(url);
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  QNetworkReply* rep = nam_->get(req);
  // 回包 200=原始字节（非 JSON）；404=未设置头像（错误体才是 JSON）
  connect(rep, &QNetworkReply::finished, this,
          [this, rep, op, account, size] {
            rep->deleteLater();
            const int status =
                rep->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const QByteArray bytes = rep->readAll();
            if (rep->error() != QNetworkReply::NoError || status != 200) {
              const QJsonDocument doc = QJsonDocument::fromJson(bytes);
              const QJsonObject obj =
                  doc.isObject() ? doc.object() : QJsonObject{};
              QString msg = obj.value(QStringLiteral("error")).toString();
              if (msg.isEmpty()) msg = rep->errorString();
              fail(op, status, msg);
              return;
            }
            emit avatar_fetched(account, size, bytes);
          });
}

void FilesClient::avatar_delete() {
  send_json(QStringLiteral("avatar.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/avatar/delete"), {},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (!ok) return;
              emit avatar_deleted();
            });
}

// —— 邮箱·统一收件箱（mailbox.md：视图+已读状态，非新通道；只拉不推）——

void FilesClient::mailbox_list(const QString& type, bool unread,
                               const QString& q) {
  QJsonObject body;
  if (!type.isEmpty()) body.insert(QStringLiteral("type"), type);
  if (unread) body.insert(QStringLiteral("unread"), true);
  if (!q.isEmpty()) body.insert(QStringLiteral("q"), q);
  send_json(QStringLiteral("mailbox.list"), QStringLiteral("POST"),
            QStringLiteral("/files/mailbox/list"), body,
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit mailbox_listed(resp.value(QStringLiteral("items")).toArray(),
                                  resp.value(QStringLiteral("counts")).toObject());
            });
}

void FilesClient::mailbox_unread() {
  send_json(QStringLiteral("mailbox.unread"), QStringLiteral("POST"),
            QStringLiteral("/files/mailbox/unread"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit mailbox_unread_loaded(
                  resp.value(QStringLiteral("counts")).toObject());
            });
}

void FilesClient::mailbox_mark_read(qint64 id, bool all) {
  QJsonObject body;
  if (all) {
    body.insert(QStringLiteral("all"), true);
  } else {
    body.insert(QStringLiteral("id"), static_cast<double>(id));
  }
  send_json(QStringLiteral("mailbox.read"), QStringLiteral("POST"),
            QStringLiteral("/files/mailbox/read"), body,
            [this, id](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit mailbox_read_marked(
                  id, static_cast<int>(
                          resp.value(QStringLiteral("marked")).toDouble()));
            });
}

void FilesClient::mailbox_mark_handled(qint64 id) {
  send_json(QStringLiteral("mailbox.handle"), QStringLiteral("POST"),
            QStringLiteral("/files/mailbox/handle"),
            {{QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (!ok) return;
              emit mailbox_handled(id);
            });
}

void FilesClient::mailbox_confirm(qint64 id) {
  send_json(QStringLiteral("mailbox.confirm"), QStringLiteral("POST"),
            QStringLiteral("/files/mailbox/confirm"),
            {{QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (!ok) return;
              emit mailbox_confirmed(id);
            });
}

void FilesClient::mailbox_confirmations(qint64 id) {
  send_json(QStringLiteral("mailbox.confirmations"), QStringLiteral("POST"),
            QStringLiteral("/files/mailbox/confirmations"),
            {{QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit mailbox_confirmations_listed(
                  id, resp.value(QStringLiteral("rows")).toArray());
            });
}

} // namespace memex::client
