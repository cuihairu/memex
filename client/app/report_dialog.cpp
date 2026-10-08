// 二期·日报周报窗口（实现）。服务端裁决一切判权（看下属归直属上级专属、
// 当日重复=upsert 更新）——客户端只提交与展示；周报=按周过滤的聚合视图
//（客户端过滤，不单设表）。
#include "report_dialog.hpp"

#include <QDate>
#include <QDateEdit>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "engine/collab/files_client.hpp"

namespace memex::client {

ReportDialog::ReportDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("日报周报"));
  resize(600, 520);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）"));
    btn_save_->setEnabled(true);
    btn_week_save_->setEnabled(true);
    refresh();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::reports_listed, this,
          &ReportDialog::populate);
  connect(client_, &FilesClient::team_reports_listed, this,
          &ReportDialog::populate_team);
  connect(client_, &FilesClient::report_saved, this, [this](qint64) {
    set_status(pending_kind_ == QStringLiteral("week")
                   ? QStringLiteral("周报已保存（同周重复提交=更新，以周一落笔）")
                   : QStringLiteral("日报已保存（当日重复提交=更新）"));
    refresh();
  });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

void ReportDialog::build_ui() {
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
  connect(btn_connect_, &QPushButton::clicked, this, [this, host, port,
                                                       account_box, password] {
    connect_to(host->text(), port->text().toUShort(), account_box->text(),
               password->text());
  });

  // 我的日报（date 倒序；点行回填编辑器=改当日即更新语义）
  list_ = new QListWidget(this);
  list_->setAlternatingRowColors(true);
  layout->addWidget(list_, 2);

  // 编辑区：归属日＋三段自由文本（占位提示=模板）
  auto* form = new QHBoxLayout;
  form->addWidget(new QLabel(QStringLiteral("归属日"), this));
  date_ = new QDateEdit(QDate::currentDate(), this);
  date_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  date_->setCalendarPopup(true);
  form->addWidget(date_);
  form->addStretch(1);
  layout->addLayout(form);
  content_ = new QPlainTextEdit(this);
  content_->setPlaceholderText(
      QStringLiteral("今日完成：\n明日计划：\nblockers（无则留空）："));
  content_->setMaximumHeight(140);
  layout->addWidget(content_, 1);

  // 周报补写：任选周内一天→保存时归一到该周周一落笔（同周重复=更新；
  // 与日报同表同 upsert 语义，服务端零改动）
  auto* week_form = new QHBoxLayout;
  week_form->addWidget(
      new QLabel(QStringLiteral("周报补写（选周内任一天）"), this));
  week_date_ = new QDateEdit(
      QDate::currentDate().addDays(1 - QDate::currentDate().dayOfWeek()),
      this); // 默认本周一
  week_date_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  week_date_->setCalendarPopup(true);
  week_form->addWidget(week_date_);
  week_form->addStretch(1);
  layout->addLayout(week_form);

  // 操作区
  auto* ops = new QHBoxLayout;
  btn_save_ = new QPushButton(QStringLiteral("保存日报"), this);
  btn_save_->setEnabled(false);
  btn_week_save_ = new QPushButton(QStringLiteral("保存周报"), this);
  btn_week_save_->setEnabled(false);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  ops->addWidget(btn_save_);
  ops->addWidget(btn_week_save_);
  ops->addWidget(btn_refresh_);
  ops->addStretch(1);
  layout->addLayout(ops);

  // 团队聚合（直属上级可看下属；判权服务端裁，无下属=空列表）
  layout->addWidget(new QLabel(QStringLiteral("下属日报（直属上级可见）"), this));
  team_list_ = new QListWidget(this);
  team_list_->setAlternatingRowColors(true);
  layout->addWidget(team_list_, 1);
  auto* team_ops = new QHBoxLayout;
  btn_team_week_ = new QPushButton(this);
  btn_team_week_->setCheckable(true);
  btn_team_week_->setChecked(team_week_only_);
  team_ops->addWidget(btn_team_week_);
  team_ops->addStretch(1);
  layout->addLayout(team_ops);
  const auto sync_week_btn = [this] {
    btn_team_week_->setText(team_week_only_
                                ? QStringLiteral("只看本周（周报视图）：开")
                                : QStringLiteral("只看本周（周报视图）：关"));
  };
  sync_week_btn();
  connect(btn_team_week_, &QPushButton::clicked, this,
          [this, sync_week_btn] {
            toggle_team_week();
            sync_week_btn();
          });

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_save_, &QPushButton::clicked, this, [this] {
    write_report(date_->date().toString(QStringLiteral("yyyy-MM-dd")),
                 content_->toPlainText().trimmed());
  });
  connect(btn_week_save_, &QPushButton::clicked, this, [this] {
    write_week_report(week_date_->date(),
                      content_->toPlainText().trimmed());
  });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  // 点我的日报行回填编辑器（改完再存=当日 upsert 更新）
  connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
    if (!it) return;
    const QDate d =
        QDate::fromString(it->data(Qt::UserRole + 1).toString(),
                          QStringLiteral("yyyy-MM-dd"));
    if (d.isValid()) date_->setDate(d);
    content_->setPlainText(it->data(Qt::UserRole + 2).toString());
  });
}

