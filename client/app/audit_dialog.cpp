// 二期·会话审计窗口（实现）。判权全在服务端（az audit:read——持
// auditor 有效角色；无角色检索 403 且被拒尝试服务端留痕可对账）——
// 客户端只提交条件与展示结果。
#include "audit_dialog.hpp"

#include <QCheckBox>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QTime>
#include <QSettings>
#include <QVBoxLayout>

#include "engine/collab/files_client.hpp"

namespace memex::client {

AuditDialog::AuditDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("会话审计"));
  resize(680, 560);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）——检索与查阅日志均须 auditor 有效角色"));
    btn_search_->setEnabled(true);
    btn_reads_->setEnabled(true);
    refresh_reads();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::audit_searched, this,
          [this](const QJsonArray& messages) {
            populate_results(messages);
            refresh_reads(); // 每次检索落一条查阅日志——顺带对齐台账
          });
  connect(client_, &FilesClient::audit_reads_listed, this,
          &AuditDialog::populate_reads);
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

void AuditDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // 连接区（与文件助手同构：地址/端口/账号/口令）
  auto* conn = new QHBoxLayout;
  auto* host = new QLineEdit(this);
  host->setPlaceholderText(QStringLiteral("服务器地址"));
  auto* port = new QLineEdit(this);
  port->setPlaceholderText(QStringLiteral("文件端口"));
  port->setMaximumWidth(90);
  auto* account_box = new QLineEdit(this);
  account_box->setPlaceholderText(QStringLiteral("账号"));
  account_box->setMaximumWidth(110);
  auto* password = new QLineEdit(this);
  password->setPlaceholderText(QStringLiteral("口令"));
  password->setEchoMode(QLineEdit::Password);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn->addWidget(host);
  conn->addWidget(port);
  conn->addWidget(account_box);
  conn->addWidget(password);
  conn->addWidget(btn_connect_);
  layout->addLayout(conn);
  connect(btn_connect_, &QPushButton::clicked, this,
          [this, host, port, account_box, password] {
            connect_to(host->text(), port->text().toUShort(),
                       account_box->text(), password->text());
          });

  // 检索区：账号＋关键词＋时间窗（勾选生效）
  auto* form1 = new QHBoxLayout;
  form1->addWidget(new QLabel(QStringLiteral("账号"), this));
  account_ = new QLineEdit(this);
  account_->setPlaceholderText(QStringLiteral("可空=全部"));
  form1->addWidget(account_, 1);
  form1->addWidget(new QLabel(QStringLiteral("关键词"), this));
  keyword_ = new QLineEdit(this);
  keyword_->setPlaceholderText(QStringLiteral("可空=全部"));
  form1->addWidget(keyword_, 1);
  layout->addLayout(form1);
  auto* form2 = new QHBoxLayout;
  use_window_ = new QCheckBox(QStringLiteral("时间窗"), this);
  since_ = new QDateEdit(QDate::currentDate().addDays(-7), this);
  since_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  since_->setCalendarPopup(true);
  until_ = new QDateEdit(QDate::currentDate(), this);
  until_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  until_->setCalendarPopup(true);
  form2->addWidget(use_window_);
  form2->addWidget(since_);
  form2->addWidget(new QLabel(QStringLiteral("至"), this));
  form2->addWidget(until_);
  form2->addStretch(1);
  layout->addLayout(form2);

  // 检索结果
  list_ = new QListWidget(this);
  list_->setAlternatingRowColors(true);
  layout->addWidget(list_, 2);

  // 操作区
  auto* ops = new QHBoxLayout;
  btn_search_ = new QPushButton(QStringLiteral("检索"), this);
  btn_search_->setEnabled(false);
  btn_reads_ = new QPushButton(QStringLiteral("刷新查阅日志"), this);
  btn_reads_->setEnabled(false);
  ops->addWidget(btn_search_);
  ops->addWidget(btn_reads_);
  ops->addStretch(1);
  layout->addLayout(ops);

  // 查阅日志台账
  layout->addWidget(new QLabel(QStringLiteral("查阅日志（每次检索/被拒都留痕）"), this));
  reads_list_ = new QListWidget(this);
  reads_list_->setAlternatingRowColors(true);
  layout->addWidget(reads_list_, 1);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_search_, &QPushButton::clicked, this, [this] {
    qint64 since_ms = 0;
    qint64 until_ms = 0;
    if (use_window_->isChecked()) {
      since_ms = QDateTime(since_->date(), QTime(0, 0))
                     .toMSecsSinceEpoch();
      until_ms = QDateTime(until_->date(), QTime(23, 59, 59))
                     .toMSecsSinceEpoch();
    }
    run_search(account_->text().trimmed(), keyword_->text().trimmed(),
               since_ms, until_ms);
  });
  connect(btn_reads_, &QPushButton::clicked, this,
          [this] { refresh_reads(); });
}

