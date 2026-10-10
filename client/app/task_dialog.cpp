// R27-1 个人任务清单窗口（实现）。服务端裁决一切判权（完成归清单主人、
// 撤回归主人或分配人、分配按「同群/同部门」现查现裁）——客户端只提交
// 与展示，不自造规则。
#include "task_dialog.hpp"

#include <QCalendarWidget>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QTextCharFormat>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <QtAlgorithms>

#include "engine/collab/files_client.hpp"
#include "engine/task/dingtalk_provider.hpp"
#include "engine/task/feishu_provider.hpp"
#include "engine/task/github_provider.hpp"
#include "engine/task/task_http.hpp"
#include "notify_center.hpp"
#include "task_provider_settings.hpp"
#include "task_provider_store.hpp"
#include "task_template_settings.hpp"

namespace memex::client {
namespace {
constexpr int kPollMs = 30000; // 开窗期间 30s 轮询（到期待办检查）
} // namespace

TaskDialog::TaskDialog(QWidget* parent, TaskHttp* http,
                       TaskProviderStore* store)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("任务清单"));
  resize(560, 520);
  client_ = new FilesClient(this);
  if (http != nullptr) {
    http_ = http; // 测试注入（非拥有，调用方保存续）
  } else {
    http_ = new QtNetworkTaskHttp(new QNetworkAccessManager(this));
    http_owned_ = true;
  }
  store_ = store != nullptr ? store : &TaskProviderStore::instance();
  build_ui();

  // 凭据变更（设置面保存/清除/换口令）→ live provider 重建＋下拉刷新
  connect(store_, &TaskProviderStore::changed, this,
          [this] { rebuild_live_providers(); });
  rebuild_live_providers();

  // 登录回执：成功即拉列表＋起轮询；失败给状态行
  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）"));
    btn_add_->setEnabled(true);
    btn_add_ext_->setEnabled(true);
    refresh();
    if (!findChild<QTimer*>("task_poll")) {
      auto* poll = new QTimer(this);
      poll->setObjectName(QStringLiteral("task_poll"));
      connect(poll, &QTimer::timeout, this, [this] { check_due(); refresh(); });
      poll->start(kPollMs);
    }
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::tasks_listed, this,
          &TaskDialog::populate);
  connect(client_, &FilesClient::task_created, this, [this](qint64) {
    title_->clear();
    note_->clear();
    set_status(QStringLiteral("任务已添加"));
    refresh();
  });
  connect(client_, &FilesClient::task_done_set, this, [this](qint64) {
    set_status(QStringLiteral("任务状态已更新"));
    refresh();
  });
  connect(client_, &FilesClient::task_deleted, this, [this](qint64) {
    set_status(QStringLiteral("任务已撤回"));
    refresh();
  });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            if (op == QStringLiteral("task.list")) return; // 轮询失败不刷状态行
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

