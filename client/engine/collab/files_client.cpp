#include "files_client.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QUrl>

namespace memex::client {

namespace {
constexpr int kMetaTimeoutMs = 15000;              // 元数据面 15s
constexpr int kByteTimeoutMs = 10 * 60 * 1000;     // 字节面 512MiB 上限给足
} // namespace

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
  // 服务端令牌不主动吊销（内网 v1 无 revoke 面）：本地清即可，
  // 服务端 12h TTL 自然过期
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

// —— R24-3 群密码箱（全程密文：客户端只传 b64 盐/包裹块/条目密文）——

void FilesClient::group_vault_info(quint64 gid) {
  send_json(QStringLiteral("group-vault.info"), QStringLiteral("GET"),
            QStringLiteral("/files/group-vault/info?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_vault_info_fetched(resp);
            });
}

void FilesClient::init_group_vault(quint64 gid, const QString& kdf_salt,
                                   int kdf_iters,
                                   const QString& wrapped_dek) {
  send_json(QStringLiteral("group-vault.init"), QStringLiteral("POST"),
            QStringLiteral("/files/group-vault/init"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("kdf_salt"), kdf_salt},
             {QStringLiteral("kdf_iters"), kdf_iters},
             {QStringLiteral("wrapped_dek"), wrapped_dek}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit group_vault_initialized();
            });
}

void FilesClient::rekey_group_vault(quint64 gid, const QString& kdf_salt,
                                    int kdf_iters,
                                    const QString& wrapped_dek) {
  send_json(QStringLiteral("group-vault.rekey"), QStringLiteral("POST"),
            QStringLiteral("/files/group-vault/rekey"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("kdf_salt"), kdf_salt},
             {QStringLiteral("kdf_iters"), kdf_iters},
             {QStringLiteral("wrapped_dek"), wrapped_dek}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit group_vault_rekeyed();
            });
}

void FilesClient::list_group_vault_entries(quint64 gid) {
  send_json(QStringLiteral("group-vault.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-vault/list?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_vault_listed(
                  resp.value(QStringLiteral("entries")).toArray());
            });
}

void FilesClient::access_group_vault_entry(quint64 gid, qint64 id,
                                           const QString& action) {
  send_json(QStringLiteral("group-vault.access"), QStringLiteral("POST"),
            QStringLiteral("/files/group-vault/access"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("id"), static_cast<double>(id)},
             {QStringLiteral("action"), action}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_vault_accessed(resp);
            });
}

void FilesClient::save_group_vault_entry(quint64 gid, const QString& name,
                                         const QString& account_name,
                                         const QString& secret_ct,
                                         const QString& secret_nonce,
                                         qint64 id) {
  QJsonObject body{{QStringLiteral("gid"), static_cast<double>(gid)},
                   {QStringLiteral("name"), name},
                   {QStringLiteral("account_name"), account_name},
                   {QStringLiteral("secret_ct"), secret_ct},
                   {QStringLiteral("secret_nonce"), secret_nonce}};
  if (id > 0) body.insert(QStringLiteral("id"), static_cast<double>(id));
  send_json(QStringLiteral("group-vault.save"), QStringLiteral("POST"),
            QStringLiteral("/files/group-vault/save"), body,
            [this, id](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_vault_entry_saved(
                  id > 0 ? id
                         : static_cast<qint64>(resp.value(QStringLiteral("id"))
                                                   .toDouble()));
            });
}

void FilesClient::delete_group_vault_entry(quint64 gid, qint64 id) {
  send_json(QStringLiteral("group-vault.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/group-vault/delete"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit group_vault_entry_deleted(id);
            });
}

void FilesClient::set_group_vault_acl(quint64 gid,
                                      const QStringList& accounts) {
  QJsonArray arr;
  for (const QString& a : accounts) arr.append(a);
  send_json(QStringLiteral("group-vault.acl"), QStringLiteral("POST"),
            QStringLiteral("/files/group-vault/acl"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("accounts"), arr}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit group_vault_acl_set();
            });
}

void FilesClient::group_vault_audit(quint64 gid) {
  send_json(QStringLiteral("group-vault.audit"), QStringLiteral("GET"),
            QStringLiteral("/files/group-vault/audit?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_vault_audit_listed(
                  resp.value(QStringLiteral("rows")).toArray());
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
