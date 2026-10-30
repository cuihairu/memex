// R27-1 个人任务清单窗口（实现）。服务端裁决一切判权（完成归清单主人、
// 撤回归主人或分配人、分配按「同群/同部门」现查现裁）——客户端只提交
// 与展示，不自造规则。
#include "task_dialog.hpp"
#include "collab_defaults.hpp"

#include <QCalendarWidget>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QGroupBox>
#include <QGridLayout>
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
#include <QScrollArea>
#include <QSplitter>
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
  resize(1180, 760);
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
    conn_badge_->setText(
        QStringLiteral("已连接（30s 轮询到期提醒）"));
    conn_badge_->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
            .arg(QColor(Qt::green).name()));
    btn_add_->setEnabled(true);
    btn_add_ext_->setEnabled(true);
    refresh();
    client_->fetch_task_types(); // R27-6 类型清单（admin 后台可增删改）
    if (cb_lead_->isChecked()) client_->fetch_team_tasks(); // 领导视角默认开
    if (!findChild<QTimer*>("task_poll")) {
      auto* poll = new QTimer(this);
      poll->setObjectName(QStringLiteral("task_poll"));
      connect(poll, &QTimer::timeout, this, [this] {
        check_due();
        refresh();
        if (cb_lead_->isChecked()) client_->fetch_team_tasks();
      });
      poll->start(kPollMs);
    }
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            conn_badge_->setText(QStringLiteral("未连接"));
            conn_badge_->setStyleSheet(QString());
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
  // R27-6 状态切换回执（与 done 双写同源：服务端双写，读取以 status 为准）
  connect(client_, &FilesClient::task_status_set, this, [this](qint64) {
    set_status(QStringLiteral("任务状态已更新"));
    refresh();
  });
  // R27-6 类型清单来自服务端（缺省选中默认类型，原选中尽量不跳）
  connect(client_, &FilesClient::task_types_loaded, this,
          [this](const QJsonArray& types, const QString& def) {
            const QString prev = type_->currentText();
            type_->clear();
            for (const auto& v : types) {
              type_->addItem(
                  v.toObject().value(QStringLiteral("name")).toString());
            }
            const int pi = type_->findText(prev);
            if (pi >= 0) {
              type_->setCurrentIndex(pi);
            } else {
              const int di = type_->findText(def);
              type_->setCurrentIndex(di >= 0 ? di : 0);
            }
          });
  // R27-7 领导视角泳道：服务端按直属关系与 az 裁决后下发（判权不过的
  // 下属整条泳道不出现不泄露存在性）
  connect(client_, &FilesClient::team_tasks_loaded, this,
          [this](const QJsonArray& team) {
            team_cache_ = team;
            render_lead();
            if (cb_lead_->isChecked()) {
              set_status(
                  QStringLiteral("领导视角已刷新（%1 名下属）").arg(team.size()));
            }
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
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);

  // —— 连接区（原型 desktop-tasks conn-row：地址/端口/账号/口令徽章＋
  //    右上「已连接」绿点）。实现兼连接入口——字段可编辑＋连接按钮，
  //    已连接徽章常显状态；过渡消息仍走底部状态行 ——
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
  conn_badge_ = new QLabel(QStringLiteral("未连接"), this);
  conn_badge_->setObjectName(QStringLiteral("task_conn_badge"));
  conn->addWidget(host_);
  conn->addWidget(port_);
  conn->addWidget(account_box_);
  conn->addWidget(password_);
  conn->addWidget(btn_connect_);
  conn->addStretch(1);
  conn->addWidget(conn_badge_);
  layout->addLayout(conn);

  // —— 工具行（原型 tool-row）：动作按钮左，拉取外部/任务设置/模板右 ——
  auto* ops = new QHBoxLayout;
  btn_add_ = new QPushButton(QStringLiteral("添加任务"), this);
  btn_add_->setEnabled(false);
  btn_toggle_ = new QPushButton(QStringLiteral("完成 / 回退"), this);
  btn_status_ = new QPushButton(QStringLiteral("开始 / 暂停"), this);
  btn_delete_ = new QPushButton(QStringLiteral("撤回"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  btn_calendar_ = new QPushButton(QStringLiteral("日历"), this);
  btn_kanban_ = new QPushButton(QStringLiteral("看板（切列表）"), this);
  cb_lead_ = new QCheckBox(QStringLiteral("领导视角"), this);
  cb_lead_->setObjectName(QStringLiteral("task_lead_view"));
  {
    const QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
    cb_lead_->setChecked(
        s.value(QStringLiteral("task_lead_view"), true).toBool());
  }
  ops->addWidget(btn_add_);
  ops->addWidget(btn_toggle_);
  ops->addWidget(btn_status_);
  ops->addWidget(btn_delete_);
  ops->addWidget(btn_refresh_);
  ops->addWidget(btn_calendar_);
  ops->addWidget(btn_kanban_);
  ops->addWidget(cb_lead_);
  ops->addStretch(1);
  // R27-3 拉取行（原型并入 tool-row 右侧）：已配置凭据且声明 L2 的
  // provider 直拉外部任务（不经 memex 服务端，断连也可用）；⚙ 进设置面
  ops->addWidget(new QLabel(QStringLiteral("拉取外部"), this));
  pull_provider_ = new QComboBox(this);
  pull_provider_->setMinimumWidth(150);
  btn_pull_ = new QPushButton(QStringLiteral("拉取"), this);
  btn_settings_ = new QPushButton(QStringLiteral("任务设置…"), this);
  btn_templates_ = new QPushButton(QStringLiteral("模板…"), this);
  ops->addWidget(pull_provider_);
  ops->addWidget(btn_pull_);
  ops->addWidget(btn_settings_);
  ops->addWidget(btn_templates_);
  layout->addLayout(ops);

  // —— body-split：左 298px 录入列（月历/录入区/外部登记/脚注）＋右视图
  //    列（任务清单列表 / 看板 / 领导视角 / 状态行）——
  auto* body = new QHBoxLayout;
  body->setSpacing(8);

  // —— entry-col（原型 aside.entry-col 固定 298px）——
  auto* entry = new QWidget(this);
  entry->setFixedWidth(298);
  auto* entry_lay = new QVBoxLayout(entry);
  entry_lay->setContentsMargins(8, 8, 8, 8);
  entry_lay->setSpacing(10);

  // 月历（到期日标注 · 点日过滤）：默认收起，「日历」按钮开关；图例常显
  auto* cal_box = new QGroupBox(
      QStringLiteral("月历（到期日标注 · 点日过滤）"), entry);
  {
    auto* cbl = new QVBoxLayout(cal_box);
    calendar_ = new QCalendarWidget(cal_box);
    calendar_->setObjectName(QStringLiteral("task_calendar"));
    calendar_->setGridVisible(true);
    calendar_->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
    calendar_->setVisible(false);
    cbl->addWidget(calendar_);
    auto* legend = new QLabel(
        QStringLiteral("橙底＝有到期任务 · 橙实底＝今天 · 再点一次清除过滤"),
        cal_box);
    legend->setWordWrap(true); // 298px 列宽内不断行截断
    cbl->addWidget(legend);
  }
  entry_lay->addWidget(cal_box);

  // 录入区（原型 aside 录入区 fgroup）：标题/备注/截止/分配给/类型/
  // 开始/提醒纵向成行
  auto* form_box = new QGroupBox(QStringLiteral("录入区"), entry);
  {
    auto* fl = new QGridLayout(form_box);
    fl->setVerticalSpacing(7);
    fl->setHorizontalSpacing(7);
    auto mk_label = [form_box](const QString& t) {
      auto* l = new QLabel(t, form_box);
      l->setMinimumWidth(64);
      return l;
    };
    int row = 0;
    title_ = new QLineEdit(form_box);
    title_->setPlaceholderText(QStringLiteral("任务标题"));
    fl->addWidget(mk_label(QStringLiteral("任务标题")), row, 0);
    fl->addWidget(title_, row, 1, 1, 2);
    ++row;
    note_ = new QLineEdit(form_box);
    note_->setPlaceholderText(QStringLiteral("可空"));
    fl->addWidget(mk_label(QStringLiteral("备注")), row, 0);
    fl->addWidget(note_, row, 1, 1, 2);
    ++row;
    due_ = new QDateTimeEdit(form_box);
    due_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    due_->setCalendarPopup(true);
    due_->setDateTime(QDateTime::currentDateTime());
    fl->addWidget(mk_label(QStringLiteral("截止（deadline）")), row, 0);
    fl->addWidget(due_, row, 1, 1, 2);
    ++row;
    assignee_ = new QComboBox(form_box);
    assignee_->setEditable(true);
    assignee_->addItem(QStringLiteral("（自己）"));
    fl->addWidget(mk_label(QStringLiteral("分配给")), row, 0);
    fl->addWidget(assignee_, row, 1, 1, 2);
    ++row;
    // R27-6 类型：清单从服务端拉取（缺省=默认类型）；下发说明行常显
    type_ = new QComboBox(form_box);
    type_->setObjectName(QStringLiteral("task_type"));
    fl->addWidget(mk_label(QStringLiteral("类型")), row, 0);
    fl->addWidget(type_, row, 1, 1, 2);
    ++row;
    fl->addWidget(new QLabel(QStringLiteral("类型清单由服务端下发"
                                            "（管理台可增删改）"),
                             form_box),
                  row, 0, 1, 3);
    ++row;
    // R27-6 开始时间：勾选才生效（0=未设）
    start_on_ = new QCheckBox(QStringLiteral("开始"), form_box);
    start_on_->setObjectName(QStringLiteral("task_start_on"));
    start_ = new QDateTimeEdit(form_box);
    start_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    start_->setCalendarPopup(true);
    start_->setDateTime(QDateTime::currentDateTime());
    start_->setEnabled(false);
    fl->addWidget(start_on_, row, 0);
    fl->addWidget(start_, row, 1, 1, 2);
    ++row;
    connect(start_on_, &QCheckBox::toggled, start_,
            &QDateTimeEdit::setEnabled);
    // R27-6 提醒开关同一处：开始到点提醒＋deadline 到期提醒（默认开，
    // QSettings 持久；提醒仍走通知中心现口径，回执只提醒一次）
    cb_start_remind_ = new QCheckBox(QStringLiteral("开始到点提醒"), form_box);
    cb_start_remind_->setObjectName(QStringLiteral("task_remind_start"));
    cb_due_remind_ = new QCheckBox(QStringLiteral("截止到期提醒"), form_box);
    cb_due_remind_->setObjectName(QStringLiteral("task_remind_due"));
    const QSettings rem(QStringLiteral("memex"), QStringLiteral("collab"));
    cb_start_remind_->setChecked(
        rem.value(QStringLiteral("task_remind_start"), true).toBool());
    cb_due_remind_->setChecked(
        rem.value(QStringLiteral("task_remind_due"), true).toBool());
    connect(cb_start_remind_, &QCheckBox::toggled, this, [this](bool on) {
      QSettings(QStringLiteral("memex"), QStringLiteral("collab"))
          .setValue(QStringLiteral("task_remind_start"), on);
    });
    connect(cb_due_remind_, &QCheckBox::toggled, this, [this](bool on) {
      QSettings(QStringLiteral("memex"), QStringLiteral("collab"))
          .setValue(QStringLiteral("task_remind_due"), on);
    });
    auto* remind_row = new QHBoxLayout;
    remind_row->addWidget(cb_start_remind_);
    remind_row->addWidget(cb_due_remind_);
    remind_row->addStretch(1);
    fl->addLayout(remind_row, row, 0, 1, 3);
    ++row;
    fl->setColumnStretch(1, 1);
  }
  entry_lay->addWidget(form_box);

  // 外部任务登记（R27-2）：provider（内置 L1 预设＋自定义模板）＋键
  //（「project#键」或完整链接）。登记前本地解析 detail URL——解析不出
  // 不发网不落库。
  auto* ext_box = new QGroupBox(QStringLiteral("外部任务登记"), entry);
  {
    auto* el = new QGridLayout(ext_box);
    el->setVerticalSpacing(7);
    el->setHorizontalSpacing(7);
    ext_provider_ = new QComboBox(ext_box);
    refresh_ext_combo();
    ext_key_ = new QLineEdit(ext_box);
    ext_key_->setPlaceholderText(
        QStringLiteral("仓库/站点#键 或完整链接（如 org/repo#12）"));
    btn_add_ext_ = new QPushButton(
        QStringLiteral("登记外部任务（本地先解析跳转链接）"), ext_box);
    btn_add_ext_->setEnabled(false);
    el->addWidget(new QLabel(QStringLiteral("Provider"), ext_box), 0, 0);
    el->addWidget(ext_provider_, 0, 1);
    el->addWidget(new QLabel(QStringLiteral("键 / 链接"), ext_box), 1, 0);
    el->addWidget(ext_key_, 1, 1);
    el->addWidget(btn_add_ext_, 2, 0, 1, 2);
    el->setColumnStretch(1, 1);
  }
  entry_lay->addWidget(ext_box);

  // 脚注（原型 aside 尾部说明）
  auto* footnote = new QLabel(
      QStringLiteral("⇣ 行＝服务端拉取的只读镜像；🌐 行＝双击跳外部详情；"
                     "勾选外部条目＝本地 memex 标记（回写属 L3）。凭据在"
                     "「任务设置」里加密保存，本窗口零凭据。"),
      entry);
  footnote->setWordWrap(true);
  entry_lay->addWidget(footnote);
  entry_lay->addStretch(1);
  body->addWidget(entry);

  // —— view-col（原型 section.view-col）——
  auto* view = new QWidget(this);
  auto* view_lay = new QVBoxLayout(view);
  view_lay->setContentsMargins(8, 8, 8, 8);
  view_lay->setSpacing(8);

  // 任务清单 sec-head：计数（我的/派给我/外部）＋只看未完成开关
  list_head_ = new QWidget(view);
  {
    auto* hl = new QHBoxLayout(list_head_);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->addWidget(new QLabel(QStringLiteral("任务清单"), list_head_));
    counts_ = new QLabel(QStringLiteral("我的 0 · 派给我 0 · 外部 0"),
                         list_head_);
    hl->addWidget(counts_);
    hl->addStretch(1);
    cb_only_open_ = new QCheckBox(QStringLiteral("只看未完成"), list_head_);
    cb_only_open_->setObjectName(QStringLiteral("task_only_open"));
    hl->addWidget(cb_only_open_);
  }
  view_lay->addWidget(list_head_);

  // 任务列表
  list_ = new QListWidget(view);
  list_->setAlternatingRowColors(true);
  view_lay->addWidget(list_, 1);

  // 看板 sec-head（列表↔看板互斥；toggle_kanban 切两头显隐）
  kanban_head_ = new QWidget(view);
  kanban_head_->setVisible(false);
  {
    auto* hl = new QHBoxLayout(kanban_head_);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->addWidget(new QLabel(QStringLiteral("看板"), kanban_head_));
    hl->addWidget(new QLabel(QStringLiteral("同一批任务的另一视图 · "
                                            "双击卡片推进状态"),
                             kanban_head_));
    hl->addStretch(1);
    hl->addWidget(new QLabel(QStringLiteral("逾期红 · 3 天内到期金黄"
                                            "（task_due_warn_days 可调）"),
                             kanban_head_));
  }
  view_lay->addWidget(kanban_head_);

  // R27-6 看板：同一批任务数据的另一视图（不新建数据模型/通道），与
  // 列表并存切换，默认隐藏。三列=待办/进行中/完成（读 status 语义）。
  kanban_ = new QSplitter(Qt::Horizontal, view);
  kanban_->setObjectName(QStringLiteral("task_kanban"));
  auto mkcol = [this](const QString& title, const QString& object) {
    auto* box = new QWidget(kanban_);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(new QLabel(title, box));
    auto* lw = new QListWidget(box);
    lw->setObjectName(object);
    lw->setAlternatingRowColors(true);
    lay->addWidget(lw, 1);
    kanban_->addWidget(box);
    return lw;
  };
  kanban_todo_ = mkcol(QStringLiteral("待办"), QStringLiteral("kanban_todo"));
  kanban_doing_ =
      mkcol(QStringLiteral("进行中"), QStringLiteral("kanban_doing"));
  kanban_done_ = mkcol(QStringLiteral("完成"), QStringLiteral("kanban_done"));
  kanban_->setVisible(false);
  view_lay->addWidget(kanban_, 1);

  // R27-7 领导视角：直属下属任务泳道（A 泳道式=每下属一条，一眼看压
  // 力分布；负荷统计口径写死 未完成/已逾期/7 天内到期）。默认开（个人
  // 可关，QSettings task_lead_view）；无下属或判权全过滤时自动隐藏。
  // 说明头随泳道同显隐（原型 .lead 说明行）
  lead_head_ = new QLabel(
      QStringLiteral("领导视角 · 仅直属下级一层（不跨级）；关闭后此区隐藏"
                     "并持久化"),
      view);
  lead_head_->setVisible(false);
  view_lay->addWidget(lead_head_);
  lead_scroll_ = new QScrollArea(view);
  lead_scroll_->setObjectName(QStringLiteral("task_lead_pane"));
  lead_scroll_->setWidgetResizable(true);
  lead_scroll_->setMinimumHeight(170);
  lead_scroll_->setVisible(false);
  view_lay->addWidget(lead_scroll_);

  // 状态行（原型 .statusline）：动态状态＋两条固定说明
  auto* status_row = new QHBoxLayout;
  status_ = new QLabel(view);
  status_row->addWidget(status_, 1);
  status_row->addWidget(new QLabel(
      QStringLiteral("任务类型由服务端配置（管理台可增删改）"), view));
  status_row->addWidget(new QLabel(
      QStringLiteral("server-side pull 默认关；开启后 ⇣ 镜像由服务端"
                     "长连接推送"),
      view));
  view_lay->addLayout(status_row);

  body->addWidget(view, 1);
  layout->addLayout(body, 1);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(resolve_conn_host(host_->text()), port_->text().toUShort(),
               account_box_->text(), password_->text());
  });
  connect(btn_add_, &QPushButton::clicked, this, [this] {
    const QString who = assignee_->currentText().trimmed();
    add_task(title_->text().trimmed(), note_->text().trimmed(),
             due_->dateTime().toMSecsSinceEpoch(),
             who.startsWith(QStringLiteral("（")) ? QString() : who,
             type_->currentText(),
             start_on_->isChecked()
                 ? start_->dateTime().toMSecsSinceEpoch()
                 : 0);
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
  // R27-6 状态按钮：待办↔进行中（完成态走 完成/回退，服务端双写）
  connect(btn_status_, &QPushButton::clicked, this, [this] {
    const auto* item = list_->currentItem();
    if (item == nullptr || item->data(Qt::UserRole + 6).toBool()) {
      set_status(QStringLiteral("先选中一条本端任务"), true);
      return;
    }
    set_selected_status(item->data(Qt::UserRole + 10).toString() ==
                                QStringLiteral("进行中")
                            ? QStringLiteral("待办")
                            : QStringLiteral("进行中"));
  });
  connect(btn_kanban_, &QPushButton::clicked, this,
          [this] { toggle_kanban(); });
  // 领导视角开关：持久化（默认开）＋开启即拉泳道
  connect(cb_lead_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings(QStringLiteral("memex"), QStringLiteral("collab"))
        .setValue(QStringLiteral("task_lead_view"), on);
    if (on && is_connected()) client_->fetch_team_tasks();
    render_lead();
  });
  // 只看未完成：隐藏已完成行（与日历筛选叠加；不持久，开窗默认为关）
  connect(cb_only_open_, &QCheckBox::toggled, this,
          [this](bool) { apply_day_filter(); });
  // 选中态同步状态按钮文案（开始/暂停）
  connect(list_, &QListWidget::itemSelectionChanged, this, [this] {
    const auto* item = list_->currentItem();
    btn_status_->setText(item != nullptr &&
                                 item->data(Qt::UserRole + 10).toString() ==
                                     QStringLiteral("进行中")
                             ? QStringLiteral("暂停")
                             : QStringLiteral("开始"));
  });
  // 看板卡片双击=推进状态（待办→进行中→完成→待办循环）；看板只收
  // 本端数据（status 判权 owner-only，派出行不进看板）
  connect(kanban_todo_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem* it) {
            const qint64 id =
                it ? it->data(Qt::UserRole).toLongLong() : -1;
            if (id > 0) {
              client_->set_task_status(id, QStringLiteral("进行中"));
            }
          });
  connect(kanban_doing_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem* it) {
            const qint64 id =
                it ? it->data(Qt::UserRole).toLongLong() : -1;
            if (id > 0) {
              client_->set_task_status(id, QStringLiteral("完成"));
            }
          });
  connect(kanban_done_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem* it) {
            const qint64 id =
                it ? it->data(Qt::UserRole).toLongLong() : -1;
            if (id > 0) {
              client_->set_task_status(id, QStringLiteral("待办"));
            }
          });
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
  const bool only_open = cb_only_open_ != nullptr && cb_only_open_->isChecked();
  for (int i = 0; i < list_->count(); ++i) {
    auto* it = list_->item(i);
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    const bool hit =
        day_filter_.isValid() && due > 0 &&
        QDateTime::fromMSecsSinceEpoch(due).date() == day_filter_;
    // 筛选态只留命中日；无到期数据行（派出/⇣/未设提醒）一并隐藏。
    // 只看未完成：done 行（UserRole+1 或 status=完成）隐藏，与日历叠加
    const bool done = it->data(Qt::UserRole + 1).toBool() ||
                      it->data(Qt::UserRole + 10).toString() ==
                          QStringLiteral("完成");
    it->setHidden((day_filter_.isValid() && !hit) || (only_open && done));
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
                          qint64 due_ms, const QString& assignee,
                          const QString& type, qint64 start_ms) {
  if (title.isEmpty()) {
    set_status(QStringLiteral("标题不能为空"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->create_task(title, note, due_ms, assignee, QString(), QString(),
                       type, start_ms);
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

// R27-6 状态切换：待办/进行中/完成（服务端判权 owner-only 并双写 done，
// 读取以 status 为准——客户端只提交与展示）
bool TaskDialog::set_selected_status(const QString& status) {
  if (selected_row_pulled()) {
    set_status(QStringLiteral("外部拉取行只读"), true);
    return false;
  }
  const qint64 id = selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  client_->set_task_status(id, status);
  return true;
}

// —— R27-6 看板（与列表同源同批数据，切换即重渲染）——

void TaskDialog::toggle_kanban() {
  const bool show = !kanban_->isVisible();
  kanban_->setVisible(show);
  list_->setVisible(!show);
  if (list_head_ != nullptr) list_head_->setVisible(!show);
  if (kanban_head_ != nullptr) kanban_head_->setVisible(show);
  btn_kanban_->setText(show ? QStringLiteral("列表（切看板）")
                            : QStringLiteral("看板（切列表）"));
  if (show) render_kanban();
}

void TaskDialog::render_kanban() {
  // 卡片=标题 ·类型＋开始/截止；逾期红、临近黄（阈值 QSettings
  // task_due_warn_days 缺省 3 天），完成灰
  const QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const qint64 warn =
      settings.value(QStringLiteral("task_due_warn_days"), 3).toInt() *
      86400000LL;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  kanban_todo_->clear();
  kanban_doing_->clear();
  kanban_done_->clear();
  for (const auto& v : mine_cache_) {
    const auto t = v.toObject();
    const bool done = t.value(QStringLiteral("done")).toBool();
    const QString status = t.value(QStringLiteral("status")).toString();
    const qint64 start = static_cast<qint64>(
        t.value(QStringLiteral("start_ms")).toDouble());
    const qint64 due =
        static_cast<qint64>(t.value(QStringLiteral("due_ms")).toDouble());
    QString extra;
    if (start > 0) {
      extra += QStringLiteral("  开始 %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(start)
                            .toString(QStringLiteral("MM-dd HH:mm")));
    }
    if (due > 0) {
      extra += QStringLiteral("  截止 %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(due)
                            .toString(QStringLiteral("MM-dd HH:mm")));
    }
    auto* card = new QListWidgetItem(
        QStringLiteral("%1 ·%2%3")
            .arg(t.value(QStringLiteral("title")).toString(),
                 t.value(QStringLiteral("type")).toString(), extra));
    card->setData(Qt::UserRole,
                  t.value(QStringLiteral("id")).toDouble());
    QListWidget* col = kanban_todo_;
    if (done || status == QStringLiteral("完成")) {
      col = kanban_done_;
      card->setForeground(Qt::gray);
    } else if (status == QStringLiteral("进行中")) {
      col = kanban_doing_;
    }
    if (!done && due > 0 && due <= now) {
      card->setForeground(Qt::red); // 逾期
    } else if (!done && due > 0 && due - now <= warn) {
      card->setForeground(QColor(QStringLiteral("#b8860b"))); // 临近暗金黄
    }
    col->addItem(card);
  }
}

int TaskDialog::kanban_count(int col) const {
  const QListWidget* c = col == 0 ? kanban_todo_
                         : col == 1 ? kanban_doing_
                                    : kanban_done_;
  return c != nullptr ? c->count() : -1;
}

QStringList TaskDialog::task_type_names() const {
  QStringList out;
  if (type_ != nullptr) {
    for (int i = 0; i < type_->count(); ++i) out << type_->itemText(i);
  }
  return out;
}

// —— R27-7 领导视角（只读泳道；动作仍归下属本人清单）——

int TaskDialog::lead_lane_count() const {
  return lead_scroll_->isVisible() ? team_cache_.size() : 0;
}

void TaskDialog::render_lead() {
  // 泳道=每直属下属一条（服务端已按直属关系与 az 裁决过滤）；标头带
  // 负荷统计三数，卡片带类型与开始/截止，逾期红/临近黄/完成灰
  const bool show = cb_lead_->isChecked() && !team_cache_.isEmpty();
  lead_scroll_->setVisible(show);
  if (lead_head_ != nullptr) lead_head_->setVisible(show);
  if (!show) return;
  const QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const qint64 warn =
      settings.value(QStringLiteral("task_due_warn_days"), 3).toInt() *
      86400000LL;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  auto* lanes = new QWidget;
  auto* lay = new QHBoxLayout(lanes);
  lay->setContentsMargins(0, 0, 0, 0);
  for (const auto& v : team_cache_) {
    const auto lane = v.toObject();
    const auto loads = lane.value(QStringLiteral("loads")).toObject();
    const int open = loads.value(QStringLiteral("open")).toInt();
    const int overdue = loads.value(QStringLiteral("overdue")).toInt();
    const int due_soon = loads.value(QStringLiteral("due_soon")).toInt();
    auto* box = new QGroupBox(
        QStringLiteral("%1（未完成 %2 · 逾期 %3 · 7 天内到期 %4）")
            .arg(lane.value(QStringLiteral("account")).toString(),
                 QString::number(open), QString::number(overdue),
                 QString::number(due_soon)),
        lanes);
    auto* vlay = new QVBoxLayout(box);
    auto* cards = new QListWidget(box);
    for (const auto& tv : lane.value(QStringLiteral("tasks")).toArray()) {
      const auto t = tv.toObject();
      const bool done = t.value(QStringLiteral("done")).toBool() ||
                        t.value(QStringLiteral("status")).toString() ==
                            QStringLiteral("完成");
      const qint64 start = static_cast<qint64>(
          t.value(QStringLiteral("start_ms")).toDouble());
      const qint64 due =
          static_cast<qint64>(t.value(QStringLiteral("due_ms")).toDouble());
      QString extra;
      if (start > 0) {
        extra += QStringLiteral("  开始 %1")
                     .arg(QDateTime::fromMSecsSinceEpoch(start)
                              .toString(QStringLiteral("MM-dd HH:mm")));
      }
      if (due > 0) {
        extra += QStringLiteral("  截止 %1")
                     .arg(QDateTime::fromMSecsSinceEpoch(due)
                              .toString(QStringLiteral("MM-dd HH:mm")));
      }
      auto* card = new QListWidgetItem(
          QStringLiteral("%1 ·%2%3%4")
              .arg(t.value(QStringLiteral("title")).toString(),
                   t.value(QStringLiteral("type")).toString(),
                   t.value(QStringLiteral("status")).toString() ==
                           QStringLiteral("进行中")
                       ? QStringLiteral("（进行中）")
                       : QString(),
                   extra));
      if (done) {
        card->setForeground(Qt::gray);
      } else if (due > 0 && due <= now) {
        card->setForeground(Qt::red); // 逾期
      } else if (due > 0 && due - now <= warn) {
        card->setForeground(QColor(QStringLiteral("#b8860b"))); // 临近
      }
      cards->addItem(card);
    }
    vlay->addWidget(cards);
    lay->addWidget(box);
  }
  lay->addStretch(1);
  lead_scroll_->setWidget(lanes); // 旧泳道容器由 QScrollArea 回收
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
  // R27-6 两种提醒各给开关（默认开，QSettings 持久），同一处管理
  const QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const bool start_on =
      settings.value(QStringLiteral("task_remind_start"), true).toBool();
  const bool due_on =
      settings.value(QStringLiteral("task_remind_due"), true).toBool();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (int i = 0; i < list_->count(); ++i) {
    const auto* it = list_->item(i);
    if (it->data(Qt::UserRole + 6).toBool()) continue; // ⇣ 只读镜像不提醒
    if (it->data(Qt::UserRole + 1).toBool()) continue; // 已完成不提醒
    const qint64 id = it->data(Qt::UserRole).toLongLong();
    // deadline 到期提醒（回执只提醒一次）
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    const qint64 reminded = it->data(Qt::UserRole + 3).toLongLong();
    if (due_on && due > 0 && due <= now && reminded == 0) {
      NotificationCenter::instance().on_notice(
          QStringLiteral("任务提醒"), it->data(Qt::UserRole + 4).toString(),
          QStringLiteral("任务已到期待办"), /*IMPORTANT*/ 2, QString(), now,
          QStringLiteral("task-%1-%2").arg(id).arg(now));
      client_->mark_task_reminded(id);
    }
    // 开始到点提醒（R27-6；start_reminded_ms 回执独立，只提醒一次）
    const qint64 start = it->data(Qt::UserRole + 7).toLongLong();
    const qint64 srem = it->data(Qt::UserRole + 8).toLongLong();
    if (start_on && start > 0 && start <= now && srem == 0) {
      NotificationCenter::instance().on_notice(
          QStringLiteral("任务提醒"), it->data(Qt::UserRole + 4).toString(),
          QStringLiteral("任务已到开始时间"), /*IMPORTANT*/ 2, QString(), now,
          QStringLiteral("task-start-%1-%2").arg(id).arg(now));
      client_->mark_task_reminded(id, QStringLiteral("start"));
    }
  }
}

qint64 TaskDialog::selected_id() const {
  const auto* item = list_->currentItem();
  return item ? item->data(Qt::UserRole).toLongLong() : -1;
}

void TaskDialog::populate(const QJsonArray& mine, const QJsonArray& assigned) {
  mine_cache_ = mine;   // 看板同源数据（toggle_kanban 重渲染用）
  assigned_cache_ = assigned;
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
    // R27-6 开始/回执（start）/类型/状态（roles +7..+10）
    item->setData(Qt::UserRole + 7,
                  static_cast<qint64>(
                      t.value(QStringLiteral("start_ms")).toDouble()));
    item->setData(Qt::UserRole + 8,
                  static_cast<qint64>(
                      t.value(QStringLiteral("start_reminded_ms"))
                          .toDouble()));
    item->setData(Qt::UserRole + 9,
                  t.value(QStringLiteral("type")).toString());
    item->setData(Qt::UserRole + 10,
                  t.value(QStringLiteral("status")).toString());
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
      const qint64 start = item->data(Qt::UserRole + 7).toLongLong();
      if (start > 0) {
        when += QStringLiteral("  开始 %1")
                    .arg(QDateTime::fromMSecsSinceEpoch(start)
                             .toString(QStringLiteral("MM-dd HH:mm")));
      }
      const QString status = item->data(Qt::UserRole + 10).toString();
      item->setText(QStringLiteral("%1%2 ·%3  %4%5%6")
                        .arg(done ? QStringLiteral("[x] ")
                                  : QStringLiteral("[ ] "),
                             t.value(QStringLiteral("title")).toString(),
                             item->data(Qt::UserRole + 9).toString(),
                             self ? QStringLiteral("自建")
                                  : QStringLiteral("由 %1 分配")
                                        .arg(creator),
                             status == QStringLiteral("进行中")
                                 ? QStringLiteral("（进行中）")
                                 : QString(),
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
  // 任务清单 sec-head 计数（我的/派给我/外部拉取镜像）
  int ext_n = 0;
  for (auto it = ext_cache_.constBegin(); it != ext_cache_.constEnd(); ++it) {
    ext_n += it.value().size();
  }
  if (counts_ != nullptr) {
    counts_->setText(QStringLiteral("我的 %1 · 派给我 %2 · 外部 %3")
                         .arg(static_cast<int>(mine.size()))
                         .arg(static_cast<int>(assigned.size()))
                         .arg(ext_n));
  }
  // R27-6 看板同批数据重渲染（隐藏时也保持最新，切换即现）＋状态按钮复位
  render_kanban();
  btn_status_->setText(QStringLiteral("开始 / 暂停"));
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