void TaskDialog::build_ui() {
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

  // R27-1 余量 日历视图：月历标到期日（品牌橙加粗），点日筛该日到期
  // 任务，再点同日清筛选回全量。默认收起，「日历」按钮开关。
  calendar_ = new QCalendarWidget(this);
  calendar_->setObjectName(QStringLiteral("task_calendar"));
  calendar_->setGridVisible(true);
  calendar_->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
  calendar_->setVisible(false);
  layout->addWidget(calendar_);

  // 任务列表
  list_ = new QListWidget(this);
  list_->setAlternatingRowColors(true);
  layout->addWidget(list_, 1);

  // 录入区：标题＋备注＋到期＋分配给
  auto* form1 = new QHBoxLayout;
  title_ = new QLineEdit(this);
  title_->setPlaceholderText(QStringLiteral("任务标题"));
  note_ = new QLineEdit(this);
  note_->setPlaceholderText(QStringLiteral("备注（可空）"));
  form1->addWidget(title_, 2);
  form1->addWidget(note_, 3);
  layout->addLayout(form1);
  auto* form2 = new QHBoxLayout;
  due_ = new QDateTimeEdit(this);
  due_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
  due_->setCalendarPopup(true);
  due_->setDateTime(QDateTime::currentDateTime());
  assignee_ = new QComboBox(this);
  assignee_->setEditable(true);
  assignee_->addItem(QStringLiteral("（自己）"));
  form2->addWidget(new QLabel(QStringLiteral("提醒时间"), this));
  form2->addWidget(due_);
  form2->addWidget(new QLabel(QStringLiteral("分配给"), this));
  form2->addWidget(assignee_, 1);
  layout->addLayout(form2);

  // 外部任务登记区（R27-2）：provider（内置 L1 预设＋自定义模板）＋键
  //（「project#键」或完整链接）。登记前本地解析 detail URL——解析不出
  // 不发网不落库。
  auto* form3 = new QHBoxLayout;
  form3->addWidget(new QLabel(QStringLiteral("外部任务"), this));
  ext_provider_ = new QComboBox(this);
  refresh_ext_combo();
  ext_key_ = new QLineEdit(this);
  ext_key_->setPlaceholderText(
      QStringLiteral("仓库/站点#键 或完整链接（如 org/repo#12）"));
  btn_add_ext_ = new QPushButton(QStringLiteral("登记外部任务"), this);
  btn_add_ext_->setEnabled(false);
  form3->addWidget(ext_provider_);
  form3->addWidget(ext_key_, 1);
  form3->addWidget(btn_add_ext_);
  layout->addLayout(form3);

  // 操作区
  auto* ops = new QHBoxLayout;
  btn_add_ = new QPushButton(QStringLiteral("添加任务"), this);
  btn_add_->setEnabled(false);
  btn_toggle_ = new QPushButton(QStringLiteral("完成/回退"), this);
  btn_delete_ = new QPushButton(QStringLiteral("撤回"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  btn_calendar_ = new QPushButton(QStringLiteral("日历"), this);
  ops->addWidget(btn_add_);
  ops->addWidget(btn_toggle_);
  ops->addWidget(btn_delete_);
  ops->addWidget(btn_refresh_);
  ops->addWidget(btn_calendar_);
  ops->addStretch(1);
  layout->addLayout(ops);

  // R27-3 拉取行：已配置凭据且声明 L2 的 provider 直拉外部任务（不经
  // memex 服务端，断连也可用）；⚙ 进设置面（凭据录入/加密落盘）
  auto* form4 = new QHBoxLayout;
  form4->addWidget(new QLabel(QStringLiteral("拉取外部"), this));
  pull_provider_ = new QComboBox(this);
  pull_provider_->setMinimumWidth(150);
  btn_pull_ = new QPushButton(QStringLiteral("拉取"), this);
  btn_settings_ = new QPushButton(QStringLiteral("任务设置…"), this);
  btn_templates_ = new QPushButton(QStringLiteral("模板…"), this);
  form4->addWidget(pull_provider_);
  form4->addWidget(btn_pull_);
  form4->addWidget(btn_settings_);
  form4->addWidget(btn_templates_);
  form4->addStretch(1);
  layout->addLayout(form4);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(host_->text(), port_->text().toUShort(),
               account_box_->text(), password_->text());
  });
  connect(btn_add_, &QPushButton::clicked, this, [this] {
    const QString who = assignee_->currentText().trimmed();
    add_task(title_->text().trimmed(), note_->text().trimmed(),
             due_->dateTime().toMSecsSinceEpoch(),
             who.startsWith(QStringLiteral("（")) ? QString() : who);
  });
  connect(btn_add_ext_, &QPushButton::clicked, this, [this] {
    if (add_external_task(ext_provider_->currentData().toString(),
                          ext_key_->text(), title_->text())) {
      title_->clear();
      ext_key_->clear();
    }
  });
  connect(btn_toggle_, &QPushButton::clicked, this,
          [this] { toggle_selected_done(); });
  connect(btn_delete_, &QPushButton::clicked, this, [this] {
    if (selected_id() < 0) {
      set_status(QStringLiteral("先选中一条任务"), true);
      return;
    }
    const auto ret = QMessageBox::question(
        this, QStringLiteral("撤回任务"),
        QStringLiteral("确定撤回选中的任务？（清单主人或分配人可撤）"));
    if (ret == QMessageBox::Yes) delete_selected();
  });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  connect(btn_calendar_, &QPushButton::clicked, this, [this] {
    calendar_->setVisible(!calendar_->isVisible());
  });
  connect(calendar_, &QCalendarWidget::clicked, this,
          [this](const QDate& d) { toggle_day_filter(d); });
  connect(btn_pull_, &QPushButton::clicked, this, [this] {
    pull_external(pull_provider_->currentData().toString());
  });
  connect(btn_settings_, &QPushButton::clicked, this, [this] {
    auto* dlg = new TaskProviderSettingsDialog(store_, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
  });
  connect(btn_templates_, &QPushButton::clicked, this, [this] {
    auto* dlg = new TaskTemplateSettingsDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    // 保存即持久化；本窗注册表重挂＋登记下拉刷新（自定义模板进登记面）
    connect(dlg, &QDialog::finished, this, [this](int) {
      providers_.reload_custom();
      refresh_ext_combo();
    });
  });
  connect(list_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem* it) {
            // 外部行双击=跳外部详情（外部行的主语义）；本地行双击=勾完成
            const QString url =
                it ? it->data(Qt::UserRole + 5).toString() : QString();
            if (!url.isEmpty()) {
              QDesktopServices::openUrl(QUrl(url));
              return;
            }
            toggle_selected_done();
          });
}