void AuditDialog::connect_to(const QString& host, quint16 files_port,
                             const QString& acc, const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"), QString::number(files_port));
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool AuditDialog::is_connected() const { return client_->is_logged_in(); }

bool AuditDialog::run_search(const QString& account, const QString& keyword,
                             qint64 since_ms, qint64 until_ms) {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->audit_search(account, keyword, since_ms, until_ms);
  return true;
}

void AuditDialog::refresh_reads() {
  if (!is_connected()) return;
  client_->fetch_audit_reads();
}

void AuditDialog::populate_results(const QJsonArray& messages) {
  list_->clear();
  for (const auto& v : messages) {
    const auto m = v.toObject();
    const QString when =
        QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(m.value(QStringLiteral("ts_ms")).toDouble()))
            .toString(QStringLiteral("MM-dd HH:mm"));
    auto* item = new QListWidgetItem(QString(), list_);
    item->setData(Qt::UserRole,
                  m.value(QStringLiteral("msg_id")).toString());
    item->setText(QStringLiteral("%1  %2→%3  %4%5  %6")
                      .arg(when, m.value(QStringLiteral("from")).toString(),
                           m.value(QStringLiteral("to")).toString(),
                           m.value(QStringLiteral("type")).toString(),
                           m.value(QStringLiteral("recalled")).toBool()
                               ? QStringLiteral("（已撤回）")
                               : QString(),
                           m.value(QStringLiteral("text")).toString()));
    if (m.value(QStringLiteral("recalled")).toBool()) {
      item->setForeground(Qt::gray); // 撤回只置标记原文保留（留痕纪律）
    }
  }
}

void AuditDialog::populate_reads(const QJsonArray& reads) {
  reads_list_->clear();
  for (const auto& v : reads) {
    const auto r = v.toObject();
    const QString when =
        QDateTime::fromMSecsSinceEpoch(
            static_cast<qint64>(r.value(QStringLiteral("ts_ms")).toDouble()))
            .toString(QStringLiteral("MM-dd HH:mm:ss"));
    auto* item = new QListWidgetItem(QString(), reads_list_);
    item->setText(QStringLiteral("%1  %2  %3  %4%5")
                      .arg(when, r.value(QStringLiteral("op_account")).toString(),
                           r.value(QStringLiteral("action")).toString(),
                           r.value(QStringLiteral("filters")).toString(),
                           r.value(QStringLiteral("action")).toString() ==
                                   QStringLiteral("audit.message.search")
                               ? QStringLiteral("  命中%1条")
                                     .arg(r.value(QStringLiteral(
                                              "result_count"))
                                              .toInt())
                               : QString()));
  }
}

void AuditDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（恢复用空参刷新路径重设）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : list_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString AuditDialog::status_text() const { return status_->text(); }

int AuditDialog::result_count() const { return list_->count(); }

int AuditDialog::read_count() const { return reads_list_->count(); }

} // namespace memex::client
