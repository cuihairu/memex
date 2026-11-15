// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {

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

void FilesClient::group_record_export(quint64 gid, qint64 from_ms,
                                      qint64 to_ms, bool with_ai,
                                      qint64 after_id, qint64 fetched,
                                      int page_size, int cap) {
  const QString url =
      QStringLiteral(
          "/files/group-record-export?gid=%1&from=%2&to=%3&with_ai=%4"
          "&page_size=%5&cap=%6&fetched=%7&after_id=%8")
          .arg(gid)
          .arg(from_ms)
          .arg(to_ms)
          .arg(with_ai ? 1 : 0)
          .arg(page_size)
          .arg(cap)
          .arg(fetched)
          .arg(after_id);
  send_json(QStringLiteral("group-record.export"), QStringLiteral("GET"), url,
            {},
            [this](bool ok, int, const QJsonObject& resp, const QString& err) {
              if (!ok) {
                emit group_record_export_failed(err);
                return;
              }
              emit group_record_export_page(resp);
            });
}


} // namespace memex::client