void ReportDialog::connect_to(const QString& host, quint16 files_port,
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
  account_ = acc;
  client_->login(host, files_port, acc, pass);
}

bool ReportDialog::is_connected() const { return client_->is_logged_in(); }

bool ReportDialog::write_report(const QString& date, const QString& content) {
  if (content.isEmpty()) {
    set_status(QStringLiteral("内容不能为空"), true);
    return false;
  }
  if (!date.contains(QLatin1Char('-')) || date.size() != 10) {
    set_status(QStringLiteral("归属日须为 YYYY-MM-DD"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  pending_kind_ = QStringLiteral("day");
  client_->save_report(date, content);
  return true;
}

bool ReportDialog::write_week_report(const QDate& any_day,
                                     const QString& content) {
  if (content.isEmpty()) {
    set_status(QStringLiteral("内容不能为空"), true);
    return false;
  }
  if (!any_day.isValid()) {
    set_status(QStringLiteral("周日期非法"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  // 归一到该周周一落笔：与日报同表同 upsert 语义（UNIQUE(author,
  // report_date)，同周重复提交=更新、updated_ms 留痕），服务端零改动
  const QDate mon = any_day.addDays(1 - any_day.dayOfWeek());
  pending_kind_ = QStringLiteral("week");
  client_->save_report(mon.toString(QStringLiteral("yyyy-MM-dd")), content);
  return true;
}

void ReportDialog::refresh() {
  if (!is_connected()) return;
  client_->list_reports();
  client_->fetch_team_reports();
}

void ReportDialog::toggle_team_week() {
  team_week_only_ = !team_week_only_;
  refresh(); // 重新聚合（过滤在客户端，重拉同源数据）
}

QString ReportDialog::week_start() {
  const QDate today = QDate::currentDate();
  return today.addDays(1 - today.dayOfWeek())
      .toString(QStringLiteral("yyyy-MM-dd")); // 周一
}

QString ReportDialog::week_end() {
  const QDate today = QDate::currentDate();
  return today.addDays(7 - today.dayOfWeek())
      .toString(QStringLiteral("yyyy-MM-dd")); // 周日
}

void ReportDialog::populate(const QJsonArray& reports) {
  list_->clear();
  for (const auto& v : reports) {
    const auto r = v.toObject();
    const QString date = r.value(QStringLiteral("date")).toString();
    auto* item = new QListWidgetItem(QString(), list_);
    item->setData(Qt::UserRole, r.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, date);
    item->setData(Qt::UserRole + 2,
                  r.value(QStringLiteral("content")).toString());
    const QString first_line =
        r.value(QStringLiteral("content")).toString().section(QLatin1Char('\n'), 0, 0);
    item->setText(QStringLiteral("%1  %2")
                      .arg(date, first_line));
  }
}

void ReportDialog::populate_team(const QJsonArray& team) {
  team_list_->clear();
  const QString ws = week_start();
  const QString we = week_end();
  int shown = 0;
  for (const auto& g : team) {
    const auto grp = g.toObject();
    const QString author = grp.value(QStringLiteral("author")).toString();
    for (const auto& rv : grp.value(QStringLiteral("reports")).toArray()) {
      const auto r = rv.toObject();
      const QString date = r.value(QStringLiteral("date")).toString();
      // 周报=按周过滤的聚合视图（客户端过滤；YYYY-MM-DD 字典序即可比）
      if (team_week_only_ && (date < ws || date > we)) continue;
      const QString first_line =
          r.value(QStringLiteral("content")).toString().section(
              QLatin1Char('\n'), 0, 0);
      auto* item =
          new QListWidgetItem(QStringLiteral("%1 · %2  %3")
                                  .arg(author, date, first_line),
                              team_list_);
      item->setData(Qt::UserRole + 2,
                    r.value(QStringLiteral("content")).toString());
      ++shown;
    }
  }
  set_status(QStringLiteral("已刷新（我的 %1 篇 · 下属 %2 篇）")
                 .arg(list_->count())
                 .arg(shown));
}

void ReportDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（恢复用空参刷新路径重设）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : list_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString ReportDialog::status_text() const { return status_->text(); }

int ReportDialog::report_count() const { return list_->count(); }

int ReportDialog::team_count() const { return team_list_->count(); }

} // namespace memex::client
