// 主窗装配与生命周期（LAY-002 拆分）：构造、apply_brand 整窗换牌、
// build_ui 全界面装配（侧栏/聊天区/菜单/整屏视图骨架）。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QCursor>
#include <QFileDialog>
#include <QIcon>
#include <QMenuBar>
#include <QNetworkInterface>
#include <QScrollArea>

#include "file_assistant.hpp"
#include "group_memo_dialog.hpp"
#include "group_ci_dialog.hpp"
#include "group_pack_dialog.hpp"
#include "group_server_dialog.hpp"
#include "task_dialog.hpp"
#include "approval_dialog.hpp"
#include "report_dialog.hpp"
#include "group_tools_dialog.hpp"
#include "brand_settings_page.hpp"
#include "shortcut_settings.hpp"
#include "assist_dialog.hpp"
#include "audit_dialog.hpp"
#include "archive_console.hpp"
#include "office_map_dialog.hpp"
#include "message_search_dialog.hpp"
#include <QTimer>
#include <QShortcut>
#include <QSystemTrayIcon>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QUrl>
#include "notify_center.hpp"
#include "brand_kit.hpp"


#include "theme.hpp"
#include "theme_settings_page.hpp"
namespace memex::client {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Memex"));
  setMinimumSize(320, 420); // 下限＝列表态最小可读（rail 60＋列表列 260）
  resize(1280, 800); // 先占位；build_ui 后再落一次（布局 sizeHint 会改写）

  build_ui();
  // build_ui 之后须再落一次（布局激活会按 sizeHint 改写窗尺寸，无 show 时）
  resize(1280, 800);
  // 布局令＋Telegram 机制（尺寸随布局态绑定）：启动无会话必然坍缩到列表态
  // 尺寸（宽 320）；点好友经 open_chat 展开到会话态宽。列表态存档由
  // restore_window_geometry 按态恢复——会话态宽窗不盖回无会话态
  close_chat();

  wire_engines();
  direct_engine_.start();
  // 双态共用同一份本地库：协作消息与直连消息同库，界面按 source 合并展示
  collab_engine_.attach_store(direct_engine_.store());
  // 「盯」提醒：启动装载（重启后盯仍有效）＋巡检定时（未读超阈值提醒
  // 一次、超时静默过期、离线期已读补触发）
  load_watches();
  watch_timer_ = new QTimer(this);
  watch_timer_->setInterval(30 * 1000);
  connect(watch_timer_, &QTimer::timeout, this, &MainWindow::sweep_watches);
  watch_timer_->start();
  sweep_watches();
  wire_collab();
  // T4.4 截图与标注：确认后发送（取消／失败仅提示）
  connect(&screenshot_tool_, &ScreenshotTool::confirmed, this,
          &MainWindow::on_screenshot_confirmed);
  connect(&screenshot_tool_, &ScreenshotTool::cancelled, this, [] {
    // 取消无需提示（用户主动放弃）
  });
  connect(&screenshot_tool_, &ScreenshotTool::failed, this,
          [this](const QString& reason) { show_status(reason); });

  // 离开锁屏（用户令 2026-10-08 ④）：全局输入事件喂计时器，超时即锁
  idle_timer_ = new QTimer(this);
  idle_timer_->setSingleShot(true);
  connect(idle_timer_, &QTimer::timeout, this,
          &MainWindow::maybe_lock_screen);
  qApp->installEventFilter(this);
  apply_away_lock_settings();
  apply_net_settings(); // 网段黑名单（QSettings 缺省=关=不拦）

  refresh_devices();
  show_status(QStringLiteral("就绪"));
  update_banner();

  // R19 · T4.9：主题切换即时生效——令牌变了就重刷控件样式与聊天区富文本
  connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
          [this](const QString&) {
            apply_theme_styles();
            setWindowIcon(brand_icon());
            refresh_devices(); // 列表行控件按新令牌重建（行内样式非全局 QSS）
          });

  // 品牌物料：缓存先行（离线也有牌）＋拉取应用后整窗换牌（设计稿
  // 品牌物料.md §4——brand_applied 驱动，默认标兜底没配就不变）
  BrandKit::instance().load_cache();
  connect(&BrandKit::instance(), &BrandKit::brand_applied, this,
          &MainWindow::apply_brand);
  apply_brand();

  // 恢复上次窗口几何：按当前布局态恢复（启动＝列表态，只取列表态存档；
  // 会话态存档留给 open_chat，不盖回无会话态——2026-10-15 缺陷修复）。
  // 列表态存档宽不超 320（列表态右缘上限＝320），restore 不被 maximumWidth
  // 钳掉；上限复归在 restore 出口统一做，不在此先放开（放开＝新装环境
  // 上限恒失守）
  restore_window_geometry();
}

void MainWindow::apply_brand() {
  const BrandKit& kit = BrandKit::instance();
  setWindowTitle(kit.window_title());
  setWindowIcon(kit.window_icon());
  if (tray_) tray_->setIcon(kit.window_icon());
  if (brand_row_ != nullptr) {
    const bool has = kit.branded();
    brand_row_->setVisible(has);
    if (has) {
      if (!kit.logo().isNull()) {
        brand_logo_->setPixmap(
            kit.logo().scaled(20, 20, Qt::KeepAspectRatio,
                              Qt::SmoothTransformation));
      }
      brand_name_->setText(kit.company_name());
    }
  }
}

