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
} // namespace memex::client
