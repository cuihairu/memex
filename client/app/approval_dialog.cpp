// 二期·审批窗口（实现）。服务端裁决一切判权（决定归直属上级/无上级
// org-admin、撤回归申请人且仅 pending、四态只许 pending 可决）——
// 客户端只提交与展示，不自造规则。
#include "approval_dialog.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "engine/collab/files_client.hpp"

namespace memex::client {
namespace {
// 与服务端白名单同源（客户端只用于下拉默认项，白名单裁决在服务端）
const QStringList kTypes{QStringLiteral("年假"), QStringLiteral("事假"),
                         QStringLiteral("病假"), QStringLiteral("调休")};

QString status_label(const QString& s) {
  if (s == QStringLiteral("approved")) return QStringLiteral("已批准");
  if (s == QStringLiteral("rejected")) return QStringLiteral("已拒绝");
  if (s == QStringLiteral("withdrawn")) return QStringLiteral("已撤回");
  return QStringLiteral("待决");
}
} // namespace

ApprovalDialog::ApprovalDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("审批"));
  resize(560, 480);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）"));
    btn_add_->setEnabled(true);
    refresh();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::approvals_listed, this,
          &ApprovalDialog::populate);
  connect(client_, &FilesClient::approval_created, this, [this](qint64) {
    reason_->clear();
    set_status(QStringLiteral("申请已提交"));
    refresh();
  });
  connect(client_, &FilesClient::approval_decided, this, [this](qint64) {
    set_status(QStringLiteral("已决定"));
    refresh();
  });
  connect(client_, &FilesClient::approval_withdrawn, this, [this](qint64) {
    set_status(QStringLiteral("申请已撤回"));
    refresh();
  });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

void ApprovalDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // 连接区（与文件助手同构：地址/端口/账号/口令）
  auto* conn = new QHBoxLayout;
  host_ = new QLineEdit(this);
  host_->setPlaceholderText(QStringLiteral("服务器地址"));
  port_ = new QLineEdit(this);
  port_->setPlaceholderText(QStringLiteral("文件端口"));
  port_->setMaximumWidth(90);
  account_box_ = new QLineEdit(this);
  account_box_->setPlaceholderText(QStringLiteral("账号"));
  account_box_->setMaximumWidth(110);
  password_ = new QLineEdit(this);
  password_->setPlaceholderText(QStringLiteral("口令"));
  password_->setEchoMode(QLineEdit::Password);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn->addWidget(host_);
  conn->addWidget(port_);
  conn->addWidget(account_box_);
  conn->addWidget(password_);
  conn->addWidget(btn_connect_);
  layout->addLayout(conn);

  // 列表：我的申请 + 待我决
  list_ = new QListWidget(this);
  list_->setAlternatingRowColors(true);
  layout->addWidget(list_, 1);

  // 发起区：类型＋起止＋事由
  auto* form1 = new QHBoxLayout;
  type_ = new QComboBox(this);
  for (const QString& t : kTypes) type_->addItem(t);
  from_ = new QLineEdit(this);
  from_->setPlaceholderText(QStringLiteral("开始 2026-10-12"));
  to_ = new QLineEdit(this);
  to_->setPlaceholderText(QStringLiteral("结束 2026-10-13"));
  reason_ = new QLineEdit(this);
  reason_->setPlaceholderText(QStringLiteral("事由（可空）"));
  form1->addWidget(type_);
  form1->addWidget(from_);
  form1->addWidget(to_);
  form1->addWidget(reason_, 1);
  layout->addLayout(form1);

  // 操作区：发起 / 决定 / 撤回 / 刷新
  auto* ops = new QHBoxLayout;
  btn_add_ = new QPushButton(QStringLiteral("发起申请"), this);
  btn_add_->setEnabled(false);
  note_ = new QLineEdit(this);
  note_->setPlaceholderText(QStringLiteral("批注（可空）"));
  btn_approve_ = new QPushButton(QStringLiteral("同意"), this);
  btn_reject_ = new QPushButton(QStringLiteral("拒绝"), this);
  btn_withdraw_ = new QPushButton(QStringLiteral("撤回"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  ops->addWidget(btn_add_);
  ops->addWidget(new QLabel(QStringLiteral("决定→"), this));
  ops->addWidget(note_, 1);
  ops->addWidget(btn_approve_);
  ops->addWidget(btn_reject_);
  ops->addWidget(btn_withdraw_);
  ops->addWidget(btn_refresh_);
  layout->addLayout(ops);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(host_->text(), port_->text().toUShort(),
               account_box_->text(), password_->text());
  });
  connect(btn_add_, &QPushButton::clicked, this, [this] {
    add_approval(type_->currentText(), from_->text().trimmed(),
                 to_->text().trimmed(), reason_->text().trimmed());
  });
  connect(btn_approve_, &QPushButton::clicked, this,
          [this] { decide_selected(true, note_->text().trimmed()); });
  connect(btn_reject_, &QPushButton::clicked, this,
          [this] { decide_selected(false, note_->text().trimmed()); });
  connect(btn_withdraw_, &QPushButton::clicked, this,
          [this] { withdraw_selected(); });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
}

