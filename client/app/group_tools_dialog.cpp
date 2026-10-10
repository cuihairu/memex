// 二期·群工具三件窗口（实现）。服务端裁决一切判权（群成员可参与、
// 关票/关接龙=发起人或群主/管理员、任务完成=负责人/创建者/群主/管理员、
// 已关 409/已占 409/幽灵 404）——客户端只提交与展示，不自造规则。
#include "group_tools_dialog.hpp"

#include <QCheckBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "engine/collab/files_client.hpp"
#include "notify_center.hpp"

namespace memex::client {
namespace {
// 选项票数统计行内渲染：面馆×1 食堂×2
QString tally_text(const QJsonArray& options, const QJsonArray& counts) {
  QStringList parts;
  for (int i = 0; i < options.size() && i < counts.size(); ++i) {
    parts << QStringLiteral("%1×%2")
                 .arg(options.at(i).toString())
                 .arg(counts.at(i).toInt());
  }
  return parts.join(QStringLiteral(" "));
}

// 记名台账行内渲染：member1→1, owner1→2；多选 choice 为位集→展开 1+3
QString votes_text(const QJsonArray& votes, bool multi) {
  QStringList parts;
  for (const auto& v : votes) {
    const auto o = v.toObject();
    QString choice_s;
    const int choice = o.value(QStringLiteral("choice")).toInt();
    if (multi) {
      QStringList bits;
      for (int i = 0; i < 30; ++i) {
        if ((choice >> i) & 1) bits << QString::number(i + 1);
      }
      choice_s = bits.join(QStringLiteral("+"));
    } else {
      choice_s = QString::number(choice);
    }
    parts << QStringLiteral("%1→%2")
                 .arg(o.value(QStringLiteral("account")).toString(),
                      choice_s);
  }
  return parts.join(QStringLiteral(", "));
}

// 「+」分段段数（非空段计数，连续空段不计）——与服务端格式校验同口径
int chain_segment_count(const QString& s) {
  int n = 0;
  bool in = false;
  for (const QChar ch : s) {
    if (ch != QLatin1Char('+') && !in) {
      ++n;
      in = true;
    } else if (ch == QLatin1Char('+')) {
      in = false;
    }
  }
  return n;
}
} // namespace

GroupToolsDialog::GroupToolsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群工具"));
  resize(680, 520);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral(" · 群 ") + gid_box_->text() +
               QStringLiteral("）"));
    refresh();
    // 群任务到点轮询（30s）：开窗期提醒经通知中心（关窗即停——
    // 开窗期口径，同 R27-1 初版；findChild 守卫防重连重入）
    if (!findChild<QTimer*>(QStringLiteral("gtask_poll"))) {
      auto* poll = new QTimer(this);
      poll->setObjectName(QStringLiteral("gtask_poll"));
      connect(poll, &QTimer::timeout, this, [this] {
        check_due_tasks();
        refresh();
      });
      poll->start(30000);
    }
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::polls_listed, this,
          &GroupToolsDialog::populate_polls);
  connect(client_, &FilesClient::chains_listed, this,
          &GroupToolsDialog::populate_chains);
  connect(client_, &FilesClient::group_tasks_listed, this,
          &GroupToolsDialog::populate_tasks);
  // 全部动作成功后刷新对应页（改票覆盖/认领占位/终态留痕即时可见）
  const auto refresh_all = [this] {
    const quint64 gid = gid_box_->text().toULongLong();
    client_->list_polls(gid);
    client_->list_chains(gid);
    client_->list_group_tasks(gid);
  };
  connect(client_, &FilesClient::poll_created, this, [this, refresh_all](qint64) {
    poll_topic_->clear();
    poll_options_->clear();
    set_status(QStringLiteral("投票已发起"));
    refresh_all();
  });
  connect(client_, &FilesClient::poll_voted, this, [this, refresh_all](qint64) {
    set_status(QStringLiteral("已投票（改票=再投一次覆盖）"));
    refresh_all();
  });
  connect(client_, &FilesClient::poll_closed, this, [this, refresh_all](qint64) {
    set_status(QStringLiteral("投票已截止"));
    refresh_all();
  });
  connect(client_, &FilesClient::chain_created, this,
          [this, refresh_all](qint64) {
            chain_title_->clear();
            chain_hint_->clear();
            set_status(QStringLiteral("接龙已发起"));
            refresh_all();
          });
  connect(client_, &FilesClient::chain_joined, this, [this, refresh_all](qint64) {
    chain_content_->clear();
    set_status(QStringLiteral("已接龙（重复提交=更新自己条目）"));
    refresh_all();
  });
  connect(client_, &FilesClient::chain_closed, this, [this, refresh_all](qint64) {
    set_status(QStringLiteral("接龙已截止"));
    refresh_all();
  });
  connect(client_, &FilesClient::group_task_created, this,
          [this, refresh_all](qint64) {
            task_title_->clear();
            task_assignee_->clear();
            set_status(QStringLiteral("任务已创建"));
            refresh_all();
          });
  connect(client_, &FilesClient::group_task_claimed, this,
          [this, refresh_all](qint64) {
            set_status(QStringLiteral("已认领"));
            refresh_all();
          });
  connect(client_, &FilesClient::group_task_done, this,
          [this, refresh_all](qint64) {
            set_status(QStringLiteral("任务已完成（终态留痕）"));
            refresh_all();
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

void GroupToolsDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // 连接区（与审批窗口同构＋群号——三页共享的群域操作对象）
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
  gid_box_ = new QLineEdit(this);
  gid_box_->setPlaceholderText(QStringLiteral("群号"));
  gid_box_->setMaximumWidth(80);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn->addWidget(host_);
  conn->addWidget(port_);
  conn->addWidget(account_box_);
  conn->addWidget(password_);
  conn->addWidget(gid_box_);
  conn->addWidget(btn_connect_);
  layout->addLayout(conn);

  // 三页签：投票 / 接龙 / 群任务
  tabs_ = new QTabWidget(this);
  layout->addWidget(tabs_, 1);

  // —— 投票页 ——
  auto* poll_page = new QWidget(this);
  auto* poll_layout = new QVBoxLayout(poll_page);
  poll_list_ = new QListWidget(poll_page);
  poll_list_->setAlternatingRowColors(true);
  poll_layout->addWidget(poll_list_, 1);
  auto* poll_form = new QHBoxLayout;
  poll_topic_ = new QLineEdit(poll_page);
  poll_topic_->setPlaceholderText(QStringLiteral("主题"));
  poll_options_ = new QLineEdit(poll_page);
  poll_options_->setPlaceholderText(QStringLiteral("选项（逗号分隔，2~10 个）"));
  poll_form->addWidget(poll_topic_);
  poll_form->addWidget(poll_options_, 1);
  poll_layout->addLayout(poll_form);
  // 截止时间（勾选才生效；不勾=不限期。到点判定在服务端，此处只是输入）
  auto* poll_deadline_row = new QHBoxLayout;
  poll_deadline_on_ = new QCheckBox(QStringLiteral("设截止"), poll_page);
  poll_deadline_ = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600),
                                     poll_page);
  poll_deadline_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
  poll_deadline_->setEnabled(false);
  connect(poll_deadline_on_, &QCheckBox::toggled, poll_deadline_,
          &QDateTimeEdit::setEnabled);
  poll_anon_ = new QCheckBox(QStringLiteral("匿名"), poll_page);
  poll_multi_ = new QCheckBox(QStringLiteral("多选"), poll_page);
  poll_deadline_row->addWidget(poll_deadline_on_);
  poll_deadline_row->addWidget(poll_deadline_);
  poll_deadline_row->addWidget(poll_anon_);
  poll_deadline_row->addWidget(poll_multi_);
  poll_deadline_row->addStretch(1);
  poll_layout->addLayout(poll_deadline_row);
  auto* poll_ops = new QHBoxLayout;
  btn_poll_add_ = new QPushButton(QStringLiteral("发起投票"), poll_page);
  poll_choice_ = new QLineEdit(poll_page);
  poll_choice_->setPlaceholderText(QStringLiteral("选项号（多选可 1,3）"));
  poll_choice_->setMaximumWidth(130);
  btn_poll_vote_ = new QPushButton(QStringLiteral("投票/改票"), poll_page);
  btn_poll_close_ = new QPushButton(QStringLiteral("截止投票"), poll_page);
  poll_ops->addWidget(btn_poll_add_);
  poll_ops->addWidget(new QLabel(QStringLiteral("投→"), poll_page));
  poll_ops->addWidget(poll_choice_);
  poll_ops->addWidget(btn_poll_vote_);
  poll_ops->addWidget(btn_poll_close_);
  poll_ops->addStretch(1);
  poll_layout->addLayout(poll_ops);
  tabs_->addTab(poll_page, QStringLiteral("投票"));

  // —— 接龙页 ——
  auto* chain_page = new QWidget(this);
  auto* chain_layout = new QVBoxLayout(chain_page);
  chain_list_ = new QListWidget(chain_page);
  chain_list_->setAlternatingRowColors(true);
  chain_layout->addWidget(chain_list_, 1);
  auto* chain_form = new QHBoxLayout;
  chain_title_ = new QLineEdit(chain_page);
  chain_title_->setPlaceholderText(QStringLiteral("主题"));
  chain_hint_ = new QLineEdit(chain_page);
  chain_hint_->setPlaceholderText(QStringLiteral("格式提示（可空）"));
  chain_form->addWidget(chain_title_);
  chain_form->addWidget(chain_hint_, 1);
  chain_layout->addLayout(chain_form);
  auto* chain_ops = new QHBoxLayout;
  btn_chain_add_ = new QPushButton(QStringLiteral("发起接龙"), chain_page);
  chain_content_ = new QLineEdit(chain_page);
  chain_content_->setPlaceholderText(QStringLiteral("接龙内容（一人一条，重复提交=更新）"));
  btn_chain_join_ = new QPushButton(QStringLiteral("接龙"), chain_page);
  btn_chain_close_ = new QPushButton(QStringLiteral("截止接龙"), chain_page);
  chain_ops->addWidget(btn_chain_add_);
  chain_ops->addWidget(chain_content_, 1);
  chain_ops->addWidget(btn_chain_join_);
  chain_ops->addWidget(btn_chain_close_);
  chain_layout->addLayout(chain_ops);
  tabs_->addTab(chain_page, QStringLiteral("接龙"));

  // —— 群任务页 ——
  auto* task_page = new QWidget(this);
  auto* task_layout = new QVBoxLayout(task_page);
  task_list_ = new QListWidget(task_page);
  task_list_->setAlternatingRowColors(true);
  task_layout->addWidget(task_list_, 1);
  auto* task_form = new QHBoxLayout;
  task_title_ = new QLineEdit(task_page);
  task_title_->setPlaceholderText(QStringLiteral("任务标题"));
  task_assignee_ = new QLineEdit(task_page);
  task_assignee_->setPlaceholderText(QStringLiteral("负责人（空=待认领）"));
  task_form->addWidget(task_title_);
  task_form->addWidget(task_assignee_, 1);
  task_layout->addLayout(task_form);
  // 截止时间（勾选才生效；不勾=不限期，同投票截止行；到点提醒在客户端
  // 轮询侧，服务端只存 due_ms）
  auto* task_due_row = new QHBoxLayout;
  task_deadline_on_ = new QCheckBox(QStringLiteral("设截止"), task_page);
  task_deadline_on_->setObjectName(QStringLiteral("task_deadline_on"));
  task_deadline_ = new QDateTimeEdit(
      QDateTime::currentDateTime().addSecs(3600), task_page);
  task_deadline_->setObjectName(QStringLiteral("task_deadline"));
  task_deadline_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
  task_deadline_->setEnabled(false);
  connect(task_deadline_on_, &QCheckBox::toggled, task_deadline_,
          &QDateTimeEdit::setEnabled);
  task_due_row->addWidget(task_deadline_on_);
  task_due_row->addWidget(task_deadline_);
  task_due_row->addStretch(1);
  task_layout->addLayout(task_due_row);
  auto* task_ops = new QHBoxLayout;
  btn_task_add_ = new QPushButton(QStringLiteral("建任务"), task_page);
  btn_task_claim_ = new QPushButton(QStringLiteral("认领"), task_page);
  btn_task_done_ = new QPushButton(QStringLiteral("完成"), task_page);
  task_ops->addWidget(btn_task_add_);
  task_ops->addWidget(btn_task_claim_);
  task_ops->addWidget(btn_task_done_);
  task_ops->addStretch(1);
  task_layout->addLayout(task_ops);
  tabs_->addTab(task_page, QStringLiteral("群任务"));

  // 刷新＋状态行
  auto* foot = new QHBoxLayout;
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  foot->addWidget(btn_refresh_);
  foot->addStretch(1);
  layout->addLayout(foot);
  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(host_->text(), port_->text().toUShort(),
               account_box_->text(), password_->text(),
               gid_box_->text().toULongLong());
  });
  connect(btn_poll_add_, &QPushButton::clicked, this, [this] {
    add_poll(poll_topic_->text().trimmed(),
             poll_options_->text().split(QLatin1Char(','),
                                         Qt::SkipEmptyParts),
             poll_deadline_on_->isChecked()
                 ? poll_deadline_->dateTime().toMSecsSinceEpoch()
                 : 0,
             poll_anon_->isChecked(), poll_multi_->isChecked());
  });
  connect(btn_poll_vote_, &QPushButton::clicked, this, [this] {
    // 多选行输入「1,3」→位集；单选行照旧单选项号（服务端权威裁决兜底）
    const auto* item = poll_list_->currentItem();
    vote_selected(item && item->data(Qt::UserRole + 1).toBool()
                      ? parse_choices(poll_choice_->text())
                      : poll_choice_->text().trimmed().toInt());
  });
  connect(btn_poll_close_, &QPushButton::clicked, this,
          [this] { close_selected_poll(); });
  connect(btn_chain_add_, &QPushButton::clicked, this, [this] {
    add_chain(chain_title_->text().trimmed(), chain_hint_->text().trimmed());
  });
  connect(btn_chain_join_, &QPushButton::clicked, this, [this] {
    join_selected(chain_content_->text().trimmed());
  });
  connect(btn_chain_close_, &QPushButton::clicked, this,
          [this] { close_selected_chain(); });
  connect(btn_task_add_, &QPushButton::clicked, this, [this] {
    add_task(task_title_->text().trimmed(), task_assignee_->text().trimmed(),
             task_deadline_on_->isChecked()
                 ? task_deadline_->dateTime().toMSecsSinceEpoch()
                 : 0);
  });
  connect(btn_task_claim_, &QPushButton::clicked, this,
          [this] { claim_selected(); });
  connect(btn_task_done_, &QPushButton::clicked, this,
          [this] { done_selected(); });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
}

