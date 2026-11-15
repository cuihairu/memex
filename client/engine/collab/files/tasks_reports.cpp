// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {

// —— 个人任务清单（R27-1）——

void FilesClient::create_task(const QString& title, const QString& note,
                              qint64 due_ms, const QString& assignee,
                              const QString& provider,
                              const QString& ext_key, const QString& type,
                              qint64 start_ms) {
  QJsonObject body{{QStringLiteral("title"), title}};
  if (!note.isEmpty()) body.insert(QStringLiteral("note"), note);
  if (due_ms > 0) body.insert(QStringLiteral("due_ms"), static_cast<double>(due_ms));
  if (!assignee.isEmpty()) body.insert(QStringLiteral("assignee"), assignee);
  if (!provider.isEmpty()) body.insert(QStringLiteral("provider"), provider);
  if (!ext_key.isEmpty()) body.insert(QStringLiteral("ext_key"), ext_key);
  // R27-6：类型（空=服务端默认「任务」）与开始时间（0=未设）
  if (!type.isEmpty()) body.insert(QStringLiteral("type"), type);
  if (start_ms > 0) {
    body.insert(QStringLiteral("start_ms"), static_cast<double>(start_ms));
  }
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

void FilesClient::fetch_task_types() {
  send_json(QStringLiteral("task.types"), QStringLiteral("GET"),
            QStringLiteral("/files/task-types"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit task_types_loaded(
                    resp.value(QStringLiteral("types")).toArray(),
                    resp.value(QStringLiteral("default")).toString());
              }
            });
}

void FilesClient::fetch_team_tasks() {
  send_json(QStringLiteral("task.team"), QStringLiteral("GET"),
            QStringLiteral("/files/tasks/team"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit team_tasks_loaded(
                    resp.value(QStringLiteral("team")).toArray());
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

void FilesClient::set_task_status(qint64 id, const QString& status) {
  send_json(QStringLiteral("task.status"), QStringLiteral("POST"),
            QStringLiteral("/files/tasks/status"),
            {{QStringLiteral("id"), static_cast<double>(id)},
             {QStringLiteral("status"), status}},
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit task_status_set(id);
            });
}

void FilesClient::mark_task_reminded(qint64 id, const QString& kind) {
  QJsonObject body{{QStringLiteral("id"), static_cast<double>(id)}};
  if (!kind.isEmpty()) body.insert(QStringLiteral("kind"), kind);
  send_json(QStringLiteral("task.reminded"), QStringLiteral("POST"),
            QStringLiteral("/files/tasks/reminded"), body,
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


} // namespace memex::client
