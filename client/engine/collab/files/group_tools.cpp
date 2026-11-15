// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {

// —— 群工具三件（二期·投票/接龙/群任务）——

void FilesClient::create_poll(quint64 gid, const QString& topic,
                              const QStringList& options,
                              qint64 deadline_ms, bool anonymous,
                              bool multi) {
  QJsonArray arr;
  for (const QString& o : options) arr.append(o);
  send_json(QStringLiteral("group-poll.create"), QStringLiteral("POST"),
            QStringLiteral("/files/group-polls"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("topic"), topic},
             {QStringLiteral("options"), arr},
             {QStringLiteral("deadline_ms"), static_cast<double>(deadline_ms)},
             {QStringLiteral("anonymous"), anonymous},
             {QStringLiteral("multi"), multi}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit poll_created(static_cast<qint64>(
                    resp.value(QStringLiteral("poll_id")).toDouble()));
              }
            });
}

void FilesClient::list_polls(quint64 gid) {
  send_json(QStringLiteral("group-poll.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-polls?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit polls_listed(
                    resp.value(QStringLiteral("polls")).toArray());
              }
            });
}

void FilesClient::vote_poll(quint64 gid, qint64 poll_id, int choice) {
  send_json(QStringLiteral("group-poll.vote"), QStringLiteral("POST"),
            QStringLiteral("/files/group-polls/vote"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("poll_id"), static_cast<double>(poll_id)},
             {QStringLiteral("choice"), choice}},
            [this, poll_id](bool ok, int, const QJsonObject&,
                            const QString&) {
              if (ok) emit poll_voted(poll_id);
            });
}

void FilesClient::close_poll(quint64 gid, qint64 poll_id) {
  send_json(QStringLiteral("group-poll.close"), QStringLiteral("POST"),
            QStringLiteral("/files/group-polls/close"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("poll_id"), static_cast<double>(poll_id)}},
            [this, poll_id](bool ok, int, const QJsonObject&,
                            const QString&) {
              if (ok) emit poll_closed(poll_id);
            });
}

void FilesClient::create_chain(quint64 gid, const QString& title,
                               const QString& format_hint) {
  QJsonObject body{{QStringLiteral("gid"), static_cast<double>(gid)},
                   {QStringLiteral("title"), title}};
  if (!format_hint.isEmpty()) {
    body.insert(QStringLiteral("format_hint"), format_hint);
  }
  send_json(QStringLiteral("group-chain.create"), QStringLiteral("POST"),
            QStringLiteral("/files/group-chains"), body,
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit chain_created(static_cast<qint64>(
                    resp.value(QStringLiteral("chain_id")).toDouble()));
              }
            });
}

void FilesClient::list_chains(quint64 gid) {
  send_json(QStringLiteral("group-chain.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-chains?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit chains_listed(
                    resp.value(QStringLiteral("chains")).toArray());
              }
            });
}

void FilesClient::join_chain(quint64 gid, qint64 chain_id,
                             const QString& content) {
  send_json(QStringLiteral("group-chain.join"), QStringLiteral("POST"),
            QStringLiteral("/files/group-chains/join"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("chain_id"), static_cast<double>(chain_id)},
             {QStringLiteral("content"), content}},
            [this, chain_id](bool ok, int, const QJsonObject&,
                             const QString&) {
              if (ok) emit chain_joined(chain_id);
            });
}

void FilesClient::close_chain(quint64 gid, qint64 chain_id) {
  send_json(QStringLiteral("group-chain.close"), QStringLiteral("POST"),
            QStringLiteral("/files/group-chains/close"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("chain_id"), static_cast<double>(chain_id)}},
            [this, chain_id](bool ok, int, const QJsonObject&,
                             const QString&) {
              if (ok) emit chain_closed(chain_id);
            });
}