TaskDialog::~TaskDialog() {
  qDeleteAll(live_);
  if (http_owned_) delete http_;
}

// —— R27-1 余量 日历视图 ——

void TaskDialog::update_day_marks() {
  // 无参重载清全表再标——刷新重建后重入不叠色
  calendar_->setDateTextFormat(QDate(), QTextCharFormat());
  QTextCharFormat fmt;
  fmt.setFontWeight(QFont::Bold);
  // 品牌橙（与主窗 accent 兜底同源；本窗未接主题面，同 Qt::gray 行内惯例）
  fmt.setForeground(QColor(QStringLiteral("#e16531")));
  for (int i = 0; i < list_->count(); ++i) {
    const auto* it = list_->item(i);
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    if (due <= 0) continue; // 派出行/⇣ 行/未设提醒项无到期数据
    calendar_->setDateTextFormat(QDateTime::fromMSecsSinceEpoch(due).date(),
                                 fmt);
  }
}

int TaskDialog::apply_day_filter() {
  int visible = 0;
  for (int i = 0; i < list_->count(); ++i) {
    auto* it = list_->item(i);
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    const bool hit =
        day_filter_.isValid() && due > 0 &&
        QDateTime::fromMSecsSinceEpoch(due).date() == day_filter_;
    // 筛选态只留命中日；无到期数据行（派出/⇣/未设提醒）一并隐藏
    it->setHidden(day_filter_.isValid() && !hit);
    if (!it->isHidden()) ++visible;
  }
  return visible;
}

void TaskDialog::toggle_day_filter(const QDate& d) {
  day_filter_ = d.isValid() && d == day_filter_ ? QDate() : d;
  const int visible = apply_day_filter();
  if (day_filter_.isValid()) {
    set_status(QStringLiteral("日历筛选：%1（可见 %2 项，再点同日回全量）")
                   .arg(day_filter_.toString(QStringLiteral("yyyy-MM-dd")),
                        QString::number(visible)));
  } else {
    set_status(QStringLiteral("日历筛选已清（全量 %1 项）")
                   .arg(visible));
  }
}

int TaskDialog::visible_task_count() const {
  int n = 0;
  for (int i = 0; i < list_->count(); ++i) {
    if (!list_->item(i)->isHidden()) ++n;
  }
  return n;
}

void TaskDialog::refresh_ext_combo() {
  const QString prev = ext_provider_->currentData().toString();
  ext_provider_->clear();
  for (const auto* p : providers_.providers()) {
    ext_provider_->addItem(p->name(), p->id());
  }
  const int url_idx = ext_provider_->findData(QStringLiteral("url"));
  if (url_idx >= 0) ext_provider_->setCurrentIndex(url_idx); // 粘贴链接即登记
  const int prev_idx = ext_provider_->findData(prev);
  if (prev_idx >= 0) ext_provider_->setCurrentIndex(prev_idx);
}

void TaskDialog::connect_to(const QString& host, quint16 files_port,
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
  account_ = acc;
  client_->login(host, files_port, acc, pass);
}

bool TaskDialog::is_connected() const { return client_->is_logged_in(); }

