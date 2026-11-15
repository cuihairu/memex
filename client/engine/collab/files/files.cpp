// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {


FilesClient::FilesClient(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

FilesClient::~FilesClient() = default;

void FilesClient::fail(const QString& op, int status, const QString& error) {
  emit request_failed(op, status, error);
}

void FilesClient::send_json(const QString& op, const QString& method,
                            const QString& path, const QJsonObject& body,
                            const JsonHandler& handler) {
  if (token_.isEmpty() && path != QStringLiteral("/files/session")) {
    fail(op, 0, QStringLiteral("未登录"));
    handler(false, 0, {}, QStringLiteral("未登录"));
    return;
  }
  QNetworkRequest req(QUrl(base_url() + path));
  req.setTransferTimeout(kMetaTimeoutMs);
  if (!token_.isEmpty()) {
    req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  }
  QNetworkReply* rep = nullptr;
  if (method == QStringLiteral("GET")) {
    rep = nam_->get(req);
  } else if (method == QStringLiteral("DELETE")) {
    rep = nam_->deleteResource(req);
  } else {
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    rep = nam_->post(req, body.isEmpty()
                              ? QByteArray()
                              : QJsonDocument(body).toJson(
                                    QJsonDocument::Compact));
  }
  connect(rep, &QNetworkReply::finished, this, [this, rep, op, handler] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QJsonObject obj;
    const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
    if (doc.isObject()) obj = doc.object();
    const bool http_ok = rep->error() == QNetworkReply::NoError && status >= 200 &&
                         status < 300;
    if (!http_ok) {
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(op, status, msg);
      handler(false, status, obj, msg);
      return;
    }
    handler(true, status, obj, {});
  });
}

void FilesClient::login(const QString& host, quint16 files_port,
                        const QString& account, const QString& password) {
  host_ = host;
  port_ = files_port;
  account_ = account;
  token_.clear();
  QJsonObject body{{QStringLiteral("account"), account},
                   {QStringLiteral("password"), password}};
  send_json(QStringLiteral("session.login"), QStringLiteral("POST"),
            QStringLiteral("/files/session"), body,
            [this](bool ok, int, const QJsonObject& resp,
                   const QString& error) {
              const QString token =
                  resp.value(QStringLiteral("token")).toString();
              if (ok && !token.isEmpty()) {
                token_ = token;
                emit logged_in();
              } else {
                emit login_failed(error.isEmpty()
                                      ? QStringLiteral("登录失败")
                                      : error);
              }
            });
}

void FilesClient::logout() {
  // 平台-2：服务端登出（落 logout_reason＋令牌即刻失效）尽力而为——
  // 网络失败也照常本地清（服务端 12h TTL 兜底）
  if (!token_.isEmpty()) {
    send_json(QStringLiteral("session.logout"), QStringLiteral("POST"),
              QStringLiteral("/files/logout"), {},
              [](bool, int, const QJsonObject&, const QString&) {});
  }
  token_.clear();
}

void FilesClient::create_memo(const QString& content) {
  send_json(QStringLiteral("memo.create"), QStringLiteral("POST"),
            QStringLiteral("/files/memo"),
            {{QStringLiteral("content"), content}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit memo_created(
                    static_cast<qint64>(resp.value(QStringLiteral("id"))
                                            .toDouble()));
              }
            });
}

void FilesClient::update_memo(qint64 id, const QString& content) {
  send_json(QStringLiteral("memo.update"), QStringLiteral("POST"),
            QStringLiteral("/files/memo"),
            {{QStringLiteral("id"), static_cast<double>(id)},
             {QStringLiteral("content"), content}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit memo_updated(id);
            });
}

void FilesClient::delete_memo(qint64 id) {
  send_json(QStringLiteral("memo.delete"), QStringLiteral("DELETE"),
            QStringLiteral("/files/memo?id=") + QString::number(id), {},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit memo_deleted(id);
            });
}

void FilesClient::list_memos() {
  send_json(QStringLiteral("memo.list"), QStringLiteral("GET"),
            QStringLiteral("/files/memo"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit memo_listed(
                    resp.value(QStringLiteral("memos")).toArray());
              }
            });
}

void FilesClient::fetch_memo(qint64 id) {
  send_json(QStringLiteral("memo.fetch"), QStringLiteral("GET"),
            QStringLiteral("/files/memo?id=") + QString::number(id), {},
            [this, id](bool ok, int, const QJsonObject& resp,
                       const QString&) {
              if (!ok) return;
              emit memo_fetched(
                  id, resp.value(QStringLiteral("memo"))
                          .toObject()
                          .value(QStringLiteral("content"))
                          .toString());
            });
}

void FilesClient::list_group_memos(quint64 gid, const QString& q) {
  QString path = QStringLiteral("/files/group-memo/list?gid=%1").arg(gid);
  if (!q.isEmpty()) {
    // 主动百分号编码（中文走 %XX；服务端 query_param 已解 %XX）
    path += QStringLiteral("&q=") +
            QString::fromLatin1(QUrl::toPercentEncoding(q));
  }
  send_json(QStringLiteral("group-memo.list"), QStringLiteral("GET"), path, {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_memo_listed(
                  resp.value(QStringLiteral("memos")).toArray(),
                  resp.value(QStringLiteral("open_edit")).toBool());
            });
}

void FilesClient::save_group_memo(quint64 gid, const QString& title,
                                  const QString& content, qint64 id) {
  QJsonObject body{{QStringLiteral("gid"), static_cast<double>(gid)},
                   {QStringLiteral("title"), title},
                   {QStringLiteral("content"), content}};
  if (id > 0) body.insert(QStringLiteral("id"), static_cast<double>(id));
  send_json(QStringLiteral("group-memo.save"), QStringLiteral("POST"),
            QStringLiteral("/files/group-memo/save"), body,
            [this, id](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_memo_saved(
                  id > 0 ? id
                         : static_cast<qint64>(resp.value(QStringLiteral("id"))
                                                   .toDouble()));
            });
}

void FilesClient::delete_group_memo(quint64 gid, qint64 id) {
  send_json(QStringLiteral("group-memo.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/group-memo/delete"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit group_memo_deleted(id);
            });
}

void FilesClient::group_memo_history(qint64 id) {
  send_json(QStringLiteral("group-memo.history"), QStringLiteral("GET"),
            QStringLiteral("/files/group-memo/history?id=") +
                QString::number(id),
            {}, [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_memo_history_fetched(
                  resp.value(QStringLiteral("revisions")).toArray());
            });
}

void FilesClient::rollback_group_memo(qint64 id, qint64 revision_id) {
  send_json(QStringLiteral("group-memo.rollback"), QStringLiteral("POST"),
            QStringLiteral("/files/group-memo/rollback"),
            {{QStringLiteral("id"), static_cast<double>(id)},
             {QStringLiteral("revision_id"), static_cast<double>(revision_id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit group_memo_rolled_back(id);
            });
}

void FilesClient::set_group_memo_open_edit(quint64 gid, bool open) {
  send_json(QStringLiteral("group-memo.open-edit"), QStringLiteral("POST"),
            QStringLiteral("/files/group-memo/open-edit"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("open"), open}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) emit group_memo_open_edit_set(
                            resp.value(QStringLiteral("open_edit")).toBool());
            });
}


} // namespace memex::client
