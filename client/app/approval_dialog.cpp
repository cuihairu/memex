// 二期·审批窗口（实现）。服务端裁决一切判权（决定归直属上级/无上级
// org-admin、撤回归申请人且仅 pending、四态只许 pending 可决）——
// 客户端只提交与展示，不自造规则。界面按 prototypes/desktop-approval.html。
#include "approval_dialog.hpp"
#include "collab_defaults.hpp"

#include <QComboBox>
#include <QColor>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

#include "engine/collab/files_client.hpp"
#include "notify_center.hpp"

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

// 起止展示：原型「2026-10-30 ~ 2026-11-01」；空侧显示「？」
QString fmt_when(const QString& from, const QString& to) {
  if (from.isEmpty() && to.isEmpty()) return QString();
  return QStringLiteral(" %1 ~ %2")
      .arg(from.isEmpty() ? QStringLiteral("？") : from,
           to.isEmpty() ? QStringLiteral("？") : to);
}

// 留痕时间：提交带时分，决定/撤回只到日（原型 .op 口径）
QString fmt_op(qint64 ms, const QString& suffix, bool with_time = false) {
  if (ms <= 0) return QString();
  return QDateTime::fromMSecsSinceEpoch(ms)
             .toString(with_time ? QStringLiteral("yyyy-MM-dd HH:mm")
                                 : QStringLiteral("yyyy-MM-dd"))
      .append(suffix.isEmpty() ? QString()
                               : QStringLiteral(" %1").arg(suffix));
}

// 原型 .ls-head/.act-row/.stat 面板条：底色＋分隔线（样式挂容器不挂子件，
// 避免与 QPalette 红色错误态的调色板直改相互覆盖）
QFrame* panel_row(QWidget* parent, bool top_line) {
  auto* f = new QFrame(parent);
  f->setFrameShape(QFrame::NoFrame);
  f->setStyleSheet(top_line ? QStringLiteral("QFrame{background:palette(base);"
                                             "border-top:1px solid palette(mid);}")
                            : QStringLiteral("QFrame{background:palette(base);"
                                             "border-bottom:1px solid palette(mid);}"));
  return f;
}

// 表头计数 chip（原型 .ls-head .c）
QString count_chip_style() {
  return QStringLiteral(
      "font-size:10.5px;font-weight:600;color:palette(mid);"
      "background:palette(base);border:1px solid palette(mid);"
      "border-radius:999px;padding:0 8px;");
}
} // namespace