void TaskDialog::set_assignees(const QStringList& accounts) {
  while (assignee_->count() > 1) assignee_->removeItem(assignee_->count() - 1);
  for (const QString& a : accounts) {
    if (!a.isEmpty() && a != account_) assignee_->addItem(a);
  }
}

bool TaskDialog::add_task(const QString& title, const QString& note,
                          qint64 due_ms, const QString& assignee) {
  if (title.isEmpty()) {
    set_status(QStringLiteral("标题不能为空"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->create_task(title, note, due_ms, assignee);
  return true;
}

void TaskDialog::split_ext_key(const QString& raw, QString& project,
                               QString& key) {
  const qint64 at = raw.lastIndexOf(QLatin1Char('#'));
  if (at > 0) {
    project = raw.left(at);
    key = raw.mid(at + 1);
  } else {
    key = raw; // 无 # = 整串为键（直通链接等）
  }
}

bool TaskDialog::add_external_task(const QString& provider_id,
                                   const QString& key_input,
                                   const QString& title) {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  const QString raw = key_input.trimmed();
  if (raw.isEmpty()) {
    set_status(QStringLiteral("外部键不能为空"), true);
    return false;
  }
  // detailUrl 必带：解析不出 URL 本地拒不发网不落库（宁缺毋滥造坏链）
  QString project, key;
  split_ext_key(raw, project, key);
  if (providers_.detail_url(provider_id, key, project).isEmpty()) {
    set_status(QStringLiteral("解析不出详情链接（provider 未登记或键不合法）"),
               true);
    return false;
  }
  client_->create_task(title.trimmed().isEmpty() ? raw : title.trimmed(),
                       QString(), 0, QString(), provider_id, raw);
  return true;
}

QString TaskDialog::selected_detail_url() const {
  const auto* item = list_->currentItem();
  return item ? item->data(Qt::UserRole + 5).toString() : QString();
}

int TaskDialog::ext_combo_count() const { return ext_provider_->count(); }

bool TaskDialog::toggle_selected_done() {
  if (selected_row_pulled()) {
    // ⇣ 行是外部现态只读镜像：回写是 L3（complete），清单标记须先登记
    set_status(QStringLiteral("外部拉取行只读——登记后才能在清单标记完成"),
               true);
    return false;
  }
  const qint64 id = selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  // 现值取反：列表行带 done 态（数据角色）
  const auto* item = list_->currentItem();
  const bool cur = item->data(Qt::UserRole + 1).toBool();
  client_->set_task_done(id, !cur);
  return true;
}

bool TaskDialog::delete_selected() {
  if (selected_row_pulled()) {
    set_status(QStringLiteral("外部拉取行只读——外部清单不由 memex 撤回"),
               true);
    return false;
  }
  const qint64 id = selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  client_->delete_task(id); // 确认框归按钮路径（程序化入口不弹框供测试）
  return true;
}

void TaskDialog::refresh() {
  if (is_connected()) client_->list_tasks();
}

// —— R27-3 拉取接线：凭据 store → live provider → 直拉外部列表 ——
void TaskDialog::rebuild_live_providers() {
  qDeleteAll(live_);
  live_.clear();
  if (store_ != nullptr && store_->is_unlocked()) {
    if (store_->contains(QStringLiteral("github-issue"))) {
      const QJsonObject c = store_->config(QStringLiteral("github-issue"));
      live_.insert(QStringLiteral("github-issue"),
                   new GitHubIssuesProvider(
                       c.value(QStringLiteral("repo")).toString(),
                       c.value(QStringLiteral("token")).toString(), http_));
    }
    if (store_->contains(QStringLiteral("dingtalk-todo"))) {
      const QJsonObject c = store_->config(QStringLiteral("dingtalk-todo"));
      live_.insert(QStringLiteral("dingtalk-todo"),
                   new DingtalkTodoProvider(
                       c.value(QStringLiteral("app_key")).toString(),
                       c.value(QStringLiteral("app_secret")).toString(),
                       // unionId 获取流程属设置页后续件；空按未取到用户走
                       c.value(QStringLiteral("union_id")).toString(),
                       http_));
    }
    if (store_->contains(QStringLiteral("feishu-task"))) {
      const QJsonObject c = store_->config(QStringLiteral("feishu-task"));
      live_.insert(QStringLiteral("feishu-task"),
                   new FeishuTaskProvider(
                       c.value(QStringLiteral("app_id")).toString(),
                       c.value(QStringLiteral("app_secret")).toString(),
                       http_));
    }
  }
  // 拉取下拉只收已配置且声明 L2 的（重新装配保持选择尽量不跳）
  const QString prev = pull_provider_->currentData().toString();
  pull_provider_->clear();
  QStringList ids;
  for (auto it = live_.constBegin(); it != live_.constEnd(); ++it) {
    if (it.value()->can_read()) ids << it.key();
  }
  ids.sort();
  for (const QString& id : ids) {
    pull_provider_->addItem(live_.value(id)->name(), id);
  }
  const int idx = pull_provider_->findData(prev);
  if (idx >= 0) pull_provider_->setCurrentIndex(idx);
  btn_pull_->setEnabled(!ids.isEmpty());
}

QStringList TaskDialog::pull_provider_ids() const {
  QStringList out;
  for (int i = 0; i < pull_provider_->count(); ++i) {
    out << pull_provider_->itemData(i).toString();
  }
  return out;
}

void TaskDialog::pull_external(const QString& provider_id) {
  auto* p = live_.value(provider_id, nullptr);
  if (p == nullptr || !p->can_read()) {
    set_status(QStringLiteral("该 provider 未配置或未声明只读能力"
                              "（任务设置里配置凭据）"),
               true);
    return;
  }
  set_status(QStringLiteral("拉取外部任务中（%1）…").arg(p->name()));
  p->list([this, provider_id](bool ok, const QVector<ExternalTask>& items,
                              const QString& err) {
    if (ok) {
      ext_cache_.insert(provider_id, items);
      render_ext_rows();
      set_status(QStringLiteral("外部任务已拉取（%1 %2 项）")
                     .arg(live_.value(provider_id) != nullptr
                              ? live_.value(provider_id)->name()
                              : provider_id,
                          QString::number(items.size())));
    } else {
      set_status(
          QStringLiteral("拉取失败（%1：%2）").arg(provider_id, err), true);
    }
  });
}

void TaskDialog::render_ext_rows() {
  // 旧 ⇣ 行移除（UserRole+6 拉取行标记）后整批重挂
  for (int i = list_->count() - 1; i >= 0; --i) {
    if (list_->item(i)->data(Qt::UserRole + 6).toBool()) {
      delete list_->takeItem(i);
    }
  }
  for (auto it = ext_cache_.constBegin(); it != ext_cache_.constEnd(); ++it) {
    const auto* lp = live_.value(it.key());
    const auto* rp = providers_.provider(it.key());
    const QString pname = lp != nullptr ? lp->name()
                          : rp != nullptr ? rp->name()
                                          : it.key();
    for (const ExternalTask& t : it.value()) {
      auto* item = new QListWidgetItem(QString(), list_);
      item->setData(Qt::UserRole, -1); // 无服务端 id（动作被只读守卫拦）
      item->setData(Qt::UserRole + 1, t.done);
      item->setData(Qt::UserRole + 5, t.detail_url); // 双击跳外部详情
      item->setData(Qt::UserRole + 6, true);
      item->setText(QStringLiteral("%1⇣ %2·%3  %4")
                        .arg(t.done ? QStringLiteral("[x] ")
                                    : QStringLiteral("[ ] "),
                             pname, t.key, t.title));
      if (t.done) item->setForeground(Qt::gray);
    }
  }
}

bool TaskDialog::selected_row_pulled() const {
  const auto* item = list_->currentItem();
  return item != nullptr && item->data(Qt::UserRole + 6).toBool();
}

void TaskDialog::check_due() {
  if (!is_connected()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (int i = 0; i < list_->count(); ++i) {
    const auto* it = list_->item(i);
    if (it->data(Qt::UserRole + 1).toBool()) continue; // 已完成不提醒
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    const qint64 reminded = it->data(Qt::UserRole + 3).toLongLong();
    if (due <= 0 || due > now || reminded != 0) continue;
    NotificationCenter::instance().on_notice(
        QStringLiteral("任务提醒"), it->data(Qt::UserRole + 4).toString(),
        QStringLiteral("任务已到期待办"), /*IMPORTANT*/ 2, QString(), now,
        QStringLiteral("task-%1-%2").arg(it->data(Qt::UserRole).toLongLong()).arg(now));
    client_->mark_task_reminded(it->data(Qt::UserRole).toLongLong());
  }
}

qint64 TaskDialog::selected_id() const {
  const auto* item = list_->currentItem();
  return item ? item->data(Qt::UserRole).toLongLong() : -1;
}

void TaskDialog::populate(const QJsonArray& mine, const QJsonArray& assigned) {
  list_->clear();
  for (const auto& v : mine) {
    const auto t = v.toObject();
    const QString prov = t.value(QStringLiteral("provider")).toString();
    const bool done = t.value(QStringLiteral("done")).toBool();
    auto* item = new QListWidgetItem(QString(), list_);
    item->setData(Qt::UserRole, t.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, done);
    item->setData(Qt::UserRole + 2,
                  static_cast<qint64>(
                      t.value(QStringLiteral("due_ms")).toDouble()));
    item->setData(Qt::UserRole + 3,
                  static_cast<qint64>(
                      t.value(QStringLiteral("reminded_ms")).toDouble()));
    item->setData(Qt::UserRole + 4,
                  t.value(QStringLiteral("title")).toString());
    if (!prov.isEmpty()) {
      // 外部任务（R27-2）：🌐 标注 provider·键原文，现解析 detail URL
      //（provider 未登记/键不合法=无跳转，行仍在、双击给状态提示）
      const QString raw = t.value(QStringLiteral("ext_key")).toString();
      QString project, key;
      split_ext_key(raw, project, key);
      item->setData(Qt::UserRole + 5,
                    providers_.detail_url(prov, key, project));
      const auto* p = providers_.provider(prov);
      item->setText(QStringLiteral("%1🌐 %2  %3·%4")
                        .arg(done ? QStringLiteral("[x] ")
                                  : QStringLiteral("[ ] "),
                             t.value(QStringLiteral("title")).toString(),
                             p != nullptr ? p->name() : prov, raw));
    } else {
      const QString creator =
          t.value(QStringLiteral("creator")).toString();
      const bool self = creator == account_;
      const qint64 due = item->data(Qt::UserRole + 2).toLongLong();
      QString when;
      if (due > 0) {
        when = QStringLiteral("  提醒 %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(due)
                            .toString(QStringLiteral("MM-dd HH:mm")));
      }
      item->setText(QStringLiteral("%1%2  %3%4")
                        .arg(done ? QStringLiteral("[x] ")
                                  : QStringLiteral("[ ] "),
                             t.value(QStringLiteral("title")).toString(),
                             self ? QStringLiteral("自建")
                                  : QStringLiteral("由 %1 分配")
                                        .arg(creator),
                             when));
    }
    if (done) {
      item->setForeground(Qt::gray);
    }
  }
  // 我派出的折叠进列表尾（只读视角：完成与否看得到，动作在对方清单）
  for (const auto& v : assigned) {
    const auto t = v.toObject();
    auto* item = new QListWidgetItem(
        QStringLiteral("[→] %1  派给 %2%3")
            .arg(t.value(QStringLiteral("title")).toString(),
                 t.value(QStringLiteral("owner")).toString(),
                 t.value(QStringLiteral("done")).toBool()
                     ? QStringLiteral("（已完成）")
                     : QString()),
        list_);
    item->setData(Qt::UserRole, t.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1,
                  t.value(QStringLiteral("done")).toBool());
    // 派出行不可在本人侧勾完成/提醒（服务端也会拒——owner 才是动作主体）
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
  }
  // 外部拉取会话缓存行（⇣ 只读镜像）缀尾，随每次重渲染对齐外部现态
  render_ext_rows();
  // 日历视图：重标到期日＋重施筛选（30s 轮询刷新重建列表，筛选态保持）
  update_day_marks();
  apply_day_filter();
  set_status(QStringLiteral("清单已刷新（%1 项）").arg(mine.size()));
}

void TaskDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（恢复用空参刷新路径重设）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : list_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString TaskDialog::status_text() const { return status_->text(); }

int TaskDialog::task_count() const { return list_->count(); }

} // namespace memex::client
