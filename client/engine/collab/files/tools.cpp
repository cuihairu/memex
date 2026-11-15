// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {

// —— R25-4 工具凭据面（value 只此一次出门；服务端回包不回显）——

void FilesClient::tool_cred_set(quint64 gid, const QString& tool,
                                const QString& value) {
  send_json(QStringLiteral("group-credential.set"), QStringLiteral("POST"),
            QStringLiteral("/files/group-tools/credential"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("tool"), tool},
             {QStringLiteral("value"), value}},
            [this, gid, tool](bool ok, int, const QJsonObject&,
                              const QString&) {
              if (ok) emit tool_cred_saved(static_cast<qint64>(gid), tool);
            });
}

void FilesClient::tool_cred_delete(quint64 gid, const QString& tool) {
  send_json(QStringLiteral("group-credential.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/group-tools/credential"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("tool"), tool},
             {QStringLiteral("op"), QStringLiteral("delete")}},
            [this, gid, tool](bool ok, int, const QJsonObject&,
                              const QString&) {
              if (ok) emit tool_cred_removed(static_cast<qint64>(gid), tool);
            });
}

void FilesClient::tool_cred_list(quint64 gid) {
  send_json(QStringLiteral("group-credential.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-tools/credentials?gid=%1").arg(gid),
            {}, [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit tool_credentials_listed(
                  resp.value(QStringLiteral("credentials")).toArray());
            });
}

// —— R26-2 群服务器面（令牌只在登记回包出现一次） ——

void FilesClient::server_enroll(quint64 gid, const QString& name,
                                const QString& host) {
  send_json(QStringLiteral("group-server.enroll"), QStringLiteral("POST"),
            QStringLiteral("/files/group-servers/enroll"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("name"), name},
             {QStringLiteral("host"), host}},
            [this, gid](bool ok, int, const QJsonObject& resp,
                        const QString&) {
              if (!ok) return;
              emit server_enrolled(
                  static_cast<qint64>(gid),
                  static_cast<qint64>(
                      resp.value(QStringLiteral("id")).toDouble()),
                  resp.value(QStringLiteral("token")).toString());
            });
}

void FilesClient::server_list(quint64 gid) {
  send_json(QStringLiteral("group-server.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-servers/list?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit servers_listed(
                  resp.value(QStringLiteral("servers")).toArray());
            });
}

// —— R26-3 远程会话（短票只在此层瞬时经手，不落任何日志） ——

void FilesClient::session_request(quint64 gid, qint64 server_id,
                                  const QString& protocol) {
  send_json(QStringLiteral("group-session.request"), QStringLiteral("POST"),
            QStringLiteral("/files/group-servers/session/request"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("server_id"), static_cast<double>(server_id)},
             {QStringLiteral("protocol"), protocol}},
            [this, gid](bool ok, int, const QJsonObject& resp,
                        const QString&) {
              if (!ok) return;
              emit session_requested(
                  static_cast<qint64>(gid),
                  static_cast<qint64>(
                      resp.value(QStringLiteral("session_id")).toDouble()),
                  resp.value(QStringLiteral("ticket")).toString(),
                  static_cast<qint64>(
                      resp.value(QStringLiteral("expires_ms")).toDouble()));
            });
}

void FilesClient::session_redeem(const QString& ticket) {
  send_json(QStringLiteral("group-session.redeem"), QStringLiteral("POST"),
            QStringLiteral("/files/group-servers/session/redeem"),
            {{QStringLiteral("ticket"), ticket}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit session_redeemed(
                  static_cast<qint64>(
                      resp.value(QStringLiteral("session_id")).toDouble()),
                  static_cast<qint64>(
                      resp.value(QStringLiteral("gid")).toDouble()),
                  resp.value(QStringLiteral("server_name")).toString(),
                  resp.value(QStringLiteral("host")).toString());
            });
}

void FilesClient::session_close(quint64 gid, qint64 session_id) {
  send_json(QStringLiteral("group-session.close"), QStringLiteral("POST"),
            QStringLiteral("/files/group-servers/session/close"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("session_id"), static_cast<double>(session_id)}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit session_closed(static_cast<qint64>(
                  resp.value(QStringLiteral("session_id")).toDouble()));
            });
}

void FilesClient::session_list(quint64 gid) {
  send_json(QStringLiteral("group-session.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-servers/sessions?gid=%1").arg(gid),
            {}, [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit sessions_listed(
                  resp.value(QStringLiteral("sessions")).toArray());
            });
}

// —— R26-4 服务器凭据面（明文只在此层瞬时经手，不落任何日志/框） ——

void FilesClient::server_cred_set(quint64 gid, qint64 server_id,
                                  const QString& value) {
  send_json(QStringLiteral("group-server-cred.set"), QStringLiteral("POST"),
            QStringLiteral("/files/group-servers/credential"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("server_id"), static_cast<double>(server_id)},
             {QStringLiteral("value"), value}},
            [this, gid, server_id](bool ok, int, const QJsonObject&,
                                   const QString&) {
              if (ok) {
                emit server_cred_saved(static_cast<qint64>(gid), server_id);
              }
            });
}

void FilesClient::server_cred_delete(quint64 gid, qint64 server_id) {
  send_json(QStringLiteral("group-server-cred.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/group-servers/credential"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("server_id"), static_cast<double>(server_id)},
             {QStringLiteral("op"), QStringLiteral("delete")}},
            [this, gid, server_id](bool ok, int, const QJsonObject&,
                                   const QString&) {
              if (ok) {
                emit server_cred_removed(static_cast<qint64>(gid), server_id);
              }
            });
}

void FilesClient::list_inbox() {
  send_json(QStringLiteral("inbox.list"), QStringLiteral("GET"),
            QStringLiteral("/files/list?target=inbox"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit inbox_listed(
                    resp.value(QStringLiteral("items")).toArray());
              }
            });
}

void FilesClient::upload_inbox(const QString& file_path) {
  if (token_.isEmpty()) {
    fail(QStringLiteral("inbox.upload"), 0, QStringLiteral("未登录"));
    return;
  }
  QFile f(file_path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(QStringLiteral("inbox.upload"), 0,
         QStringLiteral("文件打开失败"));
    return;
  }
  const QByteArray bytes = f.readAll();
  const QString name = QFileInfo(file_path).fileName();
  QNetworkRequest req(
      QUrl(base_url() + QStringLiteral("/files/upload?target=inbox")));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setRawHeader("X-File-Name", name.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/octet-stream"));
  QNetworkReply* rep = nam_->post(req, bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
    if (rep->error() != QNetworkReply::NoError || status < 200 ||
        status >= 300) {
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(QStringLiteral("inbox.upload"), status, msg);
      return;
    }
    emit upload_finished(
        static_cast<qint64>(obj.value(QStringLiteral("id")).toDouble()),
        obj.value(QStringLiteral("second_transfer")).toBool());
  });
}

void FilesClient::delete_file(qint64 file_id) {
  send_json(QStringLiteral("file.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/manage/delete?id=") + QString::number(file_id),
            {}, [this, file_id](bool ok, int, const QJsonObject&,
                                const QString&) {
              if (ok) emit file_deleted(file_id);
            });
}


} // namespace memex::client