ApprovalDialog::ApprovalDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("审批"));
  resize(1040, 660);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）——窗口开启期间 30s 轮询差量"));
    btn_add_->setEnabled(true);
    refresh();
    // 开窗期 30s 轮询（R27-1 同口径；服务端推送/通知中心常驻化留后续）
    if (!findChild<QTimer*>(QStringLiteral("approval_poll"))) {
      auto* poll = new QTimer(this);
      poll->setObjectName(QStringLiteral("approval_poll"));
      connect(poll, &QTimer::timeout, this, [this] { refresh(); });
      poll->start(30000);
    }
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
    skip_notify_once_ = true;  // 自己撤回触发的刷新跳过 diff（不提醒自己）
    set_status(QStringLiteral("申请已撤回"));
    refresh();
  });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            if (op == QStringLiteral("approval.list")) return;  // 轮询失败静默
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

  // —— 双列（原型 .split）：左 发起申请 270px ｜ 右 待我决＋我的申请 ——
  auto* split = new QHBoxLayout;
  split->setSpacing(0);

  // 左列：发起申请 · 请假（首版固定模板）
  auto* form_col = new QVBoxLayout;
  form_col->setContentsMargins(14, 14, 14, 14);
  form_col->setSpacing(9);
  auto* form_head =
      new QLabel(QStringLiteral("发起申请 · 请假（首版固定模板）"), this);
  form_head->setStyleSheet(QStringLiteral(
      "font-size:12px;color:palette(mid);letter-spacing:0.04em;"
      "font-weight:700;"));
  form_col->addWidget(form_head);
  auto* form = new QFormLayout;
  form->setContentsMargins(0, 0, 0, 0);
  form->setHorizontalSpacing(7);
  type_ = new QComboBox(this);
  for (const QString& t : kTypes) type_->addItem(t);
  from_ = new QLineEdit(this);
  from_->setPlaceholderText(QStringLiteral("2026-10-26 09:00"));
  from_->setFixedWidth(96);
  to_ = new QLineEdit(this);
  to_->setPlaceholderText(QStringLiteral("2026-10-28 18:00"));
  to_->setFixedWidth(96);
  reason_ = new QLineEdit(this);
  reason_->setPlaceholderText(QStringLiteral("（可空）"));
  for (const auto& row : {std::pair<QWidget*, QWidget*>{
                              new QLabel(QStringLiteral("类型"), this), type_},
                          {new QLabel(QStringLiteral("开始"), this), from_},
                          {new QLabel(QStringLiteral("结束"), this), to_},
                          {new QLabel(QStringLiteral("事由"), this), reason_}}) {
    auto* lb = qobject_cast<QLabel*>(row.first);
    lb->setFixedWidth(40);
    lb->setStyleSheet(QStringLiteral("font-size:12px;color:palette(mid);"));
    form->addRow(row.first, row.second);
  }
  form_col->addLayout(form);
  btn_add_ = new QPushButton(QStringLiteral("发起申请"), this);
  btn_add_->setEnabled(false);
  form_col->addWidget(btn_add_);

  // 原型 .gated 三段说明块（审批人判定 / 权限 / 四态）
  for (const QString& note_text :
       {QStringLiteral(
            "审批人判定：直属上级优先（组织架构 org_reporting_lines 实时查）；"
            "无直属上级则 org-admin 兜底。转交 / 会签 / 多级流＝本期不做。"),
        QStringLiteral(
            "权限：verb approval:decide。自批由 ExplicitDeny"
            "「approval-self-decide」钉在拒绝层——拒绝优先于一切允许。"
            "未命中规则默认拒绝，不自行发明。"),
        QStringLiteral(
            "四态固定：待决 → 已批准 ｜ 已拒绝 ｜ 已撤回。仅待决可决定"
            "（重复决定 409）；撤回＝申请人专属且只从待决发起。"
            "台账全字段留痕，决定不可编辑删除。")}) {
    auto* gated = new QLabel(note_text, this);
    gated->setWordWrap(true);
    gated->setStyleSheet(QStringLiteral(
        "font-size:10.5px;color:palette(mid);background:palette(base);"
        "border:1px dashed palette(mid);border-radius:8px;padding:7px 10px;"));
    form_col->addWidget(gated);
  }
  form_col->addStretch(1);

  // 右列：待我决 / 我的申请 双表（原型 .list-col）
  auto* list_col = new QVBoxLayout;
  list_col->setSpacing(0);
  list_col->setContentsMargins(0, 0, 0, 0);

  auto* pend_head = panel_row(this, false);
  auto* pend_head_row = new QHBoxLayout(pend_head);
  pend_head_row->setContentsMargins(14, 8, 14, 8);
  auto* pend_label =
      new QLabel(QStringLiteral("待我决（我是审批人：直属上级）"), this);
  pend_label->setStyleSheet(QStringLiteral(
      "font-size:12px;font-weight:700;"));
  pend_head_row->addWidget(pend_label);
  pend_head_row->addStretch(1);
  pend_count_ = new QLabel(this);
  pend_count_->setStyleSheet(count_chip_style());
  pend_head_row->addWidget(pend_count_);
  list_col->addWidget(pend_head);
  auto* pend_body = new QVBoxLayout;
  pend_body->setContentsMargins(10, 6, 10, 6);
  pending_list_ = new QListWidget(this);
  pending_list_->setAlternatingRowColors(true);
  pending_list_->setMaximumHeight(180);  // 原型待决表按内容高度
  pend_body->addWidget(pending_list_);
  list_col->addLayout(pend_body);

  auto* mine_head = panel_row(this, false);
  auto* mine_head_row = new QHBoxLayout(mine_head);
  mine_head_row->setContentsMargins(14, 8, 14, 8);
  auto* mine_label =
      new QLabel(QStringLiteral("我的申请（申请人视角，仅待决可撤回）"), this);
  mine_label->setStyleSheet(QStringLiteral(
      "font-size:12px;font-weight:700;"));
  mine_head_row->addWidget(mine_label);
  mine_head_row->addStretch(1);
  mine_count_ = new QLabel(this);
  mine_count_->setStyleSheet(count_chip_style());
  mine_head_row->addWidget(mine_count_);
  list_col->addWidget(mine_head);
  auto* mine_body = new QVBoxLayout;
  mine_body->setContentsMargins(10, 6, 10, 6);
  mine_list_ = new QListWidget(this);
  mine_list_->setAlternatingRowColors(true);
  mine_body->addWidget(mine_list_);
  list_col->addLayout(mine_body, 1);

  // 决定行（原型 .act-row：决定→ + 批注 + 同意/拒绝/撤回/刷新 + 提示）
  auto* act = panel_row(this, true);
  auto* act_row = new QHBoxLayout(act);
  act_row->setContentsMargins(14, 9, 14, 9);
  act_row->addWidget(new QLabel(QStringLiteral("决定→"), this));
  note_ = new QLineEdit(this);
  note_->setPlaceholderText(QStringLiteral("批注（可空）"));
  note_->setFixedWidth(200);
  act_row->addWidget(note_);
  btn_approve_ = new QPushButton(QStringLiteral("同意"), this);
  btn_reject_ = new QPushButton(QStringLiteral("拒绝"), this);
  btn_withdraw_ = new QPushButton(QStringLiteral("撤回"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  act_row->addWidget(btn_approve_);
  act_row->addWidget(btn_reject_);
  act_row->addWidget(btn_withdraw_);
  act_row->addWidget(btn_refresh_);
  act_row->addStretch(1);
  auto* act_hint = new QLabel(
      QStringLiteral("未选中待决条时提示「先选中一条待我决的申请」"), this);
  act_hint->setStyleSheet(QStringLiteral("font-size:11px;color:palette(mid);"));
  act_row->addWidget(act_hint);
  list_col->addWidget(act);

  // 状态行（原型 .stat：状态 + 通知规则 + 通知中心口径）
  auto* stat = panel_row(this, true);
  auto* stat_row = new QHBoxLayout(stat);
  stat_row->setContentsMargins(16, 6, 16, 6);
  status_ = new QLabel(this);
  status_->setStyleSheet(QStringLiteral("font-size:11.5px;"));
  stat_row->addWidget(status_);
  auto* rule = new QLabel(
      QStringLiteral("新增待决 → 提醒审批人；落定 → 提醒申请人；"
                     "自撤回不提醒本人；同一变更只提醒一次"),
      this);
  rule->setStyleSheet(QStringLiteral("font-size:11.5px;color:palette(mid);"));
  stat_row->addWidget(rule);
  stat_row->addStretch(1);
  auto* ctr = new QLabel(
      QStringLiteral("通知经通知中心（优先级 IMPORTANT）"), this);
  ctr->setStyleSheet(QStringLiteral("font-size:11.5px;color:palette(mid);"));
  stat_row->addWidget(ctr);
  list_col->addWidget(stat);

  split->addLayout(form_col, 0);
  split->addLayout(list_col, 1);
  layout->addLayout(split, 1);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(resolve_conn_host(host_->text()), port_->text().toUShort(),
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
  const auto* item = current_item();
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
  const auto* item = current_item();
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

QListWidgetItem* ApprovalDialog::current_item() const {
  if (pending_list_->currentRow() >= 0) return pending_list_->currentItem();
  if (mine_list_->currentRow() >= 0) return mine_list_->currentItem();
  return nullptr;
}

qint64 ApprovalDialog::selected_id() const {
  const auto* item = current_item();
  return item ? item->data(Qt::UserRole).toLongLong() : -1;
}

QString ApprovalDialog::selected_kind() const {
  const auto* item = current_item();
  return item ? item->data(Qt::UserRole + 1).toString() : QString();
}

QString ApprovalDialog::selected_text() const {
  const auto* item = current_item();
  return item ? item->text() : QString();
}

bool ApprovalDialog::select_row_containing(const QString& needle) {
  for (QListWidget* w : {pending_list_, mine_list_}) {
    for (int i = 0; i < w->count(); ++i) {
      if (w->item(i)->text().contains(needle)) {
        w->setCurrentRow(i);
        w->setFocus();
        return true;
      }
    }
  }
  return false;
}

void ApprovalDialog::populate(const QJsonArray& mine,
                              const QJsonArray& pending) {
  // —— 开窗期 diff 通知（R27-1 轮询口径）——首连全量静默吸收（存量
  // 不轰炸）；此后新待决 id→通知待决人、我的申请 pending→终态→通知
  // 申请人；自己撤回触发的刷新跳过。服务端推送/通知中心常驻化留后续。
  const bool notify = seeded_ && !skip_notify_once_;
  skip_notify_once_ = false;
  seeded_ = true;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const auto& v : pending) {
    const auto a = v.toObject();
    const QString id = QString::number(
        static_cast<qint64>(a.value(QStringLiteral("id")).toDouble()));
    if (notify && !seen_pending_.contains(id)) {
      NotificationCenter::instance().on_notice(
          a.value(QStringLiteral("applicant")).toString(),
          QStringLiteral("审批待决"),
          QStringLiteral("%1 提交了 %2 申请，请审批")
              .arg(a.value(QStringLiteral("applicant")).toString(),
                   a.value(QStringLiteral("type")).toString()),
          2 /*IMPORTANT*/, QString(), now,
          QStringLiteral("approval-pending-%1").arg(id));
    }
    seen_pending_.insert(id);
  }
  for (const auto& v : mine) {
    const auto a = v.toObject();
    const QString id = QString::number(
        static_cast<qint64>(a.value(QStringLiteral("id")).toDouble()));
    const QString st = a.value(QStringLiteral("status")).toString();
    const auto prev = seen_mine_status_.constFind(id);
    if (notify && prev != seen_mine_status_.constEnd() &&
        prev.value() == QStringLiteral("pending") && st != prev.value()) {
      NotificationCenter::instance().on_notice(
          QStringLiteral("审批"), QStringLiteral("审批落定"),
          QStringLiteral("你的 %1 申请%2（审批人 %3）")
              .arg(a.value(QStringLiteral("type")).toString(),
                   status_label(st),
                   a.value(QStringLiteral("decider")).toString()),
          2 /*IMPORTANT*/, QString(), now,
          QStringLiteral("approval-decided-%1-%2").arg(id, st));
    }
    seen_mine_status_.insert(id, st);
  }

  // 待我决（服务端逐行判权后下发；无权者看不到他人的申请）
  pending_list_->clear();
  for (const auto& v : pending) {
    const auto a = v.toObject();
    auto* item = new QListWidgetItem(QString(), pending_list_);
    item->setData(Qt::UserRole, a.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, QStringLiteral("pending"));
    item->setData(Qt::UserRole + 2,
                  a.value(QStringLiteral("applicant")).toString());
    const QString reason = a.value(QStringLiteral("reason")).toString();
    const QString when =
        fmt_when(a.value(QStringLiteral("from")).toString(),
                 a.value(QStringLiteral("to")).toString());
    item->setText(
        QStringLiteral("待决　%1 的 %2%3　%4　%5")
            .arg(a.value(QStringLiteral("applicant")).toString(),
                 a.value(QStringLiteral("type")).toString(), when,
                 reason.isEmpty() ? QStringLiteral("事由（可空）")
                                  : QStringLiteral("事由：%1").arg(reason),
                 fmt_op(static_cast<qint64>(
                            a.value(QStringLiteral("created_ms")).toDouble()),
                        QStringLiteral("提交"), true)));
  }

  // 我的申请（含终态留痕；仅待决行可撤回）
  mine_list_->clear();
  for (const auto& v : mine) {
    const auto a = v.toObject();
    const QString st = a.value(QStringLiteral("status")).toString();
    auto* item = new QListWidgetItem(QString(), mine_list_);
    item->setData(Qt::UserRole, a.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, QStringLiteral("mine"));
    item->setData(Qt::UserRole + 2, st);
    const QString when =
        fmt_when(a.value(QStringLiteral("from")).toString(),
                 a.value(QStringLiteral("to")).toString());
    const QString decider = a.value(QStringLiteral("decider")).toString();
    const QString note = a.value(QStringLiteral("decision_note")).toString();
    QString tail;
    if (!decider.isEmpty()) tail = QStringLiteral("（审批人 %1）").arg(decider);
    if (!note.isEmpty()) tail += QStringLiteral("「%1」").arg(note);
    const qint64 created = static_cast<qint64>(
        a.value(QStringLiteral("created_ms")).toDouble());
    const qint64 decided = static_cast<qint64>(
        a.value(QStringLiteral("decided_ms")).toDouble());
    // 终态留痕后缀：批准/拒绝→「决定」，撤回→「撤回」（原型 .op 口径）
    const QString op_suffix = st == QStringLiteral("withdrawn")
                                  ? QStringLiteral("撤回")
                                  : QStringLiteral("决定");
    const QString op = st == QStringLiteral("pending")
                           ? fmt_op(created, QStringLiteral("提交"), true)
                           : fmt_op(decided, op_suffix);
    item->setText(QStringLiteral("%1　我 的 %2%3　%4　%5")
                      .arg(status_label(st),
                           a.value(QStringLiteral("type")).toString(), when,
                           tail, op));
    if (st == QStringLiteral("approved") || st == QStringLiteral("rejected")) {
      item->setForeground(Qt::gray);  // 终态行置灰（原型 .arow.ended）
    } else if (st == QStringLiteral("withdrawn")) {
      item->setForeground(Qt::darkYellow);  // 撤回行警示色（原型 tagw）
    }
  }
  pend_count_->setText(QStringLiteral("%1 条").arg(pending.size()));
  mine_count_->setText(QStringLiteral("%1 条").arg(mine.size()));
  // 换表后不复用旧选中（旧表条目已清空；选中留在空行会误导「可决定」判断）
  pending_list_->setCurrentRow(-1);
  mine_list_->setCurrentRow(-1);
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
                   : pending_list_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString ApprovalDialog::status_text() const { return status_->text(); }

int ApprovalDialog::total_count() const {
  return pending_list_->count() + mine_list_->count();
}

int ApprovalDialog::pending_count() const { return pending_list_->count(); }

int ApprovalDialog::mine_count() const { return mine_list_->count(); }

} // namespace memex::client
