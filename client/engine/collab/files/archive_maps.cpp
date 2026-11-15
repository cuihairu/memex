// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {

// —— 管理台归档检索（desktop-search）三面 ——

void FilesClient::audit_context(const QString& msg_id, int span) {
  QString path = QStringLiteral("/files/audit/context?msg_id=") +
                 QString::fromUtf8(QUrl::toPercentEncoding(msg_id)) +
                 QStringLiteral("&span=%1").arg(span);
  send_json(QStringLiteral("audit.context"), QStringLiteral("GET"), path, {},
            [this, msg_id](bool ok, int, const QJsonObject& resp,
                           const QString&) {
              if (ok) {
                emit audit_context_fetched(
                    msg_id, resp.value(QStringLiteral("messages")).toArray());
              }
            });
}

void FilesClient::audit_recall_event(const QString& msg_id) {
  const QString path = QStringLiteral("/files/audit/recall-event?msg_id=") +
                       QString::fromUtf8(QUrl::toPercentEncoding(msg_id));
  send_json(QStringLiteral("audit.recall-event"), QStringLiteral("GET"), path,
            {},
            [this, msg_id](bool ok, int, const QJsonObject& resp,
                           const QString&) {
              if (ok) {
                emit audit_recall_event_fetched(
                    msg_id, resp.value(QStringLiteral("exists")).toBool(),
                    resp.value(QStringLiteral("by_account")).toString(),
                    static_cast<qint64>(
                        resp.value(QStringLiteral("ts_ms")).toDouble()));
              }
            });
}

void FilesClient::audit_export(const QString& account, const QString& keyword,
                               qint64 since_ms, qint64 until_ms) {
  QJsonObject body;
  if (!account.isEmpty()) body.insert(QStringLiteral("account"), account);
  if (!keyword.isEmpty()) body.insert(QStringLiteral("keyword"), keyword);
  if (since_ms > 0) {
    body.insert(QStringLiteral("since_ms"), static_cast<double>(since_ms));
  }
  if (until_ms > 0) {
    body.insert(QStringLiteral("until_ms"), static_cast<double>(until_ms));
  }
  send_json(QStringLiteral("audit.export"), QStringLiteral("POST"),
            QStringLiteral("/files/audit/export"), body,
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit audit_exported(
                    resp.value(QStringLiteral("messages")).toArray(),
                    resp.value(QStringLiteral("operator")).toString(),
                    static_cast<qint64>(
                        resp.value(QStringLiteral("exported_ms")).toDouble()));
              }
            });
}

// —— 办公室位置图（二期）——

void FilesClient::fetch_office_map(const QString& floor) {
  QString path = QStringLiteral("/files/office-map");
  if (!floor.isEmpty()) {
    path += QStringLiteral("?floor=") +
            QString::fromUtf8(QUrl::toPercentEncoding(floor));
  }
  send_json(QStringLiteral("office.map"), QStringLiteral("GET"), path, {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) emit office_map_fetched(resp);
            });
}

void FilesClient::save_office_seat(const QString& floor, const QString& label,
                                   double x, double y) {
  send_json(QStringLiteral("office.seat"), QStringLiteral("POST"),
            QStringLiteral("/files/office-map/seat"),
            {{QStringLiteral("floor"), floor},
             {QStringLiteral("label"), label},
             {QStringLiteral("x"), x},
             {QStringLiteral("y"), y}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit office_seat_saved(static_cast<qint64>(
                    resp.value(QStringLiteral("id")).toDouble()));
              }
            });
}

void FilesClient::delete_office_seat(qint64 id) {
  send_json(QStringLiteral("office.seat.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/office-map/seat"),
            {{QStringLiteral("remove"), true},
             {QStringLiteral("id"), static_cast<double>(id)}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit office_seat_deleted();
            });
}

void FilesClient::bind_office_seat(qint64 id, const QString& account) {
  QJsonObject body{{QStringLiteral("id"), static_cast<double>(id)}};
  if (!account.isEmpty()) body.insert(QStringLiteral("account"), account);
  send_json(QStringLiteral("office.bind"), QStringLiteral("POST"),
            QStringLiteral("/files/office-map/bind"), body,
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit office_seat_bound();
            });
}

// —— 远程协助（二期）——

