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
#include <QUrlQuery>

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

// —— 个人任务清单（R27-1）——

void FilesClient::create_task(const QString& title, const QString& note,
                              qint64 due_ms, const QString& assignee,
                              const QString& provider,
                              const QString& ext_key) {
  QJsonObject body{{QStringLiteral("title"), title}};
  if (!note.isEmpty()) body.insert(QStringLiteral("note"), note);
  if (due_ms > 0) body.insert(QStringLiteral("due_ms"), static_cast<double>(due_ms));
  if (!assignee.isEmpty()) body.insert(QStringLiteral("assignee"), assignee);
  if (!provider.isEmpty()) body.insert(QStringLiteral("provider"), provider);
  if (!ext_key.isEmpty()) body.insert(QStringLiteral("ext_key"), ext_key);
  send_json(QStringLiteral("task.create"), QStringLiteral("POST"),
            QStringLiteral("/files/tasks"), body,
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit task_created(static_cast<qint64>(
                    resp.value(QStringLiteral("id")).toDouble()));
              }
            });
}

void FilesClient::list_tasks() {
  send_json(QStringLiteral("task.list"), QStringLiteral("GET"),
            QStringLiteral("/files/tasks"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit tasks_listed(
                    resp.value(QStringLiteral("tasks")).toArray(),
                    resp.value(QStringLiteral("assigned_by_me")).toArray());
              }
            });
}

void FilesClient::set_task_done(qint64 id, bool done) {
  send_json(QStringLiteral("task.done"), QStringLiteral("POST"),
            QStringLiteral("/files/tasks/done"),
            {{QStringLiteral("id"), static_cast<double>(id)},
             {QStringLiteral("done"), done}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit task_done_set(id);
            });
}

void FilesClient::mark_task_reminded(qint64 id) {
  send_json(QStringLiteral("task.reminded"), QStringLiteral("POST"),
            QStringLiteral("/files/tasks/reminded"),
            {{QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit task_reminded(id);
            });
}

void FilesClient::delete_task(qint64 id) {
  send_json(QStringLiteral("task.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/tasks/delete"),
            {{QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit task_deleted(id);
            });
}

// —— 审批（二期·请假起步）——

void FilesClient::create_approval(const QString& type, const QString& from,
                                  const QString& to, const QString& reason) {
  QJsonObject body{{QStringLiteral("type"), type}};
  if (!from.isEmpty()) body.insert(QStringLiteral("from"), from);
  if (!to.isEmpty()) body.insert(QStringLiteral("to"), to);
  if (!reason.isEmpty()) body.insert(QStringLiteral("reason"), reason);
  send_json(QStringLiteral("approval.create"), QStringLiteral("POST"),
            QStringLiteral("/files/approvals"), body,
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit approval_created(static_cast<qint64>(
                    resp.value(QStringLiteral("id")).toDouble()));
              }
            });
}

void FilesClient::list_approvals() {
  send_json(QStringLiteral("approval.list"), QStringLiteral("GET"),
            QStringLiteral("/files/approvals"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit approvals_listed(
                    resp.value(QStringLiteral("mine")).toArray(),
                    resp.value(QStringLiteral("pending")).toArray());
              }
            });
}

void FilesClient::decide_approval(qint64 id, bool approved,
                                  const QString& note) {
  QJsonObject body{{QStringLiteral("id"), static_cast<double>(id)},
                   {QStringLiteral("approved"), approved}};
  if (!note.isEmpty()) body.insert(QStringLiteral("note"), note);
  send_json(QStringLiteral("approval.decide"), QStringLiteral("POST"),
            QStringLiteral("/files/approvals/decide"), body,
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit approval_decided(id);
            });
}

void FilesClient::withdraw_approval(qint64 id) {
  send_json(QStringLiteral("approval.withdraw"), QStringLiteral("POST"),
            QStringLiteral("/files/approvals/withdraw"),
            {{QStringLiteral("id"), static_cast<double>(id)}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit approval_withdrawn(id);
            });
}

// —— 日报周报（二期）——