void FilesClient::create_group_task(quint64 gid, const QString& title,
                                    const QString& assignee, qint64 due_ms) {
  QJsonObject body{{QStringLiteral("gid"), static_cast<double>(gid)},
                   {QStringLiteral("title"), title}};
  if (!assignee.isEmpty()) {
    body.insert(QStringLiteral("assignee"), assignee);
  }
  if (due_ms > 0) {
    body.insert(QStringLiteral("due_ms"), static_cast<double>(due_ms));
  }
  send_json(QStringLiteral("group-task.create"), QStringLiteral("POST"),
            QStringLiteral("/files/group-tasks"), body,
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit group_task_created(static_cast<qint64>(
                    resp.value(QStringLiteral("task_id")).toDouble()));
              }
            });
}

void FilesClient::list_group_tasks(quint64 gid) {
  send_json(QStringLiteral("group-task.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-tasks?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit group_tasks_listed(
                    resp.value(QStringLiteral("tasks")).toArray());
              }
            });
}

void FilesClient::claim_group_task(quint64 gid, qint64 task_id) {
  send_json(QStringLiteral("group-task.claim"), QStringLiteral("POST"),
            QStringLiteral("/files/group-tasks/claim"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("task_id"), static_cast<double>(task_id)}},
            [this, task_id](bool ok, int, const QJsonObject&,
                            const QString&) {
              if (ok) emit group_task_claimed(task_id);
            });
}

void FilesClient::done_group_task(quint64 gid, qint64 task_id) {
  send_json(QStringLiteral("group-task.done"), QStringLiteral("POST"),
            QStringLiteral("/files/group-tasks/done"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("task_id"), static_cast<double>(task_id)}},
            [this, task_id](bool ok, int, const QJsonObject&,
                            const QString&) {
              if (ok) emit group_task_done(task_id);
            });
}

void FilesClient::download_file(qint64 file_id, const QString& file_name,
                                const QString& save_dir) {
  if (token_.isEmpty()) {
    fail(QStringLiteral("file.download"), 0, QStringLiteral("未登录"));
    return;
  }
  QDir dir(save_dir);
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    fail(QStringLiteral("file.download"), 0,
         QStringLiteral("保存目录创建失败"));
    return;
  }
  // 重名加序号（-1/-2…），不覆盖已有文件
  QString path = dir.filePath(file_name);
  const QFileInfo fi(file_name);
  for (int n = 1; QFileInfo::exists(path); ++n) {
    path = dir.filePath(fi.completeBaseName() + QStringLiteral("-%1").arg(n) +
                        (fi.suffix().isEmpty() ? QString()
                                               : QStringLiteral(".") +
                                                     fi.suffix()));
  }
  QNetworkRequest req(QUrl(base_url() +
                           QStringLiteral("/files/download?id=") +
                           QString::number(file_id)));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  QNetworkReply* rep = nam_->get(req);
  auto out = std::make_shared<QSaveFile>(path);
  if (!out->open(QIODevice::WriteOnly)) {
    rep->abort();
    rep->deleteLater();
    fail(QStringLiteral("file.download"), 0,
         QStringLiteral("本地文件创建失败"));
    return;
  }
  // 流式落盘（不整包进内存）；非 200（JSON 错误体）不写入
  connect(rep, &QNetworkReply::readyRead, this, [rep, out] {
    if (rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
        200) {
      return;
    }
    out->write(rep->readAll());
  });
  connect(rep, &QNetworkReply::finished, this, [this, rep, out, path] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (rep->error() != QNetworkReply::NoError || status != 200) {
      out->cancelWriting();
      QString msg;
      const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
      if (doc.isObject()) msg = doc.object().value("error").toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(QStringLiteral("file.download"), status, msg);
      return;
    }
    out->write(rep->readAll());
    if (!out->commit()) {
      fail(QStringLiteral("file.download"), 0,
           QStringLiteral("本地文件写入失败"));
      return;
    }
    emit download_finished(path);
  });
}


} // namespace memex::client