void GroupToolsDialog::connect_to(const QString& host, quint16 files_port,
                                  const QString& acc, const QString& pass,
                                  quint64 gid) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  if (gid == 0) {
    set_status(QStringLiteral("群号须为正整数"), true);
    return;
  }
  host_->setText(host);
  port_->setText(QString::number(files_port));
  account_box_->setText(acc);
  gid_box_->setText(QString::number(gid));
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"), QString::number(files_port));
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool GroupToolsDialog::is_connected() const { return client_->is_logged_in(); }

bool GroupToolsDialog::require_connected() {
  if (is_connected()) return true;
  set_status(QStringLiteral("未连接文件面"), true);
  return false;
}

qint64 GroupToolsDialog::selected_id(QListWidget* list) const {
  const auto* item = list->currentItem();
  return item ? item->data(Qt::UserRole).toLongLong() : -1;
}

bool GroupToolsDialog::add_poll(const QString& topic,
                                const QStringList& options,
                                qint64 deadline_ms, bool anonymous,
                                bool multi) {
  if (topic.isEmpty() || options.size() < 2) {
    set_status(QStringLiteral("主题不能空，选项至少 2 个（逗号分隔）"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->create_poll(gid_box_->text().toULongLong(), topic, options,
                       deadline_ms, anonymous, multi);
  return true;
}

bool GroupToolsDialog::vote_selected(int choice) {
  const qint64 id = selected_id(poll_list_);
  if (id < 0) {
    set_status(QStringLiteral("先选中一条投票"), true);
    return false;
  }
  if (choice < 1) {
    set_status(QStringLiteral("选项号须为 1 起的整数"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->vote_poll(gid_box_->text().toULongLong(), id, choice);
  return true;
}

int GroupToolsDialog::parse_choices(const QString& text) const {
  // 「1,3」→位集（多选）；非法（非数字/0/重复）返回 0
  int bits = 0;
  for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    bool ok = false;
    const int c = part.trimmed().toInt(&ok);
    if (!ok || c < 1 || c > 30) return 0;
    const int bit = 1 << (c - 1);
    if (bits & bit) return 0;  // 重复选项号
    bits |= bit;
  }
  return bits;
}

bool GroupToolsDialog::close_selected_poll() {
  const qint64 id = selected_id(poll_list_);
  if (id < 0) {
    set_status(QStringLiteral("先选中一条投票"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->close_poll(gid_box_->text().toULongLong(), id);
  return true;
}

bool GroupToolsDialog::add_chain(const QString& title,
                                 const QString& format_hint) {
  if (title.isEmpty()) {
    set_status(QStringLiteral("主题不能为空"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->create_chain(gid_box_->text().toULongLong(), title, format_hint);
  return true;
}

bool GroupToolsDialog::join_selected(const QString& content) {
  const qint64 id = selected_id(chain_list_);
  if (id < 0) {
    set_status(QStringLiteral("先选中一条接龙"), true);
    return false;
  }
  if (content.isEmpty()) {
    set_status(QStringLiteral("接龙内容不能为空"), true);
    return false;
  }
  // 格式本地门：与选中接龙的格式提示同口径校验段数（「+」分段、空段
  // 不计数），只挡明显误操作——服务端同款裁决兜底
  QString hint;
  for (int i = 0; i < chain_list_->count(); ++i) {
    const auto* it = chain_list_->item(i);
    if ((it->flags() & Qt::ItemIsSelectable) &&
        it->data(Qt::UserRole).toLongLong() == id) {
      hint = it->data(Qt::UserRole + 1).toString();
      break;
    }
  }
  if (hint.contains(QLatin1Char('+'))) {
    const int want = chain_segment_count(hint);
    const int got = chain_segment_count(content);
    if (got != want) {
      set_status(QStringLiteral("条目须按格式提示「%1」分 %2 段（用 + 分隔）")
                     .arg(hint)
                     .arg(want),
                 true);
      return false;
    }
  }
  if (!require_connected()) return false;
  client_->join_chain(gid_box_->text().toULongLong(), id, content);
  return true;
}

bool GroupToolsDialog::close_selected_chain() {
  const qint64 id = selected_id(chain_list_);
  if (id < 0) {
    set_status(QStringLiteral("先选中一条接龙"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->close_chain(gid_box_->text().toULongLong(), id);
  return true;
}

bool GroupToolsDialog::add_task(const QString& title,
                                const QString& assignee, qint64 due_ms) {
  if (title.isEmpty()) {
    set_status(QStringLiteral("任务标题不能为空"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->create_group_task(gid_box_->text().toULongLong(), title, assignee,
                             due_ms);
  return true;
}

bool GroupToolsDialog::claim_selected() {
  const qint64 id = selected_id(task_list_);
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->claim_group_task(gid_box_->text().toULongLong(), id);
  return true;
}

bool GroupToolsDialog::done_selected() {
  const qint64 id = selected_id(task_list_);
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  if (!require_connected()) return false;
  client_->done_group_task(gid_box_->text().toULongLong(), id);
  return true;
}

void GroupToolsDialog::refresh() {
  if (!is_connected()) return;
  const quint64 gid = gid_box_->text().toULongLong();
  client_->list_polls(gid);
  client_->list_chains(gid);
  client_->list_group_tasks(gid);
}

void GroupToolsDialog::populate_polls(const QJsonArray& polls) {
  poll_list_->clear();
  for (const auto& v : polls) {
    const auto p = v.toObject();
    // 到点自动截止由服务端现算 status 下发（惰性判定不回写 closed）；
    // 客户端展示只为不误导，投票判权兜底仍在服务端
    const bool closed =
        p.value(QStringLiteral("status")).toString() ==
            QStringLiteral("closed") ||
        p.value(QStringLiteral("closed")).toBool();
    const bool multi = p.value(QStringLiteral("multi")).toBool();
    const bool anon = p.value(QStringLiteral("anonymous")).toBool();
    auto* item = new QListWidgetItem(QString(), poll_list_);
    item->setData(Qt::UserRole,
                  p.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, multi);
    QString head = closed ? QStringLiteral("[已截止] ")
                          : QStringLiteral("[进行中] ");
    QString tag;
    if (anon) tag += QStringLiteral("[匿名]");
    if (multi) tag += QStringLiteral("[多选]");
    item->setText(QStringLiteral("%1%2#%3 %4 ｜%5 ｜发起人 %6 ｜%7")
                      .arg(head, tag,
                           QString::number(
                               p.value(QStringLiteral("id")).toDouble()),
                           p.value(QStringLiteral("topic")).toString(),
                           tally_text(p.value(QStringLiteral("options")).toArray(),
                                      p.value(QStringLiteral("counts")).toArray()),
                           p.value(QStringLiteral("created_by")).toString(),
                           anon ? QStringLiteral("匿名投票不展示投票人")
                                : votes_text(
                                      p.value(QStringLiteral("votes"))
                                          .toArray(),
                                      multi)));
    if (closed) item->setForeground(Qt::gray);
  }
}

void GroupToolsDialog::populate_chains(const QJsonArray& chains) {
  chain_list_->clear();
  for (const auto& v : chains) {
    const auto c = v.toObject();
    const bool closed = c.value(QStringLiteral("closed")).toBool();
    // 接龙条目平铺在主题行下（服务端已按 ts ASC 排序）
    auto* head = new QListWidgetItem(QString(), chain_list_);
    head->setData(Qt::UserRole, c.value(QStringLiteral("id")).toDouble());
    head->setData(Qt::UserRole + 1,
                  c.value(QStringLiteral("format_hint")).toString());
    head->setText(QStringLiteral("%1#%2 %3%4 ｜发起人 %5")
                      .arg(closed ? QStringLiteral("[已截止] ")
                                  : QStringLiteral("[进行中] "),
                           QString::number(
                               c.value(QStringLiteral("id")).toDouble()),
                           c.value(QStringLiteral("title")).toString(),
                           c.value(QStringLiteral("format_hint"))
                                   .toString()
                                   .isEmpty()
                               ? QString()
                               : QStringLiteral("（%1）").arg(
                                     c.value(QStringLiteral("format_hint"))
                                         .toString()),
                           c.value(QStringLiteral("created_by")).toString()));
    if (closed) head->setForeground(Qt::gray);
    const QJsonArray entries =
        c.value(QStringLiteral("entries")).toArray();
    for (const auto& e : entries) {
      const auto o = e.toObject();
      auto* row = new QListWidgetItem(chain_list_);
      row->setText(QStringLiteral("    %1：%2")
                       .arg(o.value(QStringLiteral("account")).toString(),
                            o.value(QStringLiteral("content")).toString()));
      row->setFlags(Qt::NoItemFlags); // 条目行不可选中（操作只对主题行）
    }
  }
}

void GroupToolsDialog::populate_tasks(const QJsonArray& tasks) {
  task_list_->clear();
  for (const auto& v : tasks) {
    const auto t = v.toObject();
    const QString status = t.value(QStringLiteral("status")).toString();
    auto* item = new QListWidgetItem(QString(), task_list_);
    item->setData(Qt::UserRole, t.value(QStringLiteral("id")).toDouble());
    // 到点检查面（check_due_tasks 用）：due/status/assignee/creator/title
    item->setData(Qt::UserRole + 1,
                  static_cast<qint64>(
                      t.value(QStringLiteral("due_ms")).toDouble()));
    item->setData(Qt::UserRole + 2, status);
    item->setData(Qt::UserRole + 3,
                  t.value(QStringLiteral("assignee")).toString());
    item->setData(Qt::UserRole + 4,
                  t.value(QStringLiteral("created_by")).toString());
    item->setData(Qt::UserRole + 5,
                  t.value(QStringLiteral("title")).toString());
    const QString assignee =
        t.value(QStringLiteral("assignee")).toString();
    QString who = assignee.isEmpty() ? QStringLiteral("待认领")
                                     : QStringLiteral("负责人 %1").arg(assignee);
    QString tail;
    if (status == QStringLiteral("done")) {
      tail = QStringLiteral("（%1 完成）")
                 .arg(t.value(QStringLiteral("done_by")).toString());
    } else if (t.value(QStringLiteral("claimed_ms")).toDouble() > 0) {
      tail = QStringLiteral("（已认领）");
    }
    QString due_tail;
    const qint64 due = item->data(Qt::UserRole + 1).toLongLong();
    if (due > 0) {
      due_tail = QStringLiteral(" ｜截止 %1")
                     .arg(QDateTime::fromMSecsSinceEpoch(due).toString(
                         QStringLiteral("MM-dd HH:mm")));
    }
    item->setText(QStringLiteral("#%1 %2 ｜%3 ｜创建人 %4%5%6")
                      .arg(QString::number(
                               t.value(QStringLiteral("id")).toDouble()),
                           t.value(QStringLiteral("title")).toString(), who,
                           t.value(QStringLiteral("created_by")).toString(),
                           tail, due_tail));
    if (status == QStringLiteral("done")) item->setForeground(Qt::gray);
  }
}

void GroupToolsDialog::check_due_tasks() {
  if (!is_connected()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const QString me = client_->account();
  for (int i = 0; i < task_list_->count(); ++i) {
    const auto* it = task_list_->item(i);
    if (it->data(Qt::UserRole + 2).toString() != QStringLiteral("todo")) {
      continue; // 已完成/已认领终态不提醒
    }
    const qint64 due = it->data(Qt::UserRole + 1).toLongLong();
    if (due <= 0 || due > now) continue;
    const qint64 id = it->data(Qt::UserRole).toLongLong();
    if (gtask_reminded_.contains(id)) continue; // 会话内只提醒一次
    const QString assignee = it->data(Qt::UserRole + 3).toString();
    const QString creator = it->data(Qt::UserRole + 4).toString();
    // 提醒对象：负责人；无人认领时=创建人（谁派谁盯；其余成员不打扰）
    if (assignee.isEmpty() ? creator != me : assignee != me) continue;
    gtask_reminded_.insert(id);
    NotificationCenter::instance().on_notice(
        QStringLiteral("群任务提醒"),
        it->data(Qt::UserRole + 5).toString(),
        QStringLiteral("群任务已到期待办"), /*IMPORTANT*/ 2, QString(), now,
        QStringLiteral("gtask-%1-%2").arg(id).arg(now));
  }
}

void GroupToolsDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（同审批窗口口径）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : task_list_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString GroupToolsDialog::status_text() const { return status_->text(); }

} // namespace memex::client