void FilesClient::save_report(const QString& date, const QString& content) {
  send_json(QStringLiteral("report.save"), QStringLiteral("POST"),
            QStringLiteral("/files/reports"),
            {{QStringLiteral("date"), date},
             {QStringLiteral("content"), content}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit report_saved(static_cast<qint64>(
                    resp.value(QStringLiteral("id")).toDouble()));
              }
            });
}

void FilesClient::list_reports() {
  send_json(QStringLiteral("report.list"), QStringLiteral("GET"),
            QStringLiteral("/files/reports"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit reports_listed(
                    resp.value(QStringLiteral("reports")).toArray());
              }
            });
}

void FilesClient::fetch_team_reports() {
  send_json(QStringLiteral("report.team"), QStringLiteral("GET"),
            QStringLiteral("/files/reports/team"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit team_reports_listed(
                    resp.value(QStringLiteral("team")).toArray());
              }
            });
}

// —— 会话审计（二期）——

void FilesClient::audit_search(const QString& account, const QString& keyword,
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
  send_json(QStringLiteral("audit.search"), QStringLiteral("POST"),
            QStringLiteral("/files/audit/search"), body,
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit audit_searched(
                    resp.value(QStringLiteral("messages")).toArray());
              }
            });
}

void FilesClient::fetch_audit_reads() {
  send_json(QStringLiteral("audit.reads"), QStringLiteral("GET"),
            QStringLiteral("/files/audit/reads"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit audit_reads_listed(
                    resp.value(QStringLiteral("reads")).toArray());
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

// —— 品牌物料（二期）——

void FilesClient::fetch_branding() {
  send_json(QStringLiteral("branding.fetch"), QStringLiteral("GET"),
            QStringLiteral("/files/branding"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) emit branding_fetched(resp);
            });
}

void FilesClient::save_branding(const QString& company_name,
                                const QString& accent,
                                const QString& slogan) {
  // 三件齐发（服务端口径：显式空串=清空、缺省不动——设置页表单语义
  // 「留空=清空该字段」与服务端显式空串对齐）
  send_json(QStringLiteral("branding.set"), QStringLiteral("POST"),
            QStringLiteral("/files/branding"),
            {{QStringLiteral("company_name"), company_name},
             {QStringLiteral("accent"), accent},
             {QStringLiteral("slogan"), slogan}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit branding_saved(static_cast<qint64>(
                    resp.value(QStringLiteral("version")).toDouble()));
              }
            });
}

void FilesClient::clear_brand_logo() {
  send_json(QStringLiteral("branding.clear_logo"), QStringLiteral("POST"),
            QStringLiteral("/files/branding"),
            {{QStringLiteral("clear_logo"), true}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit brand_logo_cleared();
            });
}

void FilesClient::clear_brand_splash() {
  send_json(QStringLiteral("branding.clear_splash"), QStringLiteral("POST"),
            QStringLiteral("/files/branding"),
            {{QStringLiteral("clear_splash"), true}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit brand_splash_cleared();
            });
}

// 素材上传（PNG 原始字节 octet-stream；send_json 只走 JSON 面，照
// upload_inbox 直挂 NAM；类型/大小/尺寸校验在服务端，语义化 413/415
// 文案经统一失败通道回状态行）
void FilesClient::upload_brand_logo(const QString& file_path) {
  const QString op = QStringLiteral("branding.logo");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QFile f(file_path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(op, 0, QStringLiteral("文件打开失败"));
    return;
  }
  const QByteArray bytes = f.readAll();
  QNetworkRequest req(
      QUrl(base_url() + QStringLiteral("/files/branding/logo")));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("image/png"));
  QNetworkReply* rep = nam_->post(req, bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep, op] {
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
    emit brand_logo_uploaded(static_cast<qint64>(
        obj.value(QStringLiteral("version")).toDouble()));
  });
}