void ApprovalDialog::connect_to(const QString& host, quint16 files_port,
                                const QString& acc, const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  host_->setText(host);
  port_->setText(QString::number(files_port));
  account_box_->setText(acc);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"), QString::number(files_port));
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool ApprovalDialog::is_connected() const { return client_->is_logged_in(); }

bool ApprovalDialog::add_approval(const QString& type, const QString& from,
                                  const QString& to, const QString& reason) {
  if (type.isEmpty()) {
    set_status(QStringLiteral("类型不能为空"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->create_approval(type, from, to, reason);
  return true;
}

bool ApprovalDialog::decide_selected(bool approved, const QString& note) {
  const auto* item = list_->currentItem();
  if (!item || item->data(Qt::UserRole + 1).toString() !=
                   QStringLiteral("pending")) {
    set_status(QStringLiteral("先选中一条待我决的申请"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->decide_approval(selected_id(), approved, note);
  return true;
}

bool ApprovalDialog::withdraw_selected() {
  const auto* item = list_->currentItem();
  // 只有自己名下仍待决的申请可撤（终态行与待我决行都拒）
  if (!item || item->data(Qt::UserRole + 1).toString() !=
                   QStringLiteral("mine") ||
      item->data(Qt::UserRole + 2).toString() != QStringLiteral("pending")) {
    set_status(QStringLiteral("先选中自己名下待决的申请"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->withdraw_approval(selected_id());
  return true;
}

void ApprovalDialog::refresh() {
  if (is_connected()) client_->list_approvals();
}

qint64 ApprovalDialog::selected_id() const {
  const auto* item = list_->currentItem();
  return item ? item->data(Qt::UserRole).toLongLong() : -1;
}

QString ApprovalDialog::selected_kind() const {
  const auto* item = list_->currentItem();
  return item ? item->data(Qt::UserRole + 1).toString() : QString();
}

void ApprovalDialog::populate(const QJsonArray& mine,
                              const QJsonArray& pending) {
  list_->clear();
  for (const auto& v : mine) {
    const auto a = v.toObject();
    const QString st = a.value(QStringLiteral("status")).toString();
    auto* item = new QListWidgetItem(QString(), list_);
    item->setData(Qt::UserRole, a.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, QStringLiteral("mine"));
    item->setData(Qt::UserRole + 2, st);
    QString when;
    const QString from = a.value(QStringLiteral("from")).toString();
    const QString to = a.value(QStringLiteral("to")).toString();
    if (!from.isEmpty() || !to.isEmpty()) {
      when = QStringLiteral(" %1 ~ %2").arg(from.isEmpty() ? QStringLiteral("？")
                                                           : from,
                                           to.isEmpty() ? QStringLiteral("？")
                                                        : to);
    }
    const QString decider = a.value(QStringLiteral("decider")).toString();
    const QString note = a.value(QStringLiteral("decision_note")).toString();
    QString tail;
    if (!decider.isEmpty()) {
      tail = QStringLiteral("（审批人 %1）").arg(decider);
      if (!note.isEmpty()) tail += QStringLiteral("「%1」").arg(note);
    }
    item->setText(QStringLiteral("%1 %2%3  %4%5")
                      .arg(status_label(st),
                           a.value(QStringLiteral("type")).toString(), when,
                           a.value(QStringLiteral("reason")).toString(),
                           tail));
    if (st != QStringLiteral("pending")) item->setForeground(Qt::gray);
  }
  // 待我决（服务端逐行判权后下发；无权者看不到他人的申请）
  for (const auto& v : pending) {
    const auto a = v.toObject();
    auto* item = new QListWidgetItem(QString(), list_);
    item->setData(Qt::UserRole, a.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, QStringLiteral("pending"));
    item->setData(Qt::UserRole + 2,
                  a.value(QStringLiteral("applicant")).toString());
    QString when;
    const QString from = a.value(QStringLiteral("from")).toString();
    const QString to = a.value(QStringLiteral("to")).toString();
    if (!from.isEmpty() || !to.isEmpty()) {
      when = QStringLiteral(" %1 ~ %2").arg(from.isEmpty() ? QStringLiteral("？")
                                                           : from,
                                           to.isEmpty() ? QStringLiteral("？")
                                                        : to);
    }
    item->setText(QStringLiteral("待决 %1 的 %2%3  %4")
                      .arg(a.value(QStringLiteral("applicant")).toString(),
                           a.value(QStringLiteral("type")).toString(), when,
                           a.value(QStringLiteral("reason")).toString()));
  }
  set_status(QStringLiteral("已刷新（我的 %1 项 · 待我决 %2 项）")
                 .arg(mine.size())
                 .arg(pending.size()));
}

void ApprovalDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（恢复用空参刷新路径重设）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : list_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString ApprovalDialog::status_text() const { return status_->text(); }

int ApprovalDialog::approval_count() const { return list_->count(); }

} // namespace memex::client