void FilesClient::assist_request(const QString& target,
                                 const QStringList& perms) {
  QJsonArray arr;
  for (const auto& p : perms) arr.append(p);
  send_json(QStringLiteral("assist.request"), QStringLiteral("POST"),
            QStringLiteral("/files/assist/request"),
            {{QStringLiteral("target"), target},
             {QStringLiteral("perms"), arr}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit assist_requested(
                    resp.value(QStringLiteral("id")).toString());
              }
            });
}

void FilesClient::assist_respond(const QString& id, bool approve,
                                 const QStringList& perms) {
  QJsonObject body{{QStringLiteral("id"), id},
                   {QStringLiteral("approve"), approve}};
  if (approve && !perms.isEmpty()) {
    QJsonArray arr;
    for (const auto& p : perms) arr.append(p);
    body.insert(QStringLiteral("perms"), arr);
  }
  send_json(QStringLiteral("assist.respond"), QStringLiteral("POST"),
            QStringLiteral("/files/assist/respond"), body,
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit assist_responded(id);
            });
}

void FilesClient::assist_start(const QString& id) {
  send_json(QStringLiteral("assist.start"), QStringLiteral("POST"),
            QStringLiteral("/files/assist/start"),
            {{QStringLiteral("id"), id}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit assist_started(id);
            });
}

void FilesClient::assist_end(const QString& id, const QString& reason) {
  QJsonObject body{{QStringLiteral("id"), id}};
  if (!reason.isEmpty()) body.insert(QStringLiteral("reason"), reason);
  send_json(QStringLiteral("assist.end"), QStringLiteral("POST"),
            QStringLiteral("/files/assist/end"), body,
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit assist_ended(id);
            });
}

void FilesClient::fetch_assist_sessions() {
  send_json(QStringLiteral("assist.sessions"), QStringLiteral("GET"),
            QStringLiteral("/files/assist/sessions"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit assist_sessions_listed(
                    resp.value(QStringLiteral("sessions")).toArray());
              }
            });
}

void FilesClient::fetch_assist_audit(const QString& id) {
  send_json(QStringLiteral("assist.audit"), QStringLiteral("POST"),
            QStringLiteral("/files/assist/audit"),
            {{QStringLiteral("id"), id}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit assist_audit_listed(
                    resp.value(QStringLiteral("audits")).toArray());
              }
            });
}

void FilesClient::push_assist_frame(const QString& id, qint64 seq,
                                    const QString& jpeg_b64) {
  send_json(QStringLiteral("assist.frame"), QStringLiteral("POST"),
            QStringLiteral("/files/assist/frame"),
            {{QStringLiteral("id"), id},
             {QStringLiteral("seq"), static_cast<double>(seq)},
             {QStringLiteral("jpeg_b64"), jpeg_b64}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit assist_frame_pushed(static_cast<qint64>(
                    resp.value(QStringLiteral("seq")).toDouble()));
              }
            });
}

void FilesClient::pull_assist_frame(const QString& id) {
  send_json(QStringLiteral("assist.frame"), QStringLiteral("GET"),
            QStringLiteral("/files/assist/frame?id=") + id, {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit assist_frame_pulled(
                    static_cast<qint64>(
                        resp.value(QStringLiteral("seq")).toDouble()),
                    resp.value(QStringLiteral("jpeg_b64")).toString());
              }
            });
}

void FilesClient::send_assist_input(const QString& id, const QString& kind,
                                    double x, double y, const QString& key) {
  QJsonObject body{{QStringLiteral("id"), id},
                   {QStringLiteral("kind"), kind}};
  if (kind != QStringLiteral("key")) {
    body.insert(QStringLiteral("x"), x);
    body.insert(QStringLiteral("y"), y);
  } else {
    body.insert(QStringLiteral("key"), key);
  }
  send_json(QStringLiteral("assist.input"), QStringLiteral("POST"),
            QStringLiteral("/files/assist/input"), body,
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit assist_input_sent();
            });
}

void FilesClient::fetch_assist_input(const QString& id) {
  send_json(QStringLiteral("assist.input"), QStringLiteral("GET"),
            QStringLiteral("/files/assist/input?id=") + id, {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit assist_inputs_listed(
                    resp.value(QStringLiteral("events")).toArray());
              }
            });
}


} // namespace memex::client