void FilesClient::upload_brand_splash(const QString& file_path) {
  const QString op = QStringLiteral("branding.splash");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QFile f(file_path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(op, 0, QStringLiteral("文件打开失败"));
    return;
  }
  const QByteArray bytes = f.readAll();
  QNetworkRequest req(
      QUrl(base_url() + QStringLiteral("/files/branding/splash")));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("image/png"));
  QNetworkReply* rep = nam_->post(req, bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep, op] {
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
    emit brand_splash_uploaded(static_cast<qint64>(
        obj.value(QStringLiteral("version")).toDouble()));
  });
}

// —— 表情包素材（需求批②）——

void FilesClient::emoji_upload(const QString& file_path, const QString& name) {
  const QString op = QStringLiteral("emoji.upload");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QFile f(file_path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(op, 0, QStringLiteral("文件打开失败"));
    return;
  }
  const QString suffix = QFileInfo(file_path).suffix().toLower();
  QString content_type = QStringLiteral("image/png");
  if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg")) {
    content_type = QStringLiteral("image/jpeg");
  } else if (suffix == QStringLiteral("gif")) {
    content_type = QStringLiteral("image/gif");
  }
  const QByteArray bytes = f.readAll();
  // name 进 query（magic 与 1MiB 上限校验在服务端）
  QNetworkRequest req(QUrl(base_url() +
                           QStringLiteral("/files/emoji/upload?name=") +
                           QString::fromUtf8(
                               QUrl::toPercentEncoding(name.toUtf8()))));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader, content_type);
  QNetworkReply* rep = nam_->post(req, bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep, op] {
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
    emit emoji_uploaded(static_cast<qint64>(
        obj.value(QStringLiteral("id")).toDouble()));
  });
}

void FilesClient::emoji_list() {
  send_json(QStringLiteral("emoji.list"), QStringLiteral("GET"),
            QStringLiteral("/files/emoji/list"), {},
            [this](bool ok, int status, const QJsonObject& body,
                   const QString& error) {
              if (!ok) return;
              emit emoji_listed(
                  body.value(QStringLiteral("assets")).toArray());
            });
}

void FilesClient::emoji_download(qint64 id, const QString& name,
                                 const QString& save_dir) {
  const QString op = QStringLiteral("emoji.download");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QDir dir(save_dir);
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    fail(op, 0, QStringLiteral("保存目录创建失败"));
    return;
  }
  // 重名加序号（-1/-2…），不覆盖已有文件
  QString path = dir.filePath(name);
  const QFileInfo fi(name);
  for (int n = 1; QFileInfo::exists(path); ++n) {
    path = dir.filePath(fi.completeBaseName() + QStringLiteral("-%1").arg(n) +
                        (fi.suffix().isEmpty()
                             ? QString()
                             : QStringLiteral(".") + fi.suffix()));
  }
  QNetworkRequest req(QUrl(base_url() +
                           QStringLiteral("/files/emoji/download?id=") +
                           QString::number(id)));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  QNetworkReply* rep = nam_->get(req);
  auto out = std::make_shared<QSaveFile>(path);
  if (!out->open(QIODevice::WriteOnly)) {
    rep->abort();
    rep->deleteLater();
    fail(op, 0, QStringLiteral("本地文件创建失败"));
    return;
  }
  connect(rep, &QNetworkReply::readyRead, this, [rep, out] {
    if (rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
        200) {
      return; // 非 200（JSON 错误体）不写入
    }
    out->write(rep->readAll());
  });
  connect(rep, &QNetworkReply::finished, this, [this, rep, out, op, path] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (rep->error() != QNetworkReply::NoError || status != 200) {
      out->cancelWriting();
      const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
      const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(op, status, msg);
      return;
    }
    if (!out->commit()) {
      fail(op, 0, QStringLiteral("本地写入失败"));
      return;
    }
    emit emoji_downloaded(path);
  });
}

void FilesClient::emoji_delete(qint64 id) {
  QJsonObject body;
  body[QStringLiteral("id")] = id;
  send_json(QStringLiteral("emoji.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/emoji/delete"), body,
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (!ok) return;
              emit emoji_deleted(id);
            });
}

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

} // namespace memex::client
