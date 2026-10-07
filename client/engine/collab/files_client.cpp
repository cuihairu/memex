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

// —— R25-2 群 CI/CD 工具 ——

void FilesClient::set_tool_actions(quint64 gid, const QString& tool,
                                   const QStringList& actions) {
  QJsonArray arr;
  for (const QString& a : actions) arr.append(a);
  send_json(QStringLiteral("group-tools.config"), QStringLiteral("POST"),
            QStringLiteral("/files/group-tools/config"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("tool"), tool},
             {QStringLiteral("actions"), arr}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit tool_actions_set();
            });
}

void FilesClient::ci_set_pipeline(quint64 gid, const QString& name,
                                  const QString& description, bool remove) {
  QJsonObject body;
  body.insert(QStringLiteral("gid"), static_cast<double>(gid));
  body.insert(QStringLiteral("name"), name);
  if (remove) {
    body.insert(QStringLiteral("op"), QStringLiteral("delete"));
  } else {
    body.insert(QStringLiteral("description"), description);
  }
  send_json(QStringLiteral("group-ci.pipeline"), QStringLiteral("POST"),
            QStringLiteral("/files/group-ci/pipeline"), body,
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit ci_pipeline_set();
            });
}

void FilesClient::ci_list(quint64 gid) {
  send_json(QStringLiteral("group-ci.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-ci/list?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit ci_listed(
                  resp.value(QStringLiteral("pipelines")).toArray());
            });
}

void FilesClient::ci_trigger(quint64 gid, const QString& pipeline,
                             const QJsonObject& params) {
  send_json(QStringLiteral("group-ci.trigger"), QStringLiteral("POST"),
            QStringLiteral("/files/group-ci/trigger"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("pipeline"), pipeline},
             {QStringLiteral("params"), params}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit ci_triggered(
                  static_cast<qint64>(
                      resp.value(QStringLiteral("run_id")).toDouble()),
                  resp.value(QStringLiteral("status")).toString());
            });
}

void FilesClient::ci_runs(quint64 gid, const QString& pipeline) {
  QString path = QStringLiteral("/files/group-ci/runs?gid=%1").arg(gid);
  if (!pipeline.isEmpty()) {
    path += QStringLiteral("&pipeline=") +
            QString::fromUtf8(QUrl::toPercentEncoding(pipeline));
  }
  send_json(QStringLiteral("group-ci.runs"), QStringLiteral("GET"), path, {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit ci_runs_listed(resp.value(QStringLiteral("runs")).toArray());
            });
}

// —— R25-3 打包工具＋配置导出 ——

void FilesClient::pack_build(quint64 gid, const QString& name,
                             const QString& version, const QString& note) {
  send_json(QStringLiteral("group-pack.build"), QStringLiteral("POST"),
            QStringLiteral("/files/group-pack/build"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("name"), name},
             {QStringLiteral("version"), version},
             {QStringLiteral("note"), note}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit pack_built();
            });
}

void FilesClient::pack_list(quint64 gid) {
  send_json(QStringLiteral("group-pack.list"), QStringLiteral("GET"),
            QStringLiteral("/files/group-pack/list?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit pack_listed(
                  resp.value(QStringLiteral("artifacts")).toArray());
            });
}

void FilesClient::pack_delete(quint64 gid, const QString& name,
                              const QString& version) {
  send_json(QStringLiteral("group-pack.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/group-pack/delete"),
            {{QStringLiteral("gid"), static_cast<double>(gid)},
             {QStringLiteral("name"), name},
             {QStringLiteral("version"), version}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit pack_deleted();
            });
}

void FilesClient::group_export(quint64 gid) {
  send_json(QStringLiteral("group.export"), QStringLiteral("GET"),
            QStringLiteral("/files/group-export?gid=%1").arg(gid), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (!ok) return;
              emit group_exported(
                  resp.value(QStringLiteral("snapshot")).toObject());
            });
}

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