void MainWindow::build_ui() {
  // —— 菜单：协作态登录／登出（不重启切换形态，T2.4）；群聊（T4.1）——
  auto* collab_menu = menuBar()->addMenu(QStringLiteral("协作"));
  auto* act_login = collab_menu->addAction(QStringLiteral("登录协作态…"));
  auto* act_org = collab_menu->addAction(QStringLiteral("组织架构…"));
  act_collab_logout_ = collab_menu->addAction(QStringLiteral("登出（回到直连态）"));
  act_collab_logout_->setEnabled(false);
  collab_menu->addSeparator();
  auto* act_group_new =
      collab_menu->addAction(QStringLiteral("新建群聊…"));
  auto* act_dgroup_new =
      collab_menu->addAction(QStringLiteral("新建临时群（直连，不进归档）…"));
  connect(act_login, &QAction::triggered, this,
          &MainWindow::show_collab_login_dialog);
  connect(act_org, &QAction::triggered, this, &MainWindow::enter_org_view);
  connect(act_collab_logout_, &QAction::triggered, this,
          &MainWindow::logout_collab);
  connect(act_group_new, &QAction::triggered, this,
          [this] { create_group_dialog(); });
  connect(act_dgroup_new, &QAction::triggered, this, [this] { dgroup_dialog(); });
  // R23-3 文件助手（自己↔自己）：备忘录 + 收件箱统一收件，独立文件面会话
  auto* act_files = collab_menu->addAction(QStringLiteral("文件助手…"));
  connect(act_files, &QAction::triggered, this,
          &MainWindow::show_file_assistant);

  // R27-1 个人任务清单：一级菜单入口（设计拍板：与会话/通讯录同级常驻，
  // 不埋设置页；独立文件面会话窗口）
  auto* task_menu = menuBar()->addMenu(QStringLiteral("任务"));
  auto* act_tasks = task_menu->addAction(QStringLiteral("任务清单…"));
  connect(act_tasks, &QAction::triggered, this, [this] {
    if (!task_dialog_) {
      task_dialog_ = new TaskDialog(this);
      // 不设 WA_DeleteOnClose：关窗=hide 后台驻留——30s 到期轮询与
      // 文件面连接持续，提醒经常驻通知中心照发（关窗后台提醒常驻化）
    }
    // 分配候选=组织架构账号（未拉到组织架构时候选只剩「自己」）
    const QJsonDocument od = QJsonDocument::fromJson(last_org_json_.toUtf8());
    QStringList accounts;
    if (od.isObject()) {
      for (const auto& m : od.object().value(QStringLiteral("members"))
                               .toArray()) {
        accounts << m.toObject()
                        .value(QStringLiteral("account"))
                        .toString();
      }
    }
    task_dialog_->set_assignees(accounts);
    task_dialog_->show();
    task_dialog_->raise();
    task_dialog_->activateWindow();
  });
  // 二期·审批（请假起步）：同组常驻入口；判权全在服务端，窗口无候选面
  auto* act_approvals = task_menu->addAction(QStringLiteral("审批…"));
  connect(act_approvals, &QAction::triggered, this, [this] {
    if (!approval_dialog_) {
      approval_dialog_ = new ApprovalDialog(this);
      approval_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    approval_dialog_->show();
    approval_dialog_->raise();
    approval_dialog_->activateWindow();
  });
  // 二期·日报周报：写日报/看下属（直属上级）；判权全在服务端
  auto* act_reports = task_menu->addAction(QStringLiteral("日报周报…"));
  connect(act_reports, &QAction::triggered, this, [this] {
    if (!report_dialog_) {
      report_dialog_ = new ReportDialog(this);
      report_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    report_dialog_->show();
    report_dialog_->raise();
    report_dialog_->activateWindow();
  });
  // 二期·群工具三件（投票/接龙/群任务）：判权全在服务端，窗口只提交与展示
  auto* act_group_tools = task_menu->addAction(QStringLiteral("群工具…"));
  connect(act_group_tools, &QAction::triggered, this, [this] {
    if (!group_tools_dialog_) {
      group_tools_dialog_ = new GroupToolsDialog(this);
      group_tools_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    group_tools_dialog_->show();
    group_tools_dialog_->raise();
    group_tools_dialog_->activateWindow();
  });
  // 「盯」列表（实现单修订 2026-10-14）：入口看到「我在盯哪些」，逐条可取消
  auto* act_watch_list = task_menu->addAction(QStringLiteral("盯列表…"));
  connect(act_watch_list, &QAction::triggered, this,
          [this] { open_watch_list(); });
  // 二期·会话审计：归档在线检索＋查阅日志（auditor 持证；被拒服务端留痕）
  auto* act_audit = collab_menu->addAction(QStringLiteral("会话审计…"));
  connect(act_audit, &QAction::triggered, this, [this] {
    if (!audit_dialog_) {
      audit_dialog_ = new AuditDialog(this);
      audit_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    audit_dialog_->show();
    audit_dialog_->raise();
    audit_dialog_->activateWindow();
  });
  // 管理台·归档检索台（原型 desktop-search）：auditor 整窗控制台——条件
  // 检索＋五列命中表＋上下文/撤回事件/导出留证（每次查阅服务端留痕）
  auto* act_console = collab_menu->addAction(QStringLiteral("归档检索台…"));
  connect(act_console, &QAction::triggered, this, [this] {
    if (!archive_console_) {
      archive_console_ = new ArchiveConsole(this);
      archive_console_->setAttribute(Qt::WA_DeleteOnClose);
    }
    archive_console_->show();
    archive_console_->raise();
    archive_console_->activateWindow();
  });
  // 二期·办公室位置图：抽象平面＋拖拽落位（编辑归 org-admin，服务端现裁）
  auto* act_office = collab_menu->addAction(QStringLiteral("办公室位置图…"));
  connect(act_office, &QAction::triggered, this, [this] {
    if (!office_dialog_) {
      office_dialog_ = new OfficeMapDialog(this);
      office_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    office_dialog_->show();
    office_dialog_->raise();
    office_dialog_->activateWindow();
  });
  // 二期·远程协助：受控方显式确认＋过程持续可见（consent/audit 红线在
  // 服务端，客户端只做门面）
  auto* act_assist = collab_menu->addAction(QStringLiteral("远程协助…"));
  connect(act_assist, &QAction::triggered, this, [this] {
    if (!assist_dialog_) {
      assist_dialog_ = new AssistDialog(this);
      assist_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    assist_dialog_->show();
    assist_dialog_->raise();
    assist_dialog_->activateWindow();
  });

  // —— 设置：开机启动（T4.7；勾选态与登记文件同步）＋主题（R19 · T4.9）——
  auto* opt_menu = menuBar()->addMenu(QStringLiteral("设置"));
  opt_menu_ = opt_menu; // rail 齿轮（直连态）弹同源菜单，入口不分叉
  // —— 个人资料（需求批⑪）：个性签名设置（登录门在对话框入口）——
  auto* act_profile = opt_menu->addAction(QStringLiteral("个人资料…"));
  connect(act_profile, &QAction::triggered, this,
          &MainWindow::show_profile_dialog);
  auto* act_notify = opt_menu->addAction(QStringLiteral("通知偏好…"));
  connect(act_notify, &QAction::triggered, this, [] {
    NotificationCenter::instance().show_settings();
  });
  // —— 已读回执全局开关（需求批⑦）：勾选态即全局缺省；会话级覆盖走
  //     会话右键菜单——空 peer=全局写 QSettings receipts/send_read
  auto* act_receipts = opt_menu->addAction(QStringLiteral("发送已读回执"));
  act_receipts->setCheckable(true);
  act_receipts->setChecked(read_receipts_enabled(QString()));
  connect(act_receipts, &QAction::toggled, this, [this](bool on) {
    set_read_receipts_enabled(QString(), on);
  });
  act_theme_ = opt_menu->addAction(QStringLiteral("主题…"));
  act_theme_->setMenuRole(QAction::PreferencesRole); // macOS 走应用菜单偏好项
  connect(act_theme_, &QAction::triggered, this, &MainWindow::show_theme_settings);
  // —— 品牌物料设置页（设计稿 docs/design/品牌物料.md §4）：org-admin
  //     配牌入口；保存成功后本机 BrandKit 即时换牌（＝本机先见）——
  auto* act_brand = opt_menu->addAction(QStringLiteral("品牌物料…"));
  connect(act_brand, &QAction::triggered, this, [this] {
    if (!brand_settings_dialog_) {
      brand_settings_dialog_ = new BrandSettingsDialog(this);
      brand_settings_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    brand_settings_dialog_->show();
    brand_settings_dialog_->raise();
    brand_settings_dialog_->activateWindow();
  });
  // —— 连接设置（部署批 424：域名解析覆盖，局域网直连）——
  auto* act_conn = opt_menu->addAction(QStringLiteral("连接设置…"));
  connect(act_conn, &QAction::triggered, this,
          &MainWindow::show_conn_settings_dialog);
  // —— 快捷键设置（用户令 2026-10-08 ③：截图键可改＋冲突检测）——
  auto* act_shortcut = opt_menu->addAction(QStringLiteral("快捷键…"));
  connect(act_shortcut, &QAction::triggered, this, [this] {
    if (!shortcut_dialog_) {
      shortcut_dialog_ = new ShortcutSettingsDialog(this, this);
      shortcut_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    }
    shortcut_dialog_->show();
    shortcut_dialog_->raise();
    shortcut_dialog_->activateWindow();
  });
  // —— 离开锁屏设置（用户令 2026-10-08 ④：离开密码＋无操作超时＋启用）——
  auto* act_away = opt_menu->addAction(QStringLiteral("离开与锁屏…"));
  connect(act_away, &QAction::triggered, this, &MainWindow::show_away_dialog);
  // —— 网络与远程设置（用户令 2026-10-08 ⑤：网段黑名单＋远程控制密码）——
  auto* act_net = opt_menu->addAction(QStringLiteral("网络与远程…"));
  connect(act_net, &QAction::triggered, this, &MainWindow::show_net_dialog);
  opt_menu->addSeparator();
  act_autostart_ =
      opt_menu->addAction(QStringLiteral("开机启动（登录后自动运行）"));
  act_autostart_->setCheckable(true);
  act_autostart_->setChecked(autostart_enabled());
  connect(act_autostart_, &QAction::triggered, this, [this](bool on) {
    set_autostart(on);
    act_autostart_->setChecked(autostart_enabled()); // 落盘失败回滚勾选，不说谎
  });

  // —— 帮助：关于页（A23 图标面：logo＋版本，与窗口/任务栏图标同源）——
  auto* help_menu = menuBar()->addMenu(QStringLiteral("帮助"));
  auto* act_about = help_menu->addAction(QStringLiteral("关于 Memex…"));
  act_about->setMenuRole(QAction::AboutRole); // macOS 走应用菜单「关于」项
  connect(act_about, &QAction::triggered, this, [this] { show_about(); });

  setWindowIcon(brand_icon());
  setup_tray(); // 托盘可用才建（offscreen 等环境跳过）

  // —— 左侧：局域网设备列表（直连态以设备替代联系人）——
  auto* side = new QWidget(this);
  side_ = side;
  side->setObjectName(QStringLiteral("side_column"));
  auto* side_layout = new QVBoxLayout(side);
  side_layout->setContentsMargins(10, 10, 6, 10);
  side_layout->setSpacing(6);

  // 品牌行（服务器配了牌才显＝没配就不变；apply_brand 驱动显隐与内容）
  auto* brand_row = new QWidget(side);
  brand_row_ = brand_row;
  auto* brand_lay = new QHBoxLayout(brand_row);
  brand_lay->setContentsMargins(0, 0, 0, 0);
  brand_lay->setSpacing(6);
  brand_logo_ = new QLabel(brand_row);
  brand_logo_->setFixedSize(20, 20);
  brand_name_ = new QLabel(brand_row);
  brand_lay->addWidget(brand_logo_);
  brand_lay->addWidget(brand_name_);
  brand_lay->addStretch();
  brand_row->setVisible(false); // 默认标兜底：无牌不占位

  auto* side_head = new QHBoxLayout();
  auto* side_title = new QLabel(QStringLiteral("局域网设备"), side);
  side_title_ = side_title;
  device_count_ = new QLabel(side);
  side_head->addWidget(side_title);
  side_head->addStretch();
  side_head->addWidget(device_count_);
  // 发起单聊（原型 .side-head 品牌圆角 + 钮）：协作态可见，账号菜单开聊
  new_chat_btn_ = new QPushButton(QStringLiteral("+"), side);
  new_chat_btn_->setObjectName(QStringLiteral("new_chat_btn"));
  new_chat_btn_->setFixedSize(26, 26);
  new_chat_btn_->setCursor(Qt::PointingHandCursor);
  new_chat_btn_->setToolTip(QStringLiteral("发起单聊"));
  new_chat_btn_->setVisible(false);
  connect(new_chat_btn_, &QPushButton::clicked, this,
          &MainWindow::show_new_chat_menu);
  side_head->addWidget(new_chat_btn_);

  search_box_ = new QLineEdit(side);
  search_box_->setPlaceholderText(QStringLiteral("搜索设备 / IP"));
  search_box_->setClearButtonEnabled(true);

  // 发现口径注脚（原型 side-head：自动发现本网段 x.x.x.0/24 · UDP 2425）：
  // 子网按本机首个非环回 IPv4 推 /24；取不到地址时退「本网段」不编数。
  {
    QString subnet = QStringLiteral("本网段");
    for (const QNetworkInterface& ni : QNetworkInterface::allInterfaces()) {
      if (ni.flags() & QNetworkInterface::IsLoopBack) continue;
      for (const QNetworkAddressEntry& e : ni.addressEntries()) {
        const QHostAddress a = e.ip();
        if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
        const quint32 v = a.toIPv4Address();
        subnet = QHostAddress(v & 0xFFFFFF00u).toString() + QStringLiteral("/24");
        break;
      }
      if (subnet != QStringLiteral("本网段")) break;
    }
    disc_direct_text_ = QStringLiteral("自动发现 %1 · UDP %2")
                            .arg(subnet, QString::number(kDiscoveryPort));
    disc_hint_ = new QLabel(disc_direct_text_, side);
  }

  device_list_ = new QListWidget(side);
  device_list_->setObjectName(QStringLiteral("device_list"));
  device_list_->setFrameShape(QFrame::NoFrame);
  device_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  // 群条目右键菜单（T4.1）：拉人／公告／退群／解散
  device_list_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(device_list_, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) { show_group_menu(pos); });

  // —— 直连态「登录协作态」推广卡（原型直连屏侧栏面）：未登录可见，
  //     说明行＋登录按钮（走既有登录对话框，不另起流程）——
  collab_promo_ = new QWidget(side);
  collab_promo_->setObjectName(QStringLiteral("collab_promo"));
  auto* promo_lay = new QVBoxLayout(collab_promo_);
  promo_lay->setContentsMargins(10, 8, 10, 8);
  promo_lay->setSpacing(3);
  auto* promo_row = new QHBoxLayout();
  auto* promo_title = new QLabel(QStringLiteral("登录协作态"), collab_promo_);
  promo_title->setObjectName(QStringLiteral("collab_promo_title"));
  auto* promo_btn = new QPushButton(QStringLiteral("登录"), collab_promo_);
  promo_btn->setObjectName(QStringLiteral("collab_promo_btn"));
  promo_btn->setCursor(Qt::PointingHandCursor);
  connect(promo_btn, &QPushButton::clicked, this,
          &MainWindow::show_collab_login_dialog);
  promo_row->addWidget(promo_title);
  promo_row->addStretch();
  promo_row->addWidget(promo_btn);
  auto* promo_sub =
      new QLabel(QStringLiteral("组织架构、云端历史、消息归档检索"), collab_promo_);
  promo_sub->setObjectName(QStringLiteral("collab_promo_sub"));
  promo_sub->setWordWrap(true);
  promo_lay->addLayout(promo_row);
  promo_lay->addWidget(promo_sub);

  side_layout->addLayout(side_head);
  side_layout->addWidget(search_box_);
  side_layout->addWidget(disc_hint_);
  side_layout->addWidget(collab_promo_);
  side_layout->addWidget(device_list_, 1);
  side_layout->insertWidget(0, brand_row_);

  // —— 右侧：聊天窗 ——（布局令：空态隐藏，open_chat 展开成两栏）
  auto* chat = new QWidget(this);
  chat_panel_ = chat;
  auto* chat_layout = new QVBoxLayout(chat);
  chat_layout->setContentsMargins(0, 0, 0, 0);
  chat_layout->setSpacing(0);

  auto* head = new QWidget(chat);
  head_ = head;
  auto* head_layout = new QHBoxLayout(head);
  head_layout->setContentsMargins(14, 10, 14, 10);
  head_layout->setSpacing(10);
  // 会话头像（原型 .chat-head .avatar.sm 28px 首字）：随会话与主题重刷
  chat_avatar_ = new QLabel(head);
  chat_avatar_->setFixedSize(28, 28);
  chat_avatar_->setAlignment(Qt::AlignCenter);
  chat_title_ = new QLabel(QStringLiteral("直连态"), head);
  chat_meta_ = new QLabel(QStringLiteral("未选择设备"), head);
  auto* local_badge = new QLabel(QStringLiteral("本机保存"), head);
  local_badge_ = local_badge;
  auto* chat_search_btn = new QPushButton(QStringLiteral("🔍"), head);
  chat_search_btn_ = chat_search_btn;
  chat_search_btn->setFlat(true);
  chat_search_btn->setToolTip(
      QStringLiteral("搜索聊天记录（本机）：只记一半、词在中间、多词 AND"));
  connect(chat_search_btn, &QPushButton::clicked, this, [this] {
    if (!message_search_) {
      message_search_ = new MessageSearchDialog(this);
      message_search_->setAttribute(Qt::WA_DeleteOnClose);
      connect(message_search_, &MessageSearchDialog::open_message, this,
              [this](const QString& peer, const QString& msg_id) {
                if (peer.isEmpty()) return;
                // 对端键→会话类型：群键＝group；协作会话列表内＝collab；
                // 其余（含直连设备）＝direct。
                QString kind = QStringLiteral("direct");
                if (peer.startsWith(QStringLiteral("group:"))) {
                  kind = QStringLiteral("group");
                } else if (collab_engine_.is_logged_in() &&
                           collab_peers_.contains(peer)) {
                  kind = QStringLiteral("collab");
                }
                open_chat(kind, peer);
                // 命中定位（尽力而为）：气泡无单条锚点，退一级滚到命中所在日
                if (!msg_id.isEmpty()) {
                  for (const ChatRow& r : chat_rows_) {
                    if (!r.system && r.msg_id == msg_id) {
                      scroll_to_day(
                          QDateTime::fromMSecsSinceEpoch(r.ts_ms).date());
                      break;
                    }
                  }
                }
              });
    }
    message_search_->bind_store(direct_engine_.store());
    message_search_->show();
    message_search_->raise();
    message_search_->activateWindow();
    if (message_search_->search_box()) message_search_->search_box()->setFocus();
  });
  auto* close_chat_btn = new QPushButton(QStringLiteral("✕"), head);
  close_chat_btn->setFlat(true);
  close_chat_btn->setToolTip(QStringLiteral("关闭会话（回到列表态）"));
  connect(close_chat_btn, &QPushButton::clicked, this,
          &MainWindow::close_chat);
  // 头部动作位（原型 .ch-actions 文件/更多）：文件走生产发文件路径；
  // 更多＝既有会话操作菜单（按会话形态给真实项，不摆空钮）。语音无实现
  // 不入面（台账近似项）
  auto* head_file_btn = new QPushButton(QStringLiteral("📄"), head);
  head_file_btn->setFlat(true);
  head_file_btn->setFixedSize(30, 30);
  head_file_btn->setCursor(Qt::PointingHandCursor);
  head_file_btn->setToolTip(QStringLiteral("发送文件（点对点）"));
  connect(head_file_btn, &QPushButton::clicked, this,
          [this] { file_btn_->click(); });
  auto* head_more_btn = new QPushButton(QStringLiteral("⋮"), head);
  head_more_btn->setFlat(true);
  head_more_btn->setFixedSize(30, 30);
  head_more_btn->setCursor(Qt::PointingHandCursor);
  head_more_btn->setToolTip(QStringLiteral("更多会话操作"));
  connect(head_more_btn, &QPushButton::clicked, this, [this, btn = head_more_btn] {
    QMenu menu(this);
    const QString kind = current_kind_;
    if (kind == QStringLiteral("group")) {
      const quint64 gid = current_group_id();
      if (gid) {
        connect(menu.addAction(QStringLiteral("群资料…")),
                &QAction::triggered, this,
                [this, gid] { group_profile_dialog(gid); });
        connect(menu.addAction(QStringLiteral("拉人进群…")),
                &QAction::triggered, this,
                [this, gid] { group_invite_dialog(gid); });
        connect(menu.addAction(QStringLiteral("群公告编辑…")),
                &QAction::triggered, this,
                [this, gid] { group_announce_dialog(gid); });
        connect(menu.addAction(QStringLiteral("导出群记录…")),
                &QAction::triggered, this,
                [this, gid] { group_record_export_dialog(gid); });
      }
      connect(menu.addAction(QStringLiteral("群工具…")), &QAction::triggered,
              this, [this] {
                if (!group_tools_dialog_) {
                  group_tools_dialog_ = new GroupToolsDialog(this);
                  group_tools_dialog_->setAttribute(Qt::WA_DeleteOnClose);
                }
                group_tools_dialog_->show();
                group_tools_dialog_->raise();
                group_tools_dialog_->activateWindow();
              });
    } else {
      QAction* nudge = menu.addAction(QStringLiteral("窗口抖动"));
      connect(nudge, &QAction::triggered, this, [this] { nudge_btn_->click(); });
      QAction* by_date = menu.addAction(QStringLiteral("按日期查看…"));
      connect(by_date, &QAction::triggered, this, [this] { date_btn_->click(); });
      QAction* search = menu.addAction(QStringLiteral("搜索本机记录…"));
      connect(search, &QAction::triggered, this, [this] { chat_search_btn_->click(); });
    }
    menu.exec(btn->mapToGlobal(QPoint(0, btn->height())));
  });
  // 群聊头部动作位（原型 desktop-group.html .ch-actions：群公告/群文件/成员
  // brand 钮）——仅群会话显示；公告编辑与群资料走既有真实弹窗，群文件＝
  // 切右栏（.infopanel）显隐
  auto* head_announce_btn = new QPushButton(QStringLiteral("📢"), head);
  head_announce_btn_ = head_announce_btn;
  head_announce_btn->setFlat(true);
  head_announce_btn->setFixedSize(30, 30);
  head_announce_btn->setCursor(Qt::PointingHandCursor);
  head_announce_btn->setToolTip(QStringLiteral("群公告（编辑）"));
  head_announce_btn->hide();
  connect(head_announce_btn, &QPushButton::clicked, this, [this] {
    const quint64 gid = current_group_id();
    if (gid) group_announce_dialog(gid);
  });
  auto* head_gfiles_btn = new QPushButton(QStringLiteral("🗂"), head);
  head_gfiles_btn_ = head_gfiles_btn;
  head_gfiles_btn->setFlat(true);
  head_gfiles_btn->setFixedSize(30, 30);
  head_gfiles_btn->setCursor(Qt::PointingHandCursor);
  head_gfiles_btn->setToolTip(QStringLiteral("群文件（右侧栏）"));
  head_gfiles_btn->hide();
  connect(head_gfiles_btn, &QPushButton::clicked, this, [this] {
    group_panel_hidden_ = !group_panel_hidden_;
    if (group_panel_) group_panel_->setVisible(!group_panel_hidden_);
  });
  auto* head_members_btn = new QPushButton(QStringLiteral("👥"), head);
  head_members_btn_ = head_members_btn;
  head_members_btn->setFlat(true);
  head_members_btn->setFixedSize(30, 30);
  head_members_btn->setCursor(Qt::PointingHandCursor);
  head_members_btn->setToolTip(QStringLiteral("群成员（群资料）"));
  head_members_btn->hide();
  connect(head_members_btn, &QPushButton::clicked, this, [this] {
    const quint64 gid = current_group_id();
    if (gid) group_profile_dialog(gid);
  });
  head_layout->addWidget(chat_avatar_);
  head_layout->addWidget(chat_title_);
  head_layout->addWidget(chat_meta_);
  head_layout->addStretch();
  head_layout->addWidget(local_badge);
  head_layout->addWidget(chat_search_btn);
  head_layout->addWidget(head_announce_btn);
  head_layout->addWidget(head_gfiles_btn);
  head_layout->addWidget(head_members_btn);
  head_layout->addWidget(head_file_btn);
  head_layout->addWidget(head_more_btn);
  head_layout->addWidget(close_chat_btn);

  // 「未归档」语义标记：常驻不可关（留痕铁律的界面表达）；
  // 文案随形态切换（T2.4）：直连／降级态必须出现「消息不进归档」。
  banner_ = new QLabel(chat);
  banner_->setObjectName(QStringLiteral("mode_banner"));
  banner_->setWordWrap(true);

  chat_view_ = new QTextBrowser(chat);
  chat_view_->setFrameShape(QFrame::NoFrame);
  // 「盯」单：气泡内锚仅用于右键定位行键——关掉左键跟随链接（无导航
  // 副作用；文档内其他链接本就不存在，命名日锚不受影响）
  chat_view_->setOpenLinks(false);
  // 需求批② 表情包＋「盯」单：右键气泡菜单——图片气泡「收藏到表情包」；
  // 可盯文本气泡「盯住本消息／取消盯」（已读行入口置灰——已读无需盯）。
  chat_view_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(chat_view_, &QTextBrowser::customContextMenuRequested, this,
          [this](const QPoint& pos) {
    QTextCursor cur = chat_view_->cursorForPosition(pos);
    const QTextImageFormat img = cur.charFormat().toImageFormat();
    const QString src = img.name(); // file:///…
    if (img.isValid() && src.startsWith(QStringLiteral("file://"))) {
      const QString path = QUrl(src).toLocalFile();
      auto* menu = new QMenu(this);
      QAction* act = menu->addAction(QStringLiteral("收藏到表情包"));
      connect(act, &QAction::triggered, this, [this, path] {
        if (import_emoji(path)) {
          show_status(QStringLiteral("已收藏到表情包：%1（表情面板可见）")
                          .arg(QFileInfo(path).fileName()));
        }
      });
      menu->setAttribute(Qt::WA_DeleteOnClose);
      menu->popup(chat_view_->mapToGlobal(pos));
      return;
    }
    // 可盯行：锚携带行键（meta 行与正文同锚）。取锚不走 anchorAt——Qt 对
    // 表内（气泡是嵌套表）锚的命中区会空间错位（探针实录：字形区全空、
    // 命中区漂到表外，右键气泡主体永远取不到键）；cursorForPosition 的
    // 字符格式锚在顶层与嵌套表上都按字形区可靠解析。
    const QString key = watch_key_from_href(
        chat_view_->cursorForPosition(pos).charFormat().anchorHref());
    if (key.isEmpty()) {
      return; // 非可盯区域：不弹菜单（维持既有行为）
    }
    auto* menu = new QMenu(this);
    if (watched_.contains(key)) {
      QAction* act = menu->addAction(QStringLiteral("取消盯"));
      connect(act, &QAction::triggered, this, [this, key] {
        unwatch_key(key);
      });
    } else {
      const bool already_read = [&] {
        for (const ChatRow& r : chat_rows_) {
          if (!r.system && r.watch_key == key &&
              r.receipt == QStringLiteral("read")) {
            return true;
          }
        }
        return false;
      }();
      QAction* act = menu->addAction(QStringLiteral("盯住本消息"));
      act->setEnabled(!already_read); // 已读＝置灰（盯与已读同生命周期已尽）
      if (already_read) {
        act->setStatusTip(QStringLiteral("该消息已被读"));
      }
      connect(act, &QAction::triggered, this, [this, key] {
        watch_current_row(key);
      });
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(chat_view_->mapToGlobal(pos));
  });

  auto* input_row = new QWidget(chat);
  input_row_ = input_row;
  auto* input_layout = new QHBoxLayout(input_row);
  input_layout->setContentsMargins(10, 8, 10, 8);
  auto* file_btn = new QPushButton(QStringLiteral("发文件"), input_row);
  file_btn_ = file_btn;
  // 需求批⑤ 发文件夹：递归整目录（含子目录）走既有文件通道，对端按相对
  // 路径重建结构；进度按「第 x/N 个文件」聚合（引擎逐文件串行发）。
  auto* folder_btn = new QPushButton(QStringLiteral("发文件夹"), input_row);
  folder_btn_ = folder_btn;
  folder_btn->setToolTip(
      QStringLiteral("递归发送整个文件夹（含子目录，对端按相对路径重建）"));
  // T4.4 截图与标注：按钮＋ Ctrl+Alt+A 快捷键（与「发文件」同发送口径）
  auto* shot_btn = new QPushButton(QStringLiteral("截图"), input_row);
  shot_btn_ = shot_btn;
  shot_btn->setToolTip(
      QStringLiteral("截图并标注（%1）").arg(screenshot_shortcut()));
  // 截图快捷键（用户令 2026-10-08：可在设置改键——QSettings
  // shortcuts/screenshot，默认 Ctrl+Alt+A；冲突检测见 apply_…）
  shot_sc_ = new QShortcut(QKeySequence(screenshot_shortcut()), this);
  shot_sc_->setContext(Qt::WindowShortcut);
  connect(shot_btn, &QPushButton::clicked, this, [this] { start_screenshot(); });
  connect(shot_sc_, &QShortcut::activated, this,
          [this] { start_screenshot(); });
  // T4.5 表情：内置（按频次排序）＋自定义表情包导入（走文件通道发送）
  auto* emoji_btn = new QPushButton(QStringLiteral("表情"), input_row);
  emoji_btn_ = emoji_btn;
  connect(emoji_btn, &QPushButton::clicked, this,
          [this] { show_emoji_panel(); });
  // 需求批①文字颜色：选区着色（弹取色器，标记入输入框可继续编辑）
  auto* color_btn = new QPushButton(QStringLiteral("颜色"), input_row);
  color_btn_ = color_btn;
  color_btn->setToolTip(QStringLiteral("选中文字后点此设置颜色"));
  connect(color_btn, &QPushButton::clicked, this,
          [this] { request_input_color(); });
  auto* nudge_btn = new QPushButton(QStringLiteral("振屏"), input_row);
  nudge_btn_ = nudge_btn;
  nudge_btn->setToolTip(
      QStringLiteral("发送窗口抖动提醒（单聊；同一会话 10 秒一次）"));
  connect(nudge_btn, &QPushButton::clicked, this,
          [this] { send_nudge_to_current_chat(); });
  // 需求批⑧ 按日期查看：日历选择器跳转＋范围筛选（协作/群登录态自动
  // 向服务端补拉该窗，回包落本地索引后重渲）
  auto* date_btn = new QPushButton(QStringLiteral("按日期"), input_row);
  date_btn_ = date_btn;
  date_btn->setToolTip(QStringLiteral(
      "按日期跳转或范围筛选聊天记录（协作/群会话自动向服务端补拉）"));
  connect(date_btn, &QPushButton::clicked, this,
          [this] { show_date_dialog(); });
  input_layout->addWidget(shot_btn);
  input_layout->addWidget(file_btn);
  input_layout->addWidget(folder_btn);
  input_layout->addWidget(emoji_btn);
  input_layout->addWidget(color_btn);
  input_layout->addWidget(nudge_btn);
  input_layout->addWidget(date_btn);
  input_box_ = new QLineEdit(input_row);
  input_box_->setObjectName(QStringLiteral("input_box"));
  input_box_->setPlaceholderText(QStringLiteral("输入消息…"));
  send_btn_ = new QPushButton(QStringLiteral("发送"), input_row);
  // 「Enter 发送」提示（原型 .input-toolbar .hint：右缘 11px 次要色）
  auto* enter_hint = new QLabel(QStringLiteral("Enter 发送"), input_row);
  enter_hint->setStyleSheet(
      QStringLiteral("color:%1; font-size:11px;")
          .arg(ThemeManager::instance().tokens().text_muted.name()));
  // 「@ 唤起成员选择」提示（原型 desktop-group.html .input-toolbar .hint）：
  // 群会话专用（update_banner 控显隐；颜色随主题在 apply_theme_styles 重刷）
  at_hint_ = new QLabel(QStringLiteral("@ 唤起成员选择"), input_row);
  at_hint_->setStyleSheet(
      QStringLiteral("color:%1; font-size:11px;")
          .arg(ThemeManager::instance().tokens().text_muted.name()));
  at_hint_->hide();
  // 未归档口径小胶囊（原型 .input-foot .pill-outline「直连 · 无归档」）：
  // 随形态换字，归档会话不挂
  noarch_pill_ = new QLabel(input_row);
  input_layout->addWidget(input_box_, 1);
  input_layout->addWidget(at_hint_);
  input_layout->addWidget(enter_hint);
  input_layout->addWidget(noarch_pill_);
  input_layout->addWidget(send_btn_);

  // 输入区固定说明条（原型 .input-note）：随形态换文案（update_banner 同
  // 一状态机），跨态/直连＝警示底不可关闭，协作＝素底归档说明
  input_note_ = new QLabel(chat);
  input_note_->setWordWrap(true);

  chat_layout->addWidget(head);
  chat_layout->addWidget(banner_);
  chat_layout->addWidget(chat_view_, 1);
  chat_layout->addWidget(input_note_);
  chat_layout->addWidget(input_row);

  auto* central = new QWidget(this);
  auto* layout = new QHBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  // —— 最左：图标导航栏（原型桌面屏 rail 面，用户令 2026-10-10 原型走查
  //     P0：主导航缺失）。分组与顺序严格按原型，见 rebuild_rail()。——
  rail_ = new RailNav(central);
  rail_->setObjectName(QStringLiteral("rail_nav"));
  connect(rail_, &RailNav::item_clicked, this, &MainWindow::rail_clicked);
  layout->addWidget(rail_);
  side->setFixedWidth(260);
  layout->addWidget(side);
  // chat 高 stretch（100）：会话态吸收几乎全部余量。列表态（chat 隐藏）不再
  // 用尾部 addStretch 吸收余量——那会在右缘留整片空白（2026-10-14 用户报
  // 启动横窗右侧空白）；改为列表列本身 Expanding 占满 rail 之外的宽度
  //（close_chat 放开上限，open_chat 收回 260 固定宽）。
  layout->addWidget(chat, 100);
  // —— 群右栏（原型 desktop-group.html .infopanel：群成员/群文件/归档说明
  //     虚线卡，240px 固定宽）：仅群会话显示，内容与显隐归 update_group_panel ——
  group_panel_ = new QWidget(central);
  group_panel_->setObjectName(QStringLiteral("group_panel"));
  group_panel_->setFixedWidth(240);
  group_panel_lay_ = new QVBoxLayout(group_panel_);
  group_panel_lay_->setContentsMargins(16, 18, 16, 18);
  group_panel_lay_->setSpacing(14);
  group_panel_->hide();
  layout->addWidget(group_panel_);

  // —— 文件传输面（原型 desktop-files.html 整屏）：rail「文件传输」切视图，
  //     与会话侧栏/聊天面板互斥——侧栏换「传输通道」页（220px）、主区换
  //     传输列表（头部 meta/标签/传输卡/归档说明卡）。侧栏内容随主题与发现
  //     表全量重建（rebuild_transfer_side），传输卡随台账重建
  //     （refresh_transfers_view），此处只搭骨架。——
  transfer_side_ = new QWidget(central);
  transfer_side_->setObjectName(QStringLiteral("transfer_side"));
  transfer_side_->setFixedWidth(220);
  {
    auto* tsl = new QVBoxLayout(transfer_side_);
    tsl->setContentsMargins(10, 10, 6, 10);
    tsl->setSpacing(6);
  }
  transfer_side_->hide();

  transfer_page_ = new QWidget(central);
  transfer_page_->setObjectName(QStringLiteral("transfer_page"));
  {
    auto* tpl = new QVBoxLayout(transfer_page_);
    tpl->setContentsMargins(0, 0, 0, 0);
    tpl->setSpacing(0);
    // 头部（原型 .chat-head：名＋meta＋「发送文件」brand 钮）
    auto* th = new QWidget(transfer_page_);
    auto* thl = new QHBoxLayout(th);
    thl->setContentsMargins(14, 10, 14, 10);
    thl->setSpacing(10);
    auto* tname = new QLabel(QStringLiteral("文件传输"), th);
    tname->setObjectName(QStringLiteral("tx_title"));
    thl->addWidget(tname);
    tx_meta_ = new QLabel(th);
    thl->addWidget(tx_meta_);
    thl->addStretch();
    // 原型为 SVG 上箭头＋文字；Xvfb 无 emoji 字体，⬆/🔒 类符号渲染成
    // 豆腐块，改用基础箭头字形（DejaVu 全覆盖）
    tx_send_btn_ = new QPushButton(QStringLiteral("↑ 发送文件"), th);
    tx_send_btn_->setObjectName(QStringLiteral("tx_send_btn"));
    tx_send_btn_->setCursor(Qt::PointingHandCursor);
    tx_send_btn_->setToolTip(
        QStringLiteral("发送到当前会话（先在会话中选择对象）"));
    connect(tx_send_btn_, &QPushButton::clicked, this, [this] {
      if (current_peer_.isEmpty()) {
        show_status(QStringLiteral("先在会话中选择对象，再发送文件"));
        return;
      }
      if (current_kind_ == QStringLiteral("group") ||
          current_kind_ == QStringLiteral("dgroup")) {
        show_status(QStringLiteral("群会话暂不支持文件发送（文件走单聊点对点）"));
        return;
      }
      if (current_kind_ != QStringLiteral("collab") && !direct_send_allowed()) {
        return; // T3.4 策略闸门（与发文件同口径）
      }
      const QString path = QFileDialog::getOpenFileName(
          this, QStringLiteral("选择要发送的文件"));
      if (path.isEmpty()) return;
      transfer_send_file(path);
    });
    thl->addWidget(tx_send_btn_);
    tpl->addWidget(th);
    // 标签行（原型 .tx-tabs：全部/传输中 N/已完成/已暂停——已暂停如实
    // 空态，引擎无暂停 API，不造假控件语义）
    auto* tabs = new QWidget(transfer_page_);
    auto* tab_l = new QHBoxLayout(tabs);
    tab_l->setContentsMargins(18, 10, 18, 0);
    tab_l->setSpacing(4);
    const QStringList tab_names = {QStringLiteral("全部"),
                                   QStringLiteral("传输中"),
                                   QStringLiteral("已完成"),
                                   QStringLiteral("已暂停")};
    for (int i = 0; i < 4; ++i) {
      tx_tabs_[i] = new QPushButton(tab_names[i], tabs);
      tx_tabs_[i]->setCursor(Qt::PointingHandCursor);
      tx_tabs_[i]->setFlat(true);
      const int idx = i;
      connect(tx_tabs_[i], &QPushButton::clicked, this, [this, idx] {
        transfer_filter_ = idx;
        refresh_transfers_view();
      });
      tab_l->addWidget(tx_tabs_[i]);
    }
    tab_l->addStretch();
    tpl->addWidget(tabs);
    // 传输卡列表（滚动区）
    auto* scroll = new QScrollArea(transfer_page_);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto* list_host = new QWidget(scroll);
    list_host->setStyleSheet(QStringLiteral("background:transparent;"));
    tx_list_lay_ = new QVBoxLayout(list_host);
    tx_list_lay_->setContentsMargins(18, 14, 18, 14);
    tx_list_lay_->setSpacing(10);
    tx_empty_ = new QLabel(list_host);
    tx_empty_->setWordWrap(true);
    tx_list_lay_->addWidget(tx_empty_);
    tx_list_lay_->addStretch();
    scroll->setWidget(list_host);
    tpl->addWidget(scroll, 1);
    // 归档说明卡（原型 tx-note 同位：口径如实——本体不入任何归档，
    // 协作态授权留痕可追溯，直连态完全离线）
    auto* note = new QLabel(
        QStringLiteral("文件本体不经服务端、无归档；协作态发送经服务端授权"
                       "留痕——「谁在什么时间向谁传了哪个文件」可追溯，直连态"
                       "完全离线。"),
        transfer_page_);
    note->setObjectName(QStringLiteral("tx_note"));
    note->setWordWrap(true);
    auto* note_wrap = new QWidget(transfer_page_);
    auto* nwl = new QHBoxLayout(note_wrap);
    nwl->setContentsMargins(18, 0, 18, 16);
    nwl->addWidget(note);
    tpl->addWidget(note_wrap);
  }
  transfer_page_->hide();
  layout->addWidget(transfer_side_);
  layout->addWidget(transfer_page_, 100);

  // —— 组织架构面（原型 desktop-org.html 整屏三栏）：rail「组织架构」切
  //     视图——左＝部门树（搜索＋计数＋可见性虚线卡），中＝部门头＋成员卡
  //     网格，右＝成员档案（汇报链路＋kv＋动作钮）。树/网格/档案随 org
  //     数据与主题全量重建（rebuild_org_view），此处只搭骨架。——
  org_side_ = new QWidget(central);
  org_side_->setObjectName(QStringLiteral("org_side"));
  org_side_->setFixedWidth(240);
  {
    org_tree_lay_ = new QVBoxLayout(org_side_);
    org_tree_lay_->setContentsMargins(8, 10, 6, 10);
    org_tree_lay_->setSpacing(6);
    org_search_ = new QLineEdit(org_side_);
    org_search_->setPlaceholderText(QStringLiteral("搜索部门 / 成员"));
    org_search_->setClearButtonEnabled(true);
    org_tree_lay_->addWidget(org_search_);
    // 树与说明卡由 rebuild_org_view 填充（常驻复用说明卡）
    org_note_ = new QLabel(org_side_);
    org_note_->setObjectName(QStringLiteral("org_note"));
    org_note_->setWordWrap(true);
  }
  org_side_->hide();

  org_page_ = new QWidget(central);
  org_page_->setObjectName(QStringLiteral("org_page"));
  {
    auto* opl = new QVBoxLayout(org_page_);
    opl->setContentsMargins(0, 0, 0, 0);
    opl->setSpacing(0);
    // 部门头（原型 .side-head：部门 · N 人＋负责人胶囊＋「发起群聊」brand 钮）
    auto* oh = new QWidget(org_page_);
    auto* ohl = new QHBoxLayout(oh);
    ohl->setContentsMargins(14, 10, 14, 10);
    ohl->setSpacing(10);
    org_head_title_ = new QLabel(oh);
    org_head_title_->setObjectName(QStringLiteral("org_head_title"));
    ohl->addWidget(org_head_title_);
    org_head_pill_ = new QLabel(oh);
    org_head_pill_->setObjectName(QStringLiteral("org_head_pill"));
    ohl->addWidget(org_head_pill_);
    ohl->addStretch();
    auto* grp_btn = new QPushButton(QStringLiteral("发起群聊"), oh);
    grp_btn->setObjectName(QStringLiteral("org_group_btn"));
    grp_btn->setCursor(Qt::PointingHandCursor);
    connect(grp_btn, &QPushButton::clicked, this,
            [this] { create_group_dialog(); });
    ohl->addWidget(grp_btn);
    opl->addWidget(oh);
    // 成员卡网格（滚动区，两列流）
    auto* oscroll = new QScrollArea(org_page_);
    oscroll->setFrameShape(QFrame::NoFrame);
    oscroll->setWidgetResizable(true);
    auto* ohost = new QWidget(oscroll);
    ohost->setStyleSheet(QStringLiteral("background:transparent;"));
    org_grid_lay_ = new QVBoxLayout(ohost);
    org_grid_lay_->setContentsMargins(14, 12, 14, 12);
    org_grid_lay_->setSpacing(10);
    org_grid_empty_ = new QLabel(ohost);
    org_grid_empty_->setWordWrap(true);
    org_grid_lay_->addWidget(org_grid_empty_);
    org_grid_lay_->addStretch();
    oscroll->setWidget(ohost);
    opl->addWidget(oscroll, 1);
  }
  org_page_->hide();

  org_profile_ = new QWidget(central);
  org_profile_->setObjectName(QStringLiteral("org_profile"));
  org_profile_->setFixedWidth(260);
  {
    org_profile_lay_ = new QVBoxLayout(org_profile_);
    org_profile_lay_->setContentsMargins(14, 18, 14, 14);
    org_profile_lay_->setSpacing(8);
  }
  org_profile_->hide();
  layout->addWidget(org_side_);
  layout->addWidget(org_page_, 100);
  layout->addWidget(org_profile_);

  // —— 邮箱·统一收件箱面（原型 desktop-mailbox.html 整屏）：rail「邮箱」
  //     切视图——console（五列表＋计数页签）＋infopanel（详情/台账/留痕）。
  //     条目只经 FilesClient 拉取（不新增推送链），未读徽章以服务端计数
  //     为唯一来源；客户端在 files_client() 里注入（懒建同源）。——
  mailbox_page_ = new MailboxView(central);
  mailbox_page_->setObjectName(QStringLiteral("mailbox_page"));
  mailbox_page_->hide();
  connect(mailbox_page_, &MailboxView::unread_changed, this,
          [this](int total) { rail_->set_badge(QStringLiteral("mailbox"), total); });
  connect(mailbox_page_, &MailboxView::jump_requested, this,
          [this](const QString& ref) {
            // 正文留在各自来源：去群＝开群会话；去任务/去审批＝开对应窗口；
            // 其余（admin 事件源告警等）无桌面落位面，如实提示
            if (ref.startsWith(QStringLiteral("group:"))) {
              leave_mailbox_view();
              open_group(ref);
              return;
            }
            if (ref == QStringLiteral("tasks")) {
              if (!task_dialog_) task_dialog_ = new TaskDialog(this);
              task_dialog_->show();
              task_dialog_->raise();
              task_dialog_->activateWindow();
              return;
            }
            if (ref == QStringLiteral("approvals")) {
              if (!approval_dialog_) {
                approval_dialog_ = new ApprovalDialog(this);
                approval_dialog_->setAttribute(Qt::WA_DeleteOnClose);
              }
              approval_dialog_->show();
              approval_dialog_->raise();
              approval_dialog_->activateWindow();
              return;
            }
            show_status(QStringLiteral("该条目正文在服务端事件源，桌面无跳转面"));
          });
  layout->addWidget(mailbox_page_, 100);
  setCentralWidget(central);

  // 主题：控件样式统一在这里刷（构造时先按当前令牌落一次，之后跟随
  // ThemeManager::theme_changed 重刷，切换即时生效）。
  apply_theme_styles();

  // 需求批⑨查找联系人：可检索字段＝昵称/账号/设备 id/IP/群名/群成员
  // （UserRole+6，\n 分行）；模糊＝子串、精确＝整行相等（contains 超集，
  // 两种输入都命中）；分组头随组内可见项折叠，不留孤悬标题行。
  connect(search_box_, &QLineEdit::textChanged, this, [this](const QString& t) {
    const QString needle = t.trimmed();
    for (int i = 0; i < device_list_->count(); ++i) {
      auto* item = device_list_->item(i);
      const bool selectable =
          !item->data(Qt::UserRole).toString().isEmpty(); // 分组头＝空 id
      if (!selectable) continue; // 头行在第二遍按组内可见性折叠
      const QString hay = item->data(Qt::UserRole + 6).toString();
      const bool hit = needle.isEmpty() ||
                       (!hay.isEmpty() &&
                        hay.contains(needle, Qt::CaseInsensitive));
      item->setHidden(!hit);
    }
    // 第二遍：分组头＝其后到下一分组头之间还有可见项才显示
    for (int i = 0; i < device_list_->count(); ++i) {
      auto* item = device_list_->item(i);
      if (!item->data(Qt::UserRole).toString().isEmpty()) continue;
      bool any_visible = false;
      for (int j = i + 1; j < device_list_->count(); ++j) {
        auto* next = device_list_->item(j);
        if (next->data(Qt::UserRole).toString().isEmpty()) break; // 下一组
        if (!next->isHidden()) { any_visible = true; break; }
      }
      item->setHidden(!needle.isEmpty() && !any_visible);
    }
  });
  // 好友/会话列表：点击打开会话（点开＝面板展开）；再点当前会话项＝
  // 关回列表态（经典 IM 三栏切换）。会话开着与否看状态变量（current_peer_
  // 与面板显隐同步变化，close_chat 清/open_chat 设；离屏未 show 的窗口里
  // 子件 isVisible() 恒 false，不能拿面板显隐当判据）。selectionChanged
  // 键盘导航不打开，回车/双击激活走 itemActivated 同一路径。
  const auto list_open = [this](QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return; // 分组标题行不可选
    const QString kind = item->data(Qt::UserRole + 1).toString();
    if (!current_peer_.isEmpty() && id == current_peer_ &&
        kind == current_kind_) {
      close_chat(); // 再点当前好友＝回列表态
      return;
    }
    open_chat(kind, id);
  };
  connect(device_list_, &QListWidget::itemClicked, this, list_open);
  connect(device_list_, &QListWidget::itemActivated, this, list_open);
  connect(send_btn_, &QPushButton::clicked, this, [this] {
    const QString text = input_box_->text().trimmed();
    if (text.isEmpty() || current_peer_.isEmpty()) return;
    if (send_in_current_chat(text)) input_box_->clear();
  });
  connect(input_box_, &QLineEdit::returnPressed, this,
          [this] { send_btn_->click(); });
  // —— @ 成员选择器（原型 desktop-group.html .input-toolbar .hint「@ 唤起
  // 成员选择」）——群会话输入出现「@词头」（@ 起至串尾的无空白段）弹成员
  // 菜单（显示名，按词头过滤，≤12 项）；菜单为 Popup 型抢键，键入经
  // eventFilter 转发回输入框（连续输入继续过滤），选中回填「@显示名 」。
  at_menu_ = new QMenu(this);
  at_menu_->installEventFilter(this);
  connect(input_box_, &QLineEdit::textChanged, this, [this](const QString& t) {
    if (!at_menu_) return;
    // 菜单分发中键入会同步触发本 lambda——清 actions 推迟一帧（分发中清
    // 空 QMenu 会悬垂/崩溃），重建与显隐都走 rebuild_at_menu
    if (in_at_key_forward_) return;
    rebuild_at_menu(t);
  });
  connect(file_btn, &QPushButton::clicked, this, [this] {
    if (current_peer_.isEmpty()) {
      show_status(QStringLiteral("先选择设备再发送文件"));
      return;
    }
    if (current_kind_ == QStringLiteral("group") ||
        current_kind_ == QStringLiteral("dgroup")) {
      // 群文件传输不在本期范围：文件仍走点对点直连（单聊）
      show_status(QStringLiteral("群会话暂不支持文件发送（文件走单聊点对点）"));
      return;
    }
    if (current_kind_ != QStringLiteral("collab") &&
        !direct_send_allowed()) { // T3.4 策略闸门（文件与文字同口径）
      return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty()) return;
    transfer_send_file(path); // 发送缝（传输面「发送文件」钮同一路径）
  });
  // 需求批⑤ 发文件夹：点击＝守卫先行（无会话等场景不应弹目录选择框），
  // 目录选定后走发送缝（与测试同一路径）
  connect(folder_btn, &QPushButton::clicked, this, [this] {
    if (!folder_send_allowed()) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择要发送的文件夹"));
    if (dir.isEmpty()) return;
    send_folder_to_current_chat(dir);
  });
}

} // namespace memex::client
