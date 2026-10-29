// 二期·日报周报窗口（实现）。服务端裁决一切判权（看下属归直属上级专属、
// 当日重复=upsert 更新）——客户端只提交与展示；周报=按周过滤的聚合视图
//（客户端过滤，不单设表）。界面按 prototypes/desktop-report.html。
#include "report_dialog.hpp"

#include <QColor>
#include <QDate>
#include <QDateEdit>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "engine/collab/files_client.hpp"

namespace memex::client {
namespace {
// 原型 .w-h / .rs-head 区块标题行：标题 + 计数 chip + 说明
struct Head {
  QHBoxLayout* layout;
  QLabel* chip;
};

Head head_row(QWidget* parent, const QString& title, const QString& note) {
  Head h;
  h.layout = new QHBoxLayout;
  auto* t = new QLabel(title, parent);
  t->setStyleSheet(QStringLiteral("font-size:13px;font-weight:700;"));
  h.layout->addWidget(t);
  h.chip = new QLabel(parent);
  h.chip->setStyleSheet(QStringLiteral(
      "font-size:10.5px;font-weight:600;color:palette(mid);"
      "background:palette(base);border:1px solid palette(mid);"
      "border-radius:999px;padding:0 8px;"));
  h.layout->addWidget(h.chip);
  if (!note.isEmpty()) {
    auto* n = new QLabel(note, parent);
    n->setStyleSheet(QStringLiteral(
        "font-size:10.5px;font-weight:600;color:palette(mid);"));
    h.layout->addWidget(n);
  }
  h.layout->addStretch(1);
  return h;
}

// 原型 .hint 提示行（10.5px 弱化）
QLabel* hint_row(QWidget* parent, const QString& text) {
  auto* h = new QLabel(text, parent);
  h->setWordWrap(true);
  h->setStyleSheet(QStringLiteral(
      "font-size:10.5px;color:palette(mid);"));
  return h;
}
} // namespace

ReportDialog::ReportDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("日报周报"));
  resize(1040, 660);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）——只读他人日报需 org_reporting_lines 权限"));
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

  // —— 双列（原型 .split）：左 写日报/周报补写 420px ｜ 右 我的日报＋下属日报 ——
  auto* split = new QHBoxLayout;
  split->setSpacing(0);

  // 左列：写日报 ＋ 周报补写
  auto* write_col = new QVBoxLayout;
  write_col->setContentsMargins(14, 14, 14, 14);
  write_col->setSpacing(11);

  auto h1 = head_row(this, QStringLiteral("写日报"), QString());
  h1.chip->hide();
  write_col->addLayout(h1.layout);
  auto* day_row = new QHBoxLayout;
  auto* day_lb = new QLabel(QStringLiteral("归属日"), this);
  day_lb->setStyleSheet(QStringLiteral("font-size:12px;color:palette(mid);"));
  day_row->addWidget(day_lb);
  date_ = new QDateEdit(QDate::currentDate(), this);
  date_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  date_->setCalendarPopup(true);
  day_row->addWidget(date_);
  day_row->addStretch(1);
  write_col->addLayout(day_row);
  content_ = new QPlainTextEdit(this);
  content_->setPlaceholderText(
      QStringLiteral("今日完成：\n明日计划：\nblockers（无则留空）："));
  content_->setFixedHeight(150);
  write_col->addWidget(content_);
  auto* day_btns = new QHBoxLayout;
  btn_save_ = new QPushButton(QStringLiteral("保存日报"), this);
  btn_save_->setEnabled(false);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  day_btns->addWidget(btn_save_, 1);
  day_btns->addWidget(btn_refresh_, 1);
  write_col->addLayout(day_btns);
  write_col->addWidget(
      hint_row(this, QStringLiteral("当日重复提交＝更新（保留 updated_ms，"
                                    "不留修订历史，与个人备忘录口径一致）。")));

  auto* divider = new QFrame(this);
  divider->setFrameShape(QFrame::HLine);
  divider->setFrameShadow(QFrame::Plain);
  write_col->addWidget(divider);

  auto h2 = head_row(this, QStringLiteral("周报补写"),
                      QStringLiteral("选周内任一天"));
  h2.chip->hide();
  write_col->addLayout(h2.layout);
  auto* week_row = new QHBoxLayout;
  auto* week_lb = new QLabel(QStringLiteral("周内日期"), this);
  week_lb->setStyleSheet(QStringLiteral("font-size:12px;color:palette(mid);"));
  week_row->addWidget(week_lb);
  week_date_ = new QDateEdit(
      QDate::currentDate().addDays(1 - QDate::currentDate().dayOfWeek()),
      this);  // 默认本周一
  week_date_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  week_date_->setCalendarPopup(true);
  week_row->addWidget(week_date_);
  week_row->addStretch(1);
  write_col->addLayout(week_row);
  auto* week_view_row = new QHBoxLayout;
  auto* view_lb = new QLabel(QStringLiteral("周报视图"), this);
  view_lb->setStyleSheet(QStringLiteral("font-size:12px;color:palette(mid);"));
  week_view_row->addWidget(view_lb);
  btn_team_week_ = new QPushButton(this);
  btn_team_week_->setCheckable(true);
  btn_team_week_->setChecked(team_week_only_);
  const auto sync_week_btn = [this] {
    btn_team_week_->setText(team_week_only_ ? QStringLiteral("只看本周：开")
                                            : QStringLiteral("只看本周：关"));
  };
  sync_week_btn();
  week_view_row->addWidget(btn_team_week_, 1);
  write_col->addLayout(week_view_row);
  auto* week_btns = new QHBoxLayout;
  btn_week_save_ = new QPushButton(QStringLiteral("保存周报"), this);
  btn_week_save_->setEnabled(false);
  week_btns->addWidget(btn_week_save_, 1);
  write_col->addLayout(week_btns);
  write_col->addWidget(hint_row(
      this, QStringLiteral("周报＝同一 reports 表按周聚合视图，不另设 week "
                           "维度。补写以所选周的周一日期落一行；同周重复提交"
                           "＝更新。")));

  auto* vline = new QFrame(this);
  vline->setFrameShape(QFrame::VLine);
  vline->setFrameShadow(QFrame::Plain);

  // 右列：我的日报 ＋ 下属日报 ＋ 状态行
  auto* read_col = new QVBoxLayout;
  read_col->setSpacing(0);
  read_col->setContentsMargins(0, 0, 0, 0);

  auto h3 = head_row(this, QStringLiteral("我的日报"),
                      QStringLiteral("点击条目回灌编辑器"));
  read_col->addLayout(h3.layout);
  mine_count_ = h3.chip;
  auto* mine_body = new QVBoxLayout;
  mine_body->setContentsMargins(10, 6, 10, 6);
  list_ = new QListWidget(this);
  list_->setAlternatingRowColors(true);
  list_->setMaximumHeight(210);  // 原型我的日报区限高，给下属区留主体
  mine_body->addWidget(list_);
  read_col->addLayout(mine_body);

  auto h4 = head_row(this, QStringLiteral("下属日报（直属上级可见）"),
                      QStringLiteral("az verb report:read · 仅直属下级一层"));
  read_col->addLayout(h4.layout);
  team_count_ = h4.chip;
  auto* team_body = new QVBoxLayout;
  team_body->setContentsMargins(10, 6, 10, 6);
  team_list_ = new QListWidget(this);
  team_list_->setAlternatingRowColors(true);
  team_body->addWidget(team_list_);
  read_col->addLayout(team_body, 1);

  auto* stat = new QFrame(this);
  stat->setObjectName(QStringLiteral("reportStat"));
  stat->setFrameShape(QFrame::NoFrame);
  stat->setStyleSheet(QStringLiteral(
      "QFrame#reportStat{background:palette(base);"
      "border-top:1px solid palette(mid);}"));
  auto* stat_row = new QHBoxLayout(stat);
  stat_row->setContentsMargins(16, 6, 16, 6);
  status_ = new QLabel(this);
  status_->setStyleSheet(QStringLiteral("font-size:11.5px;"));
  stat_row->addWidget(status_);
  auto* rule = new QLabel(
      QStringLiteral("只读他人日报＝直属上级（同 R27 上级视图口径：不跨级、"
                     "无 org-admin 兜底）"),
      this);
  rule->setStyleSheet(QStringLiteral("font-size:11.5px;color:palette(mid);"));
  stat_row->addWidget(rule);
  stat_row->addStretch(1);
  auto* later = new QLabel(QStringLiteral("部门聚合视图＝后续再做"), this);
  later->setStyleSheet(QStringLiteral("font-size:11.5px;color:palette(mid);"));
  stat_row->addWidget(later);
  read_col->addWidget(stat);

  split->addLayout(write_col, 0);
  split->addWidget(vline);
  split->addLayout(read_col, 1);
  layout->addLayout(split, 1);

  connect(btn_save_, &QPushButton::clicked, this, [this] {
    write_report(date_->date().toString(QStringLiteral("yyyy-MM-dd")),
                 content_->toPlainText().trimmed());
  });
  connect(btn_week_save_, &QPushButton::clicked, this, [this] {
    write_week_report(week_date_->date(),
                      content_->toPlainText().trimmed());
  });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  connect(btn_team_week_, &QPushButton::clicked, this,
          [this, sync_week_btn] {
            toggle_team_week();
            sync_week_btn();
          });
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
  refresh();  // 重新聚合（过滤在客户端，重拉同源数据）
}

QString ReportDialog::week_start() {
  const QDate today = QDate::currentDate();
  return today.addDays(1 - today.dayOfWeek())
      .toString(QStringLiteral("yyyy-MM-dd"));  // 周一
}

QString ReportDialog::week_end() {
  const QDate today = QDate::currentDate();
  return today.addDays(7 - today.dayOfWeek())
      .toString(QStringLiteral("yyyy-MM-dd"));  // 周日
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
        r.value(QStringLiteral("content")).toString().section(
            QLatin1Char('\n'), 0, 0);
    item->setText(QStringLiteral("%1  %2").arg(date, first_line));
  }
  mine_count_->setText(QStringLiteral("%1 篇").arg(list_->count()));
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
      auto* item = new QListWidgetItem(QStringLiteral("%1  %2　%3")
                                           .arg(date, author, first_line),
                                       team_list_);
      item->setData(Qt::UserRole + 2,
                    r.value(QStringLiteral("content")).toString());
      ++shown;
    }
  }
  team_count_->setText(QStringLiteral("%1 篇").arg(shown));
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
