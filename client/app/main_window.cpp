#include "main_window.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>

#include <core/local_store.hpp>

namespace memex::client {

namespace {
// 品牌图标（程序绘制，不依赖外部资源）：品牌橙圆角底＋白色 M。
// 托盘与窗口图标共用（logo 本体不替换，图标仅为程序内绘制示意）。
QIcon brand_icon() {
  QPixmap pm(64, 64);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(QColor(QStringLiteral("#e16531")));
  p.setPen(Qt::NoPen);
  p.drawRoundedRect(pm.rect().adjusted(2, 2, -2, -2), 14, 14);
  p.setPen(Qt::white);
  QFont f = p.font();
  f.setPixelSize(38);
  f.setBold(true);
  p.setFont(f);
  p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("M"));
  return QIcon(pm);
}
} // namespace

namespace {

QString esc(const QString& s) {
  QString out = s;
  out.replace(QChar('&'), QStringLiteral("&amp;"));
  out.replace(QChar('<'), QStringLiteral("&lt;"));
  out.replace(QChar('>'), QStringLiteral("&gt;"));
  out.replace(QChar('"'), QStringLiteral("&quot;"));
  return out;
}

QString hhmm(qint64 ts_ms) {
  return QDateTime::fromMSecsSinceEpoch(ts_ms).toString(QStringLiteral("HH:mm"));
}

// 气泡行：外出＝品牌橙靠右，来访＝浅灰靠左（QTextBrowser 富文本子集，不用圆角）。
// at_mode=true（群聊）：@账号 标记整体包品牌橙加粗。
QString bubble_html(const QString& name, const QString& text, qint64 ts_ms,
                    bool outgoing, bool at_mode = false) {
  const QString meta =
      QStringLiteral("<span style=\"color:#9b8f86; font-size:small;\">%1 %2</span>")
          .arg(esc(name), hhmm(ts_ms));
  QString content = esc(text);
  if (at_mode) {
    static const QRegularExpression at_re(
        QStringLiteral("@[A-Za-z0-9_.\\-]+"));
    QString highlighted;
    qsizetype pos = 0;
    auto it = at_re.globalMatch(content);
    while (it.hasNext()) {
      const auto m = it.next();
      highlighted += content.mid(pos, m.capturedStart() - pos);
      highlighted += QStringLiteral(
                          "<span style=\"color:#e16531; font-weight:600;\">%1</span>")
                          .arg(m.captured());
      pos = m.capturedEnd();
    }
    highlighted += content.mid(pos);
    content = std::move(highlighted);
  }
  const QString body = QStringLiteral(
                           "<table cellspacing=\"0\" cellpadding=\"6\"><tr><td "
                           "bgcolor=\"%1\"><span style=\"color:%2;\">%3</span></td>"
                           "</tr></table>")
                           .arg(outgoing ? QStringLiteral("#e16531")
                                         : QStringLiteral("#f0ebe5"),
                                outgoing ? QStringLiteral("#ffffff")
                                         : QStringLiteral("#332b24"),
                                content);
  if (outgoing) {
    return QStringLiteral(
               "<div>%1</div><table width=\"100%\" cellspacing=\"0\"><tr>"
               "<td width=\"28%\"></td><td align=\"right\">%2</td></tr></table>")
        .arg(meta, body);
  }
  return QStringLiteral(
             "<div>%1</div><table width=\"100%\" cellspacing=\"0\"><tr>"
             "<td align=\"left\">%2</td><td width=\"28%\"></td></tr></table>")
      .arg(meta, body);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Memex"));
  resize(960, 640);

  build_ui();

  wire_engines();
  direct_engine_.start();
  // 双态共用同一份本地库：协作消息与直连消息同库，界面按 source 合并展示
  collab_engine_.attach_store(direct_engine_.store());
  wire_collab();
  // T4.4 截图与标注：确认后发送（取消／失败仅提示）
  connect(&screenshot_tool_, &ScreenshotTool::confirmed, this,
          &MainWindow::on_screenshot_confirmed);
  connect(&screenshot_tool_, &ScreenshotTool::cancelled, this, [] {
    // 取消无需提示（用户主动放弃）
  });
  connect(&screenshot_tool_, &ScreenshotTool::failed, this,
          [this](const QString& reason) { show_status(reason); });

  refresh_devices();
  show_status(QStringLiteral("就绪"));
  update_banner();
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
  connect(act_org, &QAction::triggered, this, &MainWindow::show_org_dialog);
  connect(act_collab_logout_, &QAction::triggered, this,
          &MainWindow::logout_collab);
  connect(act_group_new, &QAction::triggered, this,
          [this] { create_group_dialog(); });
  connect(act_dgroup_new, &QAction::triggered, this, [this] { dgroup_dialog(); });

  // —— 设置：开机启动（T4.7；勾选态与登记文件同步）——
  auto* opt_menu = menuBar()->addMenu(QStringLiteral("设置"));
  act_autostart_ =
      opt_menu->addAction(QStringLiteral("开机启动（登录后自动运行）"));
  act_autostart_->setCheckable(true);
  act_autostart_->setChecked(autostart_enabled());
  connect(act_autostart_, &QAction::triggered, this, [this](bool on) {
    set_autostart(on);
    act_autostart_->setChecked(autostart_enabled()); // 落盘失败回滚勾选，不说谎
  });

  setWindowIcon(brand_icon());
  setup_tray(); // 托盘可用才建（offscreen 等环境跳过）

  // —— 左侧：局域网设备列表（直连态以设备替代联系人）——
  auto* side = new QWidget(this);
  auto* side_layout = new QVBoxLayout(side);
  side_layout->setContentsMargins(10, 10, 6, 10);
  side_layout->setSpacing(6);

  auto* side_head = new QHBoxLayout();
  auto* side_title = new QLabel(QStringLiteral("局域网设备"), side);
  side_title->setStyleSheet(
      QStringLiteral("font-weight:600; font-size:14px; color:#332b24;"));
  device_count_ = new QLabel(side);
  device_count_->setStyleSheet(
      QStringLiteral("color:#6f8f6a; font-size:12px;"));
  side_head->addWidget(side_title);
  side_head->addStretch();
  side_head->addWidget(device_count_);

  search_box_ = new QLineEdit(side);
  search_box_->setPlaceholderText(QStringLiteral("搜索设备 / IP"));
  search_box_->setClearButtonEnabled(true);

  device_list_ = new QListWidget(side);
  device_list_->setFrameShape(QFrame::NoFrame);
  device_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  // 群条目右键菜单（T4.1）：拉人／公告／退群／解散
  device_list_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(device_list_, &QListWidget::customContextMenuRequested, this,
          [this](const QPoint& pos) { show_group_menu(pos); });
  device_list_->setStyleSheet(QStringLiteral(
      "QListWidget { background:#faf7f3; border:1px solid #e8e0d6; "
      "border-radius:8px; }"
      "QListWidget::item { padding:8px; border-bottom:1px solid #efe8de; }"
      "QListWidget::item:selected { background:#f6e3d7; color:#332b24; }"));

  side_layout->addLayout(side_head);
  side_layout->addWidget(search_box_);
  side_layout->addWidget(device_list_, 1);

  // —— 右侧：聊天窗 ——
  auto* chat = new QWidget(this);
  auto* chat_layout = new QVBoxLayout(chat);
  chat_layout->setContentsMargins(0, 0, 0, 0);
  chat_layout->setSpacing(0);

  auto* head = new QWidget(chat);
  head->setStyleSheet(QStringLiteral("background:#ffffff;"));
  auto* head_layout = new QHBoxLayout(head);
  head_layout->setContentsMargins(14, 10, 14, 10);
  chat_title_ = new QLabel(QStringLiteral("直连态"), head);
  chat_title_->setStyleSheet(
      QStringLiteral("font-weight:600; font-size:15px; color:#332b24;"));
  chat_meta_ = new QLabel(QStringLiteral("未选择设备"), head);
  chat_meta_->setStyleSheet(QStringLiteral("color:#9b8f86; font-size:12px;"));
  auto* local_badge = new QLabel(QStringLiteral("本机保存"), head);
  local_badge->setStyleSheet(QStringLiteral(
      "background:#f6e3d7; color:#a05a26; border-radius:8px; padding:2px 8px; "
      "font-size:12px;"));
  head_layout->addWidget(chat_title_);
  head_layout->addWidget(chat_meta_);
  head_layout->addStretch();
  head_layout->addWidget(local_badge);

  // 「未归档」语义标记：常驻不可关（留痕铁律的界面表达）；
  // 文案随形态切换（T2.4）：直连／降级态必须出现「消息不进归档」。
  banner_ = new QLabel(chat);
  banner_->setObjectName(QStringLiteral("mode_banner"));
  banner_->setWordWrap(true);
  banner_->setStyleSheet(QStringLiteral(
      "background:#fdeee2; color:#8a4a1f; font-size:12px; padding:6px 10px;"));

  chat_view_ = new QTextBrowser(chat);
  chat_view_->setFrameShape(QFrame::NoFrame);
  chat_view_->setStyleSheet(QStringLiteral(
      "QTextBrowser { background:#ffffff; border:none; }"));

  auto* input_row = new QWidget(chat);
  input_row->setStyleSheet(QStringLiteral("background:#ffffff;"));
  auto* input_layout = new QHBoxLayout(input_row);
  input_layout->setContentsMargins(10, 8, 10, 8);
  auto* file_btn = new QPushButton(QStringLiteral("发文件"), input_row);
  file_btn->setStyleSheet(QStringLiteral(
      "QPushButton { background:#ffffff; color:#a05a26; border:1px solid "
      "#e16531; border-radius:8px; padding:6px 12px; }"
      "QPushButton:hover { background:#fdeee2; }"));
  // T4.4 截图与标注：按钮＋ Ctrl+Alt+A 快捷键（与「发文件」同发送口径）
  auto* shot_btn = new QPushButton(QStringLiteral("截图"), input_row);
  shot_btn->setStyleSheet(QStringLiteral(
      "QPushButton { background:#ffffff; color:#a05a26; border:1px solid "
      "#e16531; border-radius:8px; padding:6px 12px; }"
      "QPushButton:hover { background:#fdeee2; }"));
  shot_btn->setToolTip(QStringLiteral("截图并标注（Ctrl+Alt+A）"));
  auto* shot_sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+A")), this);
  shot_sc->setContext(Qt::WindowShortcut);
  connect(shot_btn, &QPushButton::clicked, this, [this] { start_screenshot(); });
  connect(shot_sc, &QShortcut::activated, this, [this] { start_screenshot(); });
  // T4.5 表情：内置（按频次排序）＋自定义表情包导入（走文件通道发送）
  auto* emoji_btn = new QPushButton(QStringLiteral("表情"), input_row);
  emoji_btn->setStyleSheet(QStringLiteral(
      "QPushButton { background:#ffffff; color:#a05a26; border:1px solid "
      "#e16531; border-radius:8px; padding:6px 12px; }"
      "QPushButton:hover { background:#fdeee2; }"));
  connect(emoji_btn, &QPushButton::clicked, this,
          [this] { show_emoji_panel(); });
  input_layout->addWidget(shot_btn);
  input_layout->addWidget(file_btn);
  input_layout->addWidget(emoji_btn);
  input_box_ = new QLineEdit(input_row);
  input_box_->setPlaceholderText(QStringLiteral("输入消息，回车发送"));
  send_btn_ = new QPushButton(QStringLiteral("发送"), input_row);
  send_btn_->setStyleSheet(QStringLiteral(
      "QPushButton { background:#e16531; color:#ffffff; border:none; "
      "border-radius:8px; padding:6px 18px; font-weight:600; }"
      "QPushButton:hover { background:#c95524; }"
      "QPushButton:disabled { background:#d9cfc4; }"));
  input_layout->addWidget(input_box_, 1);
  input_layout->addWidget(send_btn_);

  chat_layout->addWidget(head);
  chat_layout->addWidget(banner_);
  chat_layout->addWidget(chat_view_, 1);
  chat_layout->addWidget(input_row);

  auto* central = new QWidget(this);
  auto* layout = new QHBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  side->setFixedWidth(260);
  side->setStyleSheet(
      QStringLiteral("background:#f5f0e8; border-right:1px solid #e8e0d6;"));
  layout->addWidget(side);
  layout->addWidget(chat, 1);
  setCentralWidget(central);

  connect(search_box_, &QLineEdit::textChanged, this, [this](const QString& t) {
    const QString needle = t.trimmed();
    for (int i = 0; i < device_list_->count(); ++i) {
      auto* item = device_list_->item(i);
      item->setHidden(!needle.isEmpty() &&
                      !item->text().contains(needle, Qt::CaseInsensitive));
    }
  });
  connect(device_list_, &QListWidget::itemSelectionChanged, this, [this] {
    auto* item = device_list_->currentItem();
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return; // 分组标题行不可选
    open_chat(item->data(Qt::UserRole + 1).toString(), id);
  });
  connect(send_btn_, &QPushButton::clicked, this, [this] {
    const QString text = input_box_->text().trimmed();
    if (text.isEmpty() || current_peer_.isEmpty()) return;
    if (send_in_current_chat(text)) input_box_->clear();
  });
  connect(input_box_, &QLineEdit::returnPressed, this,
          [this] { send_btn_->click(); });
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
    const std::string tid =
        direct_engine_.send_file(current_peer_.toStdString(), path);
    if (tid.empty()) {
      show_status(QStringLiteral("文件发送失败"));
      return;
    }
    const QString name = QFileInfo(path).fileName();
    file_sent_.insert(name, 0);
    append_system_line(QStringLiteral("[文件] %1 开始发送").arg(esc(name)));
    show_status(QStringLiteral("文件发送中：%1").arg(name));
  });
}

void MainWindow::wire_engines() {
  connect(&direct_engine_, &DirectEngine::peers_changed, this, [this] {
    cross_sweep(); // T4.2：对端离线／对端登录 → 跨态会话闭环
    update_banner(); // 跨态判定随宣告刷新（对端登录态变化即时反映）
    refresh_devices();
    show_status(status_hint_); // 在线数变化，刷新引擎态前缀
  });
  connect(&direct_engine_, &DirectEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms) {
            cross_touch(from); // T4.2：首条收发即上报会话建立
            if (from == current_peer_ &&
                current_kind_ == QStringLiteral("direct")) {
              append_message(from, text, ts_ms, false,
                             QStringLiteral("direct"));
            } else {
              show_status(QStringLiteral("来自 %1 的新消息").arg(from));
            }
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何直连新消息
  // 都通知，不随分支改写而丢失）
  connect(&direct_engine_, &DirectEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64) {
            if (!isActiveWindow()) {
              tray_notify(QStringLiteral("新消息"),
                          QStringLiteral("来自 %1：%2").arg(from, text));
            }
          });
  connect(&direct_engine_, &DirectEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            set_delivery_state(
                ok ? QStringLiteral("已送达（seq %1）").arg(seq)
                   : QStringLiteral("送达失败（seq %1）").arg(seq));
          });
  connect(&direct_engine_, &DirectEngine::file_progress, this,
          [this](const QString& /*id*/, quint64 done, quint64 total) {
            if (total == 0) return;
            // 状态栏节流：按 5% 步进刷新
            const int pct = static_cast<int>(done * 100 / total);
            static int last_pct = -1;
            if (pct / 5 != last_pct / 5 || pct == 100) {
              last_pct = pct;
              show_status(QStringLiteral("文件传输进度：%1%").arg(pct));
            }
          });
  connect(&direct_engine_, &DirectEngine::file_finished, this,
          [this](const QString& id, bool ok, const QString& error) {
            show_status(ok ? QStringLiteral("文件已送达")
                           : QStringLiteral("文件传输中断：%1").arg(error));
            // T4.4：截图临时文件在传输结束后清理（成功／失败均清）
            const auto shot = shot_paths_.constFind(id);
            if (shot != shot_paths_.cend()) {
              QFile::remove(shot.value());
              shot_paths_.erase(shot);
            }
          });
  connect(&direct_engine_, &DirectEngine::file_received, this,
          [this](const QString& /*id*/, const QString& path) {
            append_system_line(
                QStringLiteral("[文件] 已接收：%1").arg(esc(path)));
            show_status(QStringLiteral("文件已接收：%1").arg(path));
          });
}

// 协作信号接入界面（T2.4）：形态切换提示、降级提示、协作消息渲染。
// 降级口径：服务端不可达＝回落直连态，必须明示「消息不进归档」（A11）。
void MainWindow::wire_collab() {
  connect(&collab_engine_, &CollabEngine::logged_in, this,
          [this](const QString& account, const QString& display) {
            // T4.2：本端登录态写进发现宣告——对端据此判定跨态与显示账号
            direct_engine_.set_collab_account(account.toStdString());
            collab_login_ms_ = QDateTime::currentMSecsSinceEpoch(); // A8 归档起点
            act_collab_logout_->setEnabled(true);
            collab_was_logged_in_ = true;
            kick_text_.clear(); // T4.3：新会话清除旧互踢提示
            // 在线表以服务端推送为准（登录广播先于 LOGIN_RESULT 到达，
            // 推送已含自己——此处不清，否则会擦掉刚收到的推送）。
            seed_collab_peers();
            update_banner();
            refresh_devices();
            append_system_line(
                QStringLiteral("已切换协作态（%1）：此后消息经服务端转发并"
                               "全量归档")
                    .arg(esc(display)));
            if (current_kind_ == QStringLiteral("direct")) {
              // A8 客户端面：跨态直连会话归档自行（此前直连消息不进归档）
              append_system_line(QStringLiteral(
                  "归档自 %1（本次协作态登录时刻）：此前跨态直连消息不进"
                  "归档，转协作会话后自登录时刻起计入")
                      .arg(QDateTime::currentDateTime().toString(
                          QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
            }
            show_status(QStringLiteral("协作态已登录：%1").arg(display));
            // 登录即拉组织架构（策略与建群数据源）与群列表（T3.4／T4.1）
            collab_engine_.query_org();
            collab_engine_.query_groups();
          });
  connect(&collab_engine_, &CollabEngine::login_failed, this,
          [this](const QString& reason) {
            act_collab_logout_->setEnabled(false);
            update_banner();
            if (reason == QStringLiteral("无法连接服务器")) {
              // 降级：服务端不可达 → 停留直连态，明示归档缺口
              append_system_line(QStringLiteral(
                  "服务端不可达：已回落直连态，消息不进归档"
                  "（点对点传输，仍可正常收发）"));
              show_status(
                  QStringLiteral("服务端不可达，已回落直连态：消息不进归档"));
            } else {
              show_status(QStringLiteral("协作登录失败：%1").arg(reason));
            }
          });
  connect(&collab_engine_, &CollabEngine::connection_lost, this, [this] {
    collab_was_logged_in_ = false;
    online_accounts_.clear(); // 断线即未知在线态（重连推送后刷新）
    update_banner();
    refresh_devices();
    show_status(QStringLiteral(
        "服务端连接断开，直连态仍可用：消息不进归档（自动重连中）"));
  });
  connect(&collab_engine_, &CollabEngine::reconnected, this, [this] {
    collab_was_logged_in_ = true;
    update_banner();
    refresh_devices();
    show_status(QStringLiteral("协作态已恢复：消息重新进入归档"));
  });
  connect(&collab_engine_, &CollabEngine::kicked, this,
          [this](const QString& reason, const QString& replaced_by) {
            cross_end_all(); // T4.2：被踢即会话终点（此刻引擎尚未断开，
                             // CROSS_LOG 帧随断开前队列刷出）
            collab_was_logged_in_ = false;
            online_accounts_.clear(); // 在线表随会话失效（重登后推送刷新）
            act_collab_logout_->setEnabled(false);
            // kicked 信号先于引擎 logged_in_=false 到达：横幅与列表刷新
            // 排队到状态落定后（否则仍按登录态渲染协作横幅，状态说谎）。
            QMetaObject::invokeMethod(
                this,
                [this] {
                  update_banner();
                  refresh_devices();
                },
                Qt::QueuedConnection);
            // T4.3 桌面单点在线提示：常驻文案（测试断言面）＋非模态弹窗
            //（模态会阻塞自动化验收；关闭按钮常驻可查）。
            kick_text_ = QStringLiteral("已被顶替下线（%1）：%2")
                             .arg(replaced_by, reason);
            set_delivery_state(kick_text_);
            // T4.7：互踢同时走系统通知（托盘气泡；无托盘仅记录不断言面）
            tray_notify(QStringLiteral("已被顶替下线"), kick_text_);
            auto* box = new QMessageBox(
                QMessageBox::Warning, QStringLiteral("已被顶替下线"),
                kick_text_ + QStringLiteral("\n同账号在另一台设备登录，"
                                            "本机会话已断开（消息不进归档）。"),
                QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setModal(false);
            box->show();
          });
  connect(&collab_engine_, &CollabEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms,
                 const QString& msg_id) {
            if (current_kind_ == QStringLiteral("collab") &&
                from == current_peer_) {
              // 本地库已由引擎落库（同库），这里只做界面渲染
              append_message(from, text, ts_ms, false,
                             QStringLiteral("collab"));
              // T4.3：当前会话开着=已读，立即上报（发送方收 READ_NOTICE）
              if (!msg_id.isEmpty()) collab_engine_.mark_read(msg_id);
            } else {
              show_status(QStringLiteral("来自 %1 的协作消息").arg(from));
            }
            collab_peers_.insert(from);
            refresh_devices();
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何协作新消息
  // 都通知，不随分支改写而丢失）
  connect(&collab_engine_, &CollabEngine::message_received, this,
          [this](const QString& from, const QString&, qint64, const QString&) {
            if (!isActiveWindow()) {
              tray_notify(QStringLiteral("新消息"),
                          QStringLiteral("来自 %1 的协作消息").arg(from));
            }
          });
  connect(&collab_engine_, &CollabEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            // 已读是终态：后到的受理回执不回退状态（自发自收回环时先已读后回执）
            if (delivery_text_.contains(QStringLiteral("已读"))) return;
            set_delivery_state(
                ok ? QStringLiteral("协作消息已送达（seq %1）").arg(seq)
                   : QStringLiteral("协作消息送达超时（seq %1）").arg(seq));
          });
  connect(&collab_engine_, &CollabEngine::org_received, this,
          [this](const QString& org_json) {
            last_org_json_ = org_json;
            apply_policy(org_json);
            if (org_dialog_pending_) {
              org_dialog_pending_ = false;
              build_org_tree(org_json);
            } else {
              show_status(QStringLiteral("组织架构已更新"));
            }
          });
  connect(&collab_engine_, &CollabEngine::fav_received, this,
          [this](const QString& fav_json) { apply_favs(fav_json); });
  // —— T4.1 群聊 ——
  connect(&collab_engine_, &CollabEngine::group_result, this,
          [this](bool ok, const QString& reason, const QString& op,
                 quint64 group_id) {
            if (ok) {
              show_status(QStringLiteral("群操作成功（%1，群 %2）")
                              .arg(op, QString::number(group_id)));
            } else {
              show_status(QStringLiteral("群操作失败（%1）：%2").arg(op, reason));
            }
          });
  connect(&collab_engine_, &CollabEngine::groups_received, this,
          [this](const QString& groups_json) { apply_groups(groups_json); });
  connect(&collab_engine_, &CollabEngine::group_message_received, this,
          [this](const QString& group_key, const QString& sender,
                 const QString& text, qint64 ts_ms, const QString& msg_id) {
            if (current_kind_ == QStringLiteral("group") &&
                group_key == current_peer_) {
              append_message(sender, text, ts_ms, false,
                             QStringLiteral("collab"));
              // T4.3：群会话开着=已读（发送方按读者逐条收 READ_NOTICE）
              if (!msg_id.isEmpty()) collab_engine_.mark_read(msg_id);
            } else {
              const QString gname = groups_.value(group_key.mid(6).toULongLong())
                                        .name;
              show_status(QStringLiteral("来自群「%1」%2 的消息")
                              .arg(gname.isEmpty() ? group_key : gname, sender));
            }
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何群新消息
  // 都通知，不随分支改写而丢失）
  connect(&collab_engine_, &CollabEngine::group_message_received, this,
          [this](const QString& group_key, const QString& sender,
                 const QString&, qint64, const QString&) {
            if (!isActiveWindow()) {
              const QString gname =
                  groups_.value(group_key.mid(6).toULongLong()).name;
              tray_notify(QStringLiteral("新消息"),
                          QStringLiteral("来自群「%1」%2 的消息")
                              .arg(gname.isEmpty() ? group_key : gname, sender));
            }
          });
  // —— T4.3 已读回执与在线状态 ——
  connect(&collab_engine_, &CollabEngine::message_read, this,
          [this](const QString& /*msg_id*/, const QString& reader,
                 qint64 /*read_ms*/) {
            // READ_NOTICE 只发给原发送方：在此处即「对方已读我发出的消息」
            set_delivery_state(
                QStringLiteral("对方已读 ✓✓（%1）").arg(reader));
          });
  connect(&collab_engine_, &CollabEngine::presence_changed, this,
          [this](const QStringList& accounts) {
            // 逐个插入（不依赖 QSet 区间构造的可移植性）
            online_accounts_.clear();
            for (const QString& a : accounts) online_accounts_.insert(a);
            refresh_devices(); // 协作会话行在线标识随推送刷新
          });
}

// —— T2.4 模式切换公共入口 ——

void MainWindow::login_collab(const QString& host, quint16 port,
                              const QString& account,
                              const QString& password) {
  collab_engine_.login(host, port, account, password);
}

void MainWindow::logout_collab() {
  cross_end_all();          // T4.2：登出前闭环全部跨态会话（end 帧先于 LOGOUT）
  collab_engine_.logout();
  direct_engine_.set_collab_account(""); // 登出即广播「未登录」（对端即时闭环）
  act_collab_logout_->setEnabled(false);
  collab_was_logged_in_ = false;
  online_accounts_.clear(); // T4.3：登出即未知在线态
  delivery_text_.clear();   // T4.3：发送状态随会话失效
  update_banner();
  append_system_line(QStringLiteral(
      "已登出协作态，回到直连态：消息不进归档（本地历史保留，合并展示）"));
  show_status(QStringLiteral("协作态已登出：当前直连态，消息不进归档"));
  refresh_devices();
}

void MainWindow::open_collab_peer(const QString& account) {
  if (account.isEmpty()) return;
  if (account.startsWith(QStringLiteral("group:"))) { // 历史群会话键
    open_group(account);
    return;
  }
  collab_peers_.insert(account);
  refresh_devices();
  open_chat(QStringLiteral("collab"), account);
}

bool MainWindow::send_in_current_chat(const QString& text) {
  if (text.isEmpty() || current_peer_.isEmpty()) return false;
  // 协作单聊与服务端群聊同一口径：to=账号或 "group:<群号>"，均经服务端归档
  if (current_kind_ == QStringLiteral("collab") ||
      current_kind_ == QStringLiteral("group")) {
    const quint64 seq = collab_engine_.send_text(current_peer_, text);
    if (seq == 0) {
      show_status(QStringLiteral(
          "发送失败：协作态未登录（可先登录协作态；当前消息不进归档）"));
      return false;
    }
    append_message(collab_engine_.account(), text,
                   QDateTime::currentMSecsSinceEpoch(), true,
                   QStringLiteral("collab"));
    set_delivery_state(QStringLiteral("发送中…（seq %1）").arg(seq));
    return true;
  }
  // 免服务端临时群（直连态）：逐设备点对点扇出，不进归档
  if (current_kind_ == QStringLiteral("dgroup")) {
    if (!direct_send_allowed()) return false; // T3.4 策略闸门（与直连同口径）
    const QStringList members = dgroup_members_.value(current_peer_);
    int sent = 0;
    for (const QString& dev : members) {
      if (direct_engine_.send_text(dev.toStdString(), text.toStdString()) > 0) {
        ++sent;
      }
    }
    if (sent == 0) {
      show_status(QStringLiteral("临时群发送失败：无可达成员"));
      return false;
    }
    append_message(QString::fromStdString(direct_engine_.device_id()), text,
                   QDateTime::currentMSecsSinceEpoch(), true,
                   QStringLiteral("direct"));
    if (sent < members.size()) {
      show_status(QStringLiteral("临时群部分送达：%1/%2 台设备")
                      .arg(sent)
                      .arg(members.size()));
    }
    set_delivery_state(QStringLiteral("临时群已扇出（%1/%2 台设备）")
                           .arg(sent)
                           .arg(members.size()));
    return true;
  }
  if (!direct_send_allowed()) return false; // T3.4 策略闸门
  const std::string peer = current_peer_.toStdString();
  if (direct_engine_.send_text(peer, text.toStdString()) == 0) {
    show_status(QStringLiteral("发送失败：对端不可达"));
    return false;
  }
  cross_touch(current_peer_); // T4.2：跨态会话首触即上报建立
  // T4.5：登录态直连会话的最近联系上报（peer=设备标识，与服务端群/账号同表）
  if (collab_engine_.is_logged_in()) {
    collab_engine_.fav_cmd(QStringLiteral("touch"), current_peer_);
  }
  append_message(QString::fromStdString(direct_engine_.device_id()), text,
                 QDateTime::currentMSecsSinceEpoch(), true,
                 QStringLiteral("direct"));
  set_delivery_state(QStringLiteral("发送中…（直连点对点）"));
  return true;
}

void MainWindow::show_emoji_panel() {
  auto* dlg = new QDialog(this, Qt::Popup);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(QStringLiteral("表情"));
  auto* grid = new QGridLayout(dlg);
  grid->setContentsMargins(8, 8, 8, 8);
  grid->setHorizontalSpacing(4);
  grid->setVerticalSpacing(4);
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  QStringList builtin;
  builtin << QStringLiteral("😀") << QStringLiteral("😄") << QStringLiteral("😂")
          << QStringLiteral("🙂") << QStringLiteral("😉") << QStringLiteral("😊")
          << QStringLiteral("😍") << QStringLiteral("😘") << QStringLiteral("😎")
          << QStringLiteral("🤔") << QStringLiteral("😅") << QStringLiteral("😢")
          << QStringLiteral("😭") << QStringLiteral("😡") << QStringLiteral("👍")
          << QStringLiteral("👎") << QStringLiteral("👏") << QStringLiteral("🙏")
          << QStringLiteral("💪") << QStringLiteral("🎉") << QStringLiteral("❤️")
          << QStringLiteral("🔥") << QStringLiteral("✅") << QStringLiteral("❌");
  // 常用＝按本地使用频次降序（T4.5）
  std::sort(builtin.begin(), builtin.end(), [&settings](const QString& a, const QString& b) {
    return settings.value(QStringLiteral("emoji_use/") + a, 0).toInt() >
           settings.value(QStringLiteral("emoji_use/") + b, 0).toInt();
  });
  int i = 0;
  for (const QString& e : builtin) {
    auto* b = new QPushButton(e, dlg);
    b->setFixedSize(34, 34);
    b->setStyleSheet(QStringLiteral("QPushButton{border:none;font-size:18px;}"));
    connect(b, &QPushButton::clicked, this, [this, e, dlg, &settings] {
      input_box_->insert(e);
      input_box_->setFocus();
      const QString key = QStringLiteral("emoji_use/") + e;
      settings.setValue(key, settings.value(key, 0).toInt() + 1);
      dlg->close();
    });
    grid->addWidget(b, i / 8, i % 8);
    ++i;
  }
  // 自定义表情包：emoji 目录（emoji_dir，测试缝可覆盖）下的图片，
  // 点击即按文件通道发送
  const QString dir = emoji_dir();
  QDir().mkpath(dir);
  const QStringList files = QDir(dir).entryList(
      {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
       QStringLiteral("*.jpeg"), QStringLiteral("*.gif")},
      QDir::Files);
  int row = i / 8;
  int col = i % 8;
  for (const QString& f : files) {
    auto* b = new QPushButton(dlg);
    b->setFixedSize(40, 40);
    b->setIcon(QIcon(dir + QLatin1Char('/') + f));
    b->setIconSize(QSize(36, 36));
    const QString path = dir + QLatin1Char('/') + f;
    connect(b, &QPushButton::clicked, this, [this, path, dlg] {
      dlg->close();
      if (current_peer_.isEmpty()) {
        show_status(QStringLiteral("先选择会话再发送自定义表情"));
        return;
      }
      if (current_kind_ == QStringLiteral("group") ||
          current_kind_ == QStringLiteral("dgroup")) {
        show_status(QStringLiteral("群会话暂不支持自定义表情（走文件单聊）"));
        return;
      }
      const std::string tid =
          direct_engine_.send_file(current_peer_.toStdString(), path);
      if (tid.empty()) {
        show_status(QStringLiteral("自定义表情发送失败"));
        return;
      }
      append_system_line(QStringLiteral("[表情] %1")
                             .arg(esc(QFileInfo(path).fileName())));
    });
    grid->addWidget(b, row, col);
    if (++col >= 8) { col = 0; ++row; }
  }
  auto* imp = new QPushButton(QStringLiteral("导入自定义表情…"), dlg);
  connect(imp, &QPushButton::clicked, this, [this, dlg] {
    const QString src = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择表情图片"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif)"));
    if (src.isEmpty()) return;
    if (import_emoji(src)) dlg->close(); // 导入失败留下面板＋状态栏报因
  });
  grid->addWidget(imp, row + 1, 0, 1, 4);
  dlg->show();
}

// —— T4.5 自定义表情：目录与导入 ——
QString MainWindow::emoji_dir() {
  // 测试覆盖：MEMEX_TEST_EMOJI_DIR 指向临时目录（与 MEMEX_TEST_AUTOSTART_DIR
  // 同口径），避免污染真实应用数据目录
  const QByteArray override_dir = qgetenv("MEMEX_TEST_EMOJI_DIR");
  if (!override_dir.isEmpty()) return QString::fromUtf8(override_dir);
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
         QStringLiteral("/emoji");
}

bool MainWindow::import_emoji(const QString& src) {
  static const QSet<QString> kExt{QStringLiteral("png"), QStringLiteral("jpg"),
                                  QStringLiteral("jpeg"),
                                  QStringLiteral("gif")};
  const QFileInfo si(src);
  if (!si.exists() || !si.isFile() || !kExt.contains(si.suffix().toLower())) {
    show_status(QStringLiteral("导入失败：不是支持的表情图片（png/jpg/jpeg/gif）"));
    return false;
  }
  const QString dir = emoji_dir();
  if (!QDir().mkpath(dir)) {
    show_status(QStringLiteral("导入失败：无法创建目录 %1").arg(dir));
    return false;
  }
  const QString dst = dir + QLatin1Char('/') + si.fileName();
  if (QFile::exists(dst) && !QFile::remove(dst)) {
    show_status(QStringLiteral("导入失败：无法覆盖同名文件 %1").arg(dst));
    return false;
  }
  if (!QFile::copy(src, dst)) {
    show_status(QStringLiteral("导入失败：无法写入 %1").arg(dir));
    return false;
  }
  show_status(QStringLiteral("已导入表情：%1（重新打开面板可见）")
                  .arg(si.fileName()));
  return true;
}

// —— T4.2 跨态互通 ——
// 跨态＝恰一边登录协作态（我已登录而对端未登录，或反之）。对端登录态取
// 自发现宣告的 account 字段（空=未登录；仅作显示与判定，不参与路由）。
// 跨态会话固定标「未归档」且不可关闭（A7）；已登录端上报会话建立/结束
// 日志（时间/双方/时长，无内容）——未登录端无通道，由对端上报。
bool MainWindow::is_cross_state(const QString& device_id) const {
  if (device_id.isEmpty()) return false;
  const Peer p = direct_engine_.peer(device_id.toStdString());
  if (p.device_id.empty()) return false; // 未发现：无从判定，不按跨态计
  return collab_engine_.is_logged_in() != !p.account.empty();
}

// 会话首触（收／发第一条）：已登录端上报 start；表内已有即跳过（幂等）。
void MainWindow::cross_touch(const QString& device_id) {
  if (device_id.isEmpty() || cross_open_.contains(device_id)) return;
  if (!collab_engine_.is_logged_in()) return;
  const Peer p = direct_engine_.peer(device_id.toStdString());
  if (p.device_id.empty() || !p.account.empty()) return; // 未发现／非跨态
  const qint64 started_ms = QDateTime::currentMSecsSinceEpoch();
  cross_open_.insert(device_id, started_ms);
  const QString name = p.name.empty()
                           ? QString::fromStdString(p.device_id).left(8)
                           : QString::fromStdString(p.name);
  collab_engine_.cross_log(QStringLiteral("start"), device_id, name,
                           started_ms, 0);
}

// 会话闭环：上报 end 并出表。服务端按 (账号, 设备, 建立时刻) 匹配最早
// 未结束行，对端名不参与匹配（离线后取不到名，传空即可）。
void MainWindow::cross_end(const QString& device_id) {
  const auto it = cross_open_.constFind(device_id);
  if (it == cross_open_.cend()) return;
  const qint64 started_ms = it.value();
  cross_open_.remove(device_id);
  if (!collab_engine_.is_logged_in()) return; // 通道已断，无法上报
  collab_engine_.cross_log(QStringLiteral("end"), device_id, QString(),
                           started_ms,
                           QDateTime::currentMSecsSinceEpoch());
}

void MainWindow::cross_end_all() {
  const QStringList keys = cross_open_.keys();
  for (const QString& id : keys) cross_end(id);
}

// 发现表变化巡检：对端离线或对端已登录（不再跨态）→ 会话闭环上报。
void MainWindow::cross_sweep() {
  const QStringList keys = cross_open_.keys();
  for (const QString& id : keys) {
    const Peer p = direct_engine_.peer(id.toStdString());
    if (p.device_id.empty() || !p.account.empty()) cross_end(id);
  }
}

// 验收面：打开（或发起）与某局域网设备的直连会话。
void MainWindow::open_direct_peer(const QString& device_id) {
  if (device_id.isEmpty()) return;
  open_chat(QStringLiteral("direct"), device_id);
}

bool MainWindow::has_direct_peer(const QString& device_id) const {
  return !device_id.isEmpty() && direct_engine_.has_peer(device_id.toStdString());
}

// —— T3.4 策略开关（下发自服务端，按本人部门解析）——
// 直连发送闸门：未登录发直连＝免登录使用；已登录发直连＝与未登录设备通信。
bool MainWindow::direct_send_allowed() {
  if (!collab_engine_.is_logged_in() && !anonymous_allowed_) {
    show_status(QStringLiteral(
        "当前策略：需登录协作态后使用（免登录使用已被管理员禁止）"));
    return false;
  }
  if (collab_engine_.is_logged_in() && !cross_state_allowed_) {
    show_status(QStringLiteral(
        "当前策略：禁止与未登录设备通信（该会话未进归档）"));
    return false;
  }
  return true;
}

// —— T4.4 截图与标注 ——
// 入口口径与「发文件」一致：仅直连单聊可发（群会话与协作会话无文件通道）；
// 策略闸门（免登录／跨态）同样适用。
void MainWindow::start_screenshot() {
  if (current_peer_.isEmpty()) {
    show_status(QStringLiteral("先选择设备再截图"));
    return;
  }
  if (current_kind_ != QStringLiteral("direct")) {
    show_status(QStringLiteral(
        "群会话与协作会话暂不支持截图发送（截图走单聊点对点，同文件口径）"));
    return;
  }
  if (!direct_send_allowed()) return; // T3.4 策略闸门（与文件同口径）
  screenshot_tool_.start();
}

// 截图确认后发送：PNG 临时文件走既有文件通道（与「发文件」同路径）。
void MainWindow::on_screenshot_confirmed(const QString& path) {
  if (current_kind_ != QStringLiteral("direct") || current_peer_.isEmpty()) return;
  const std::string tid =
      direct_engine_.send_file(current_peer_.toStdString(), path);
  if (tid.empty()) {
    show_status(QStringLiteral("截图发送失败"));
    return;
  }
  shot_paths_.insert(QString::fromStdString(tid), path);
  const QString name = QFileInfo(path).fileName();
  file_sent_.insert(name, 0);
  append_system_line(QStringLiteral("[截图] %1 开始发送").arg(esc(name)));
  show_status(QStringLiteral("截图已发送至当前会话：%1").arg(name));
}

// 解析本人生效策略：本人部门 → 逐级上级部门 → 全局行 → 默认宽松
void MainWindow::apply_policy(const QString& org_json) {
  nlohmann::json j =
      nlohmann::json::parse(org_json.toStdString(), nullptr, false);
  if (j.is_discarded()) return;
  // 本人部门路径
  QString dept;
  const QString me = collab_engine_.account();
  if (j.contains("members")) {
    for (const auto& m : j["members"]) {
      if (QString::fromStdString(m.value("account", std::string{})) == me) {
        dept = QString::fromStdString(m.value("department_path", std::string{}));
        break;
      }
    }
  }
  const auto match = [&](const QString& path) -> const nlohmann::json* {
    if (!j.contains("policies")) return nullptr;
    for (const auto& p : j["policies"]) {
      if (QString::fromStdString(p.value("department_path", std::string{})) ==
          path) {
        return &p;
      }
    }
    return nullptr;
  };
  const nlohmann::json* row = nullptr;
  QString path = dept;
  while (!path.isEmpty()) {
    if (const nlohmann::json* hit = match(path)) {
      row = hit;
      break;
    }
    const int pos = path.lastIndexOf(QChar('/'));
    if (pos <= 0) break;
    path.truncate(pos);
  }
  if (!row) row = match(QString()); // 全局行
  if (row) {
    anonymous_allowed_ = row->value("allow_anonymous", true);
    cross_state_allowed_ = row->value("allow_cross_state", true);
  } else {
    anonymous_allowed_ = true; // 未配置＝默认宽松
    cross_state_allowed_ = true;
  }
}

bool MainWindow::collab_logged_in() const {
  return collab_engine_.is_logged_in();
}

// —— T4.1 群聊 ——

QString MainWindow::groups_json() const { return last_groups_json_; }

// 组织架构全部账号（建群／拉人数据源；未拉到组织架构时为空，弹窗走手输）
QStringList MainWindow::org_accounts() const {
  QStringList out;
  nlohmann::json j = nlohmann::json::parse(last_org_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.contains("members")) return out;
  for (const auto& m : j["members"]) {
    const QString a =
        QString::fromStdString(m.value("account", std::string{}));
    if (!a.isEmpty()) out << a;
  }
  return out;
}

// 建群（服务端群）：群名＋成员多选（组织架构账号＋手输补位）
void MainWindow::create_group_dialog() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("建群需登录协作态（群消息经服务端扇出并归档）"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("新建群聊"));
  dlg.resize(380, 520);
  auto* form = new QFormLayout(&dlg);
  auto* name = new QLineEdit(&dlg);
  name->setPlaceholderText(QStringLiteral("例如：研发部日常"));
  form->addRow(QStringLiteral("群名"), name);
  auto* members = new QListWidget(&dlg);
  const QString me = collab_engine_.account();
  for (const QString& a : org_accounts()) {
    if (a == me) continue;
    auto* item = new QListWidgetItem(a, members);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  form->addRow(QStringLiteral("成员"), members);
  auto* extra = new QLineEdit(&dlg);
  extra->setPlaceholderText(QStringLiteral("其他账号，逗号分隔（可空）"));
  form->addRow(QString(), extra);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  const QString gname = name->text().trimmed();
  if (gname.isEmpty()) {
    show_status(QStringLiteral("建群取消：群名不能为空"));
    return;
  }
  QStringList picked;
  for (int i = 0; i < members->count(); ++i) {
    if (members->item(i)->checkState() == Qt::Checked) {
      picked << members->item(i)->text();
    }
  }
  for (const QString& part :
       extra->text().split(QChar(','), Qt::SkipEmptyParts)) {
    const QString a = part.trimmed();
    if (!a.isEmpty() && !picked.contains(a)) picked << a;
  }
  collab_engine_.create_group(gname, picked);
  show_status(QStringLiteral("建群请求已发送（%1 名成员）").arg(picked.size()));
}

// 打开群会话（group_key 形如 "group:7"）
void MainWindow::open_group(const QString& group_key) {
  if (group_key.isEmpty()) return;
  const quint64 gid =
      group_key.mid(QStringLiteral("group:").size()).toULongLong();
  if (!groups_.contains(gid)) {
    show_status(QStringLiteral("群 %1 不在列表（可能已退群／解散）").arg(gid));
    return;
  }
  open_chat(QStringLiteral("group"), group_key);
}

// 常用联系人到达（T4.5）：缓存并刷新列表（星标置顶排序在 refresh 里按此算）
void MainWindow::apply_favs(const QString& fav_json) {
  last_fav_json_ = fav_json;
  refresh_devices();
}

QString MainWindow::fav_json() const { return last_fav_json_; }

bool MainWindow::fav_is_starred_(const QString& peer) const {
  nlohmann::json j = nlohmann::json::parse(last_fav_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) return false;
  for (const auto& e : j) {
    if (e.value("peer", std::string{}) == peer.toStdString()) {
      return e.value("starred", false);
    }
  }
  return false;
}

qint64 MainWindow::fav_last_ms_(const QString& peer) const {
  nlohmann::json j = nlohmann::json::parse(last_fav_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) return 0;
  for (const auto& e : j) {
    if (e.value("peer", std::string{}) == peer.toStdString()) {
      return e.value("last_ms", 0LL);
    }
  }
  return 0;
}

void MainWindow::toggle_current_fav_star() {
  if (current_kind_ != QStringLiteral("collab") &&
      current_kind_ != QStringLiteral("group")) {
    show_status(QStringLiteral("常用联系人星标仅适用于协作会话／服务端群"));
    return;
  }
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("未登录协作态，常用联系人不可用"));
    return;
  }
  const QString peer = current_peer_;
  if (peer.isEmpty()) return;
  collab_engine_.fav_cmd(fav_is_starred_(peer) ? QStringLiteral("unstar")
                                               : QStringLiteral("star"),
                         peer);
}

// 群列表数据到达：解析入 groups_，刷新列表与当前群会话标题
void MainWindow::apply_groups(const QString& groups_json) {
  last_groups_json_ = groups_json;
  nlohmann::json j = nlohmann::json::parse(groups_json.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) {
    show_status(QStringLiteral("群列表数据解析失败"));
    return;
  }
  groups_.clear();
  for (const auto& g : j) {
    GroupEntry e;
    e.name = QString::fromStdString(g.value("name", std::string{}));
    e.owner = QString::fromStdString(g.value("owner", std::string{}));
    e.announcement =
        QString::fromStdString(g.value("announcement", std::string{}));
    if (g.contains("members")) {
      for (const auto& m : g["members"]) {
        e.members << QString::fromStdString(m.get<std::string>());
      }
    }
    groups_.insert(g.value("group_id", 0), std::move(e));
  }
  // 当前打开的群：标题与 meta 随最新数据刷新；群没了则收尾提示
  if (current_kind_ == QStringLiteral("group")) {
    if (GroupEntry* g = current_group()) {
      chat_title_->setText(g->name);
      QString meta = QStringLiteral("%1 人 · 群 %2 · 协作态 · 消息进入服务端归档")
                         .arg(g->members.size())
                         .arg(current_group_id());
      if (!g->announcement.isEmpty()) {
        meta.prepend(QStringLiteral("公告：%1　·　").arg(g->announcement));
      }
      chat_meta_->setText(meta);
    } else {
      append_system_line(
          QStringLiteral("该群已退出或已解散：本会话仅供查看历史"));
    }
  }
  refresh_devices();
}

quint64 MainWindow::current_group_id() const {
  if (current_kind_ != QStringLiteral("group")) return 0;
  return current_peer_.mid(QStringLiteral("group:").size()).toULongLong();
}

MainWindow::GroupEntry* MainWindow::current_group() {
  const quint64 gid = current_group_id();
  auto it = groups_.find(gid);
  return gid > 0 && it != groups_.end() ? &it.value() : nullptr;
}

// 群条目右键菜单：服务端群（拉人／公告／退群）与临时群（解散）
void MainWindow::show_group_menu(const QPoint& pos) {
  auto* item = device_list_->itemAt(pos);
  if (!item) return;
  const QString kind = item->data(Qt::UserRole + 1).toString();
  const QString id = item->data(Qt::UserRole).toString();
  QMenu menu(this);
  if (kind == QStringLiteral("collab") || kind == QStringLiteral("group")) {
    // T4.5：常用联系人星标／取消（group kind 的 id 形如 "group:N"，直接用）
    const bool starred = fav_is_starred_(id);
    auto* act_star = menu.addAction(
        starred ? QStringLiteral("取消星标") : QStringLiteral("星标（置顶）"));
    connect(act_star, &QAction::triggered, this, [this, id, starred] {
      if (!collab_engine_.is_logged_in()) {
        show_status(QStringLiteral("未登录协作态，常用联系人不可用"));
        return;
      }
      collab_engine_.fav_cmd(starred ? QStringLiteral("unstar")
                                     : QStringLiteral("star"),
                             id);
    });
    menu.addSeparator();
  }
  if (kind == QStringLiteral("group")) {
    const quint64 gid = id.mid(QStringLiteral("group:").size()).toULongLong();
    auto* act_invite = menu.addAction(QStringLiteral("拉人进群…"));
    auto* act_ann = menu.addAction(QStringLiteral("设置群公告…"));
    menu.addSeparator();
    auto* act_leave = menu.addAction(QStringLiteral("退出群聊"));
    connect(act_invite, &QAction::triggered, this,
            [this, gid] { group_invite_dialog(gid); });
    connect(act_ann, &QAction::triggered, this,
            [this, gid] { group_announce_dialog(gid); });
    connect(act_leave, &QAction::triggered, this, [this, gid] {
      if (QMessageBox::question(
              this, QStringLiteral("退出群聊"),
              QStringLiteral("确认退出该群？（群主退群＝解散该群）")) !=
          QMessageBox::Yes) {
        return;
      }
      collab_engine_.leave_group(gid);
    });
    menu.exec(device_list_->mapToGlobal(pos));
  } else if (kind == QStringLiteral("dgroup")) {
    auto* act_del = menu.addAction(QStringLiteral("解散临时群"));
    connect(act_del, &QAction::triggered, this, [this, id] {
      dgroup_members_.remove(id);
      if (current_peer_ == id) {
        current_kind_ = QStringLiteral("direct");
        current_peer_.clear();
      }
      refresh_devices();
      show_status(QStringLiteral("临时群已解散（本就不进归档，无服务端动作）"));
    });
    menu.exec(device_list_->mapToGlobal(pos));
  }
}

// 拉人进群（成员数据源同建群；服务端校验账号存在且不在群里）
void MainWindow::group_invite_dialog(quint64 group_id) {
  if (!groups_.contains(group_id)) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("拉人进群（群 %1）").arg(group_id));
  dlg.resize(380, 480);
  auto* layout = new QVBoxLayout(&dlg);
  auto* members = new QListWidget(&dlg);
  const QString me = collab_engine_.account();
  const QStringList in_group = groups_.value(group_id).members;
  for (const QString& a : org_accounts()) {
    if (a == me || in_group.contains(a)) continue;
    auto* item = new QListWidgetItem(a, members);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  auto* extra = new QLineEdit(&dlg);
  extra->setPlaceholderText(QStringLiteral("其他账号，逗号分隔（可空）"));
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(members);
  layout->addWidget(extra);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList picked;
  for (int i = 0; i < members->count(); ++i) {
    if (members->item(i)->checkState() == Qt::Checked) {
      picked << members->item(i)->text();
    }
  }
  for (const QString& part :
       extra->text().split(QChar(','), Qt::SkipEmptyParts)) {
    const QString a = part.trimmed();
    if (!a.isEmpty() && !picked.contains(a)) picked << a;
  }
  if (picked.isEmpty()) {
    show_status(QStringLiteral("拉人取消：未选择账号"));
    return;
  }
  collab_engine_.invite_group(group_id, picked);
  show_status(QStringLiteral("拉人请求已发送（%1 人）").arg(picked.size()));
}

// 群公告（仅群主可设；服务端校验并回执结果）
void MainWindow::group_announce_dialog(quint64 group_id) {
  const auto it = groups_.find(group_id);
  if (it == groups_.end()) return;
  bool ok = false;
  const QString text = QInputDialog::getMultiLineText(
      this, QStringLiteral("设置群公告（群 %1）").arg(group_id),
      QStringLiteral("群主专属；留空＝清除公告"), it->announcement, &ok);
  if (!ok) return;
  collab_engine_.announce_group(group_id, text.trimmed());
  show_status(QStringLiteral("群公告设置请求已发送"));
}

// 免服务端临时群（直连态）：从已发现设备多选，发送＝逐设备点对点扇出。
// 不经服务端——不进归档（界面分组与提示条明示）。
void MainWindow::dgroup_dialog() {
  const auto peers = direct_engine_.peers();
  if (peers.isEmpty()) {
    show_status(QStringLiteral("未发现局域网设备，无法建临时群"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("新建临时群（点对点扇出，不进归档）"));
  dlg.resize(400, 440);
  auto* layout = new QVBoxLayout(&dlg);
  auto* list = new QListWidget(&dlg);
  for (const Peer& p : peers) {
    const QString id = QString::fromStdString(p.device_id);
    const QString name =
        p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    auto* item = new QListWidgetItem(
        QStringLiteral("%1\n%2 · TCP %3")
            .arg(name, p.address.toString(), QString::number(p.tcp_port)),
        list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    item->setData(Qt::UserRole, id);
  }
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(list);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList picked;
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->checkState() == Qt::Checked) {
      picked << list->item(i)->data(Qt::UserRole).toString();
    }
  }
  if (picked.isEmpty()) {
    show_status(QStringLiteral("建临时群取消：未选择设备"));
    return;
  }
  const QString id = QStringLiteral("dgroup:%1").arg(next_dgroup_++);
  dgroup_members_.insert(id, picked);
  refresh_devices();
  open_chat(QStringLiteral("dgroup"), id);
  show_status(QStringLiteral("临时群已建立（%1 台设备）· 不进归档").arg(picked.size()));
}

QString MainWindow::dgroup_title(const QString& dgroup_id) const {
  return QStringLiteral("临时群 %1")
      .arg(dgroup_id.mid(QStringLiteral("dgroup:").size()));
}

// —— T3.1 组织架构：登录后向服务端查询，管理端维护即生效到客户端 ——

void MainWindow::request_org() {
  if (collab_engine_.is_logged_in()) collab_engine_.query_org();
}

QString MainWindow::org_json() const { return last_org_json_; }

void MainWindow::show_org_dialog() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("组织架构需登录协作态后查看"));
    return;
  }
  org_dialog_pending_ = true;
  collab_engine_.query_org();
}

// 组织架构弹窗：部门树（全路径逐级成树）＋成员挂部门（含直属上级、职务、角色）
void MainWindow::build_org_tree(const QString& org_json) {
  nlohmann::json j = nlohmann::json::parse(org_json.toStdString(),
                                           nullptr, false);
  if (j.is_discarded()) {
    show_status(QStringLiteral("组织架构数据解析失败"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("组织架构（服务端下发）"));
  dlg.resize(560, 620);
  auto* layout = new QVBoxLayout(&dlg);
  auto* tree = new QTreeWidget(&dlg);
  tree->setHeaderLabels({QStringLiteral("部门 / 成员"), QStringLiteral("职务"),
                         QStringLiteral("直属上级")});
  tree->setColumnWidth(0, 280);
  layout->addWidget(tree);

  // 部门：全路径逐级挂树（"公司/研发部/客户端组"）
  std::map<QString, QTreeWidgetItem*> nodes;
  auto* unassigned = new QTreeWidgetItem(tree);
  unassigned->setText(0, QStringLiteral("（未分配部门）"));
  if (j.contains("departments")) {
    for (const auto& d : j["departments"]) {
      const QString path =
          QString::fromStdString(d.value("path", std::string{}));
      if (path.isEmpty() || nodes.count(path)) continue;
      const QStringList segs = path.split(QChar('/'));
      QTreeWidgetItem* parent = nullptr;
      QString prefix;
      for (const QString& seg : segs) {
        prefix = prefix.isEmpty() ? seg : prefix + QChar('/') + seg;
        auto it = nodes.find(prefix);
        if (it == nodes.end()) {
          auto* item =
              new QTreeWidgetItem(parent ? parent : tree->invisibleRootItem());
          item->setText(0, seg);
          it = nodes.emplace(prefix, item).first;
        }
        parent = it->second;
      }
    }
  }
  // 成员挂部门节点；显示名（账号）＋管理员标记
  if (j.contains("members")) {
    for (const auto& m : j["members"]) {
      const QString dept =
          QString::fromStdString(m.value("department_path", std::string{}));
      const QString account =
          QString::fromStdString(m.value("account", std::string{}));
      const QString name =
          QString::fromStdString(m.value("display_name", std::string{}));
      const QString title =
          QString::fromStdString(m.value("title", std::string{}));
      const QString manager =
          QString::fromStdString(m.value("manager", std::string{}));
      const QString role =
          QString::fromStdString(m.value("role", std::string{}));
      auto it = nodes.find(dept);
      QTreeWidgetItem* parent =
          it != nodes.end() ? it->second : unassigned;
      auto* item = new QTreeWidgetItem(parent);
      item->setText(0, QStringLiteral("%1（%2）%3")
                            .arg(name, account,
                                 role == QStringLiteral("admin")
                                     ? QStringLiteral("·管理员")
                                     : QString{}));
      item->setText(1, title);
      item->setText(2, manager);
    }
  }
  tree->expandAll();
  auto* close = new QPushButton(QStringLiteral("关闭"), &dlg);
  connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
  layout->addWidget(close);
  dlg.exec();
}

QString MainWindow::banner_text() const { return banner_->text(); }

QString MainWindow::status_text() const { return statusBar()->currentMessage(); }

QString MainWindow::chat_html() const { return chat_view_->toHtml(); }

// —— T4.3 消息状态与多端（验收面）——
QString MainWindow::delivery_text() const { return delivery_text_; }
QString MainWindow::kick_text() const { return kick_text_; }
QStringList MainWindow::online_accounts() const {
  QStringList out = online_accounts_.values(); // values() 直取，避免区间构造歧义
  out.sort();
  return out;
}
void MainWindow::set_delivery_state(const QString& text) {
  delivery_text_ = text;
  show_status(text); // 状态栏同步（最近一条发出消息的状态常驻可查）
}

// —— T4.7 系统集成 ——

QString MainWindow::last_notify() const { return last_notify_; }

bool MainWindow::tray_available() const {
  return tray_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable();
}

void MainWindow::setup_tray() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) return; // 无托盘环境跳过
  tray_ = new QSystemTrayIcon(brand_icon(), this);
  tray_->setToolTip(QStringLiteral("Memex（直连态：消息不进归档）"));
  auto* menu = new QMenu(this);
  auto* act_show = menu->addAction(QStringLiteral("显示主窗口"));
  connect(act_show, &QAction::triggered, this, [this] {
    show();
    raise();
    activateWindow();
  });
  auto* act_tray_auto = menu->addAction(QStringLiteral("开机启动"));
  act_tray_auto->setCheckable(true);
  act_tray_auto->setChecked(autostart_enabled());
  connect(act_tray_auto, &QAction::triggered, this, [this, act_tray_auto](bool on) {
    set_autostart(on);
    act_tray_auto->setChecked(autostart_enabled());
    if (act_autostart_) act_autostart_->setChecked(autostart_enabled());
  });
  auto* act_quit = menu->addAction(QStringLiteral("退出"));
  connect(act_quit, &QAction::triggered, this, [this] {
    tray_ = nullptr; // 先摘托盘，closeEvent 不再拦截（真退出）
    close();
  });
  tray_->setContextMenu(menu);
  tray_->show();
  connect(tray_, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger && !isVisible()) {
              show();
              raise();
              activateWindow();
            }
          });
}

void MainWindow::tray_notify(const QString& title, const QString& text) {
  last_notify_ = title + QStringLiteral("：") + text; // 无托盘也记录（断言面）
  if (tray_ && QSystemTrayIcon::supportsMessages()) {
    tray_->showMessage(title, text, QSystemTrayIcon::Information, 5000);
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (tray_) {
    // 托盘可用：关闭即最小化进托盘（常驻后台不断直连发现；真退出走托盘菜单）
    hide();
    tray_notify(QStringLiteral("已最小化到托盘"),
                QStringLiteral("Memex 仍在后台运行（右键托盘图标可退出）"));
    event->ignore();
    return;
  }
  QMainWindow::closeEvent(event);
}

QString MainWindow::autostart_dir() {
  // 测试覆盖：MEMEX_TEST_AUTOSTART_DIR 指向临时目录，避免污染真实家目录
  const QByteArray override_dir = qgetenv("MEMEX_TEST_AUTOSTART_DIR");
  if (!override_dir.isEmpty()) return QString::fromUtf8(override_dir);
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         QStringLiteral("/autostart");
}

bool MainWindow::autostart_enabled() const {
  return QFile::exists(autostart_dir() + QStringLiteral("/memex-client.desktop"));
}

void MainWindow::set_autostart(bool on) {
  const QString path = autostart_dir() + QStringLiteral("/memex-client.desktop");
  if (!on) {
    QFile::remove(path);
    if (act_autostart_) act_autostart_->setChecked(false);
    return;
  }
  QDir().mkpath(autostart_dir());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (act_autostart_) act_autostart_->setChecked(false); // 落盘失败回滚勾选
    return;
  }
  QTextStream out(&f);
  out << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=Memex\n"
      << "Comment=Memex 内网办公即时通讯\n"
      << "Exec=" << QCoreApplication::applicationFilePath() << "\n"
      << "Terminal=false\n"
      << "Categories=Network;InstantMessaging;\n"
      << "X-GNOME-Autostart-enabled=true\n";
  f.close();
  if (act_autostart_) act_autostart_->setChecked(true);
}

// 归档提示条（常驻不可关）：协作态显示归档口径；直连／降级态必须明示
// 「消息不进归档」——切换与降级共用这一条提示（A7、A11 界面口径）。
void MainWindow::update_banner() {
  // 协作单聊与服务端群聊都走服务端转发归档（T4.1 后者同口径）
  const bool collab_session =
      (current_kind_ == QStringLiteral("collab") ||
       current_kind_ == QStringLiteral("group")) &&
      collab_engine_.is_logged_in();
  if (collab_session) {
    banner_->setStyleSheet(QStringLiteral(
        "background:#eaf3e7; color:#3f6b3a; font-size:12px; padding:6px 10px;"));
    banner_->setText(QStringLiteral(
        "　✔ 协作态会话：消息经服务端转发并全量归档；撤回仅改显示，"
        "服务端保留原文与撤回记录"));
  } else {
    banner_->setStyleSheet(QStringLiteral(
        "background:#fdeee2; color:#8a4a1f; font-size:12px; padding:6px 10px;"));
    // T4.2 跨态会话（恰一边登录）：固定「未归档」标识，常驻不可关闭（A7）
    bool cross = false;
    if (current_kind_ == QStringLiteral("direct")) {
      cross = is_cross_state(current_peer_);
    } else if (current_kind_ == QStringLiteral("dgroup")) {
      for (const QString& dev : dgroup_members_.value(current_peer_)) {
        if (is_cross_state(dev)) {
          cross = true;
          break;
        }
      }
    }
    if (cross) {
      banner_->setText(QStringLiteral(
          "　⚠ 跨态会话 · 未归档：恰一边登录协作态，消息点对点传输不进归档"
          "（与未登录终端的会话标记常驻不可关闭）"));
    } else if (collab_was_logged_in_ && !collab_engine_.is_logged_in()) {
      banner_->setText(QStringLiteral(
          "　⚠ 已降级直连态：服务端不可达，消息不进归档"
          "（点对点传输，仅保存在双方本机）"));
    } else {
      banner_->setText(QStringLiteral(
          "　⚠ 直连态会话：消息点对点传输，不经过服务器——消息不进归档"
          "（服务端无任何记录，仅保存在双方本机）"));
    }
  }
}

// 登录后把本地库里的历史协作会话补进列表（换机／重启后会话入口不丢）。
// 群会话（"group:N"）不入单聊列表——群列表由 GROUP_DATA 单独维护（T4.1）。
void MainWindow::seed_collab_peers() {
  if (LocalStore* store = direct_engine_.store()) {
    const QStringList hist = store->peers(QStringLiteral("collab"));
    for (const QString& p : hist) {
      if (!p.startsWith(QStringLiteral("group:"))) collab_peers_.insert(p);
    }
  }
}

void MainWindow::show_collab_login_dialog() {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("登录协作态"));
  auto* form = new QFormLayout(&dlg);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* host = new QLineEdit(
      settings.value(QStringLiteral("host"), QStringLiteral("127.0.0.1"))
          .toString(),
      &dlg);
  auto* port = new QLineEdit(
      settings.value(QStringLiteral("port"), QStringLiteral("24360")).toString(),
      &dlg);
  auto* account = new QLineEdit(
      settings.value(QStringLiteral("account")).toString(), &dlg);
  auto* password = new QLineEdit(&dlg);
  password->setEchoMode(QLineEdit::Password);
  form->addRow(QStringLiteral("服务器地址"), host);
  form->addRow(QStringLiteral("端口"), port);
  form->addRow(QStringLiteral("账号"), account);
  form->addRow(QStringLiteral("口令"), password);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  if (account->text().trimmed().isEmpty()) return;
  settings.setValue(QStringLiteral("host"), host->text().trimmed());
  settings.setValue(QStringLiteral("port"), port->text().trimmed());
  settings.setValue(QStringLiteral("account"), account->text().trimmed());
  login_collab(host->text().trimmed(),
               static_cast<quint16>(port->text().toUInt()),
               account->text().trimmed(), password->text());
}

void MainWindow::refresh_devices() {
  const QString selected = current_peer_;
  device_list_->blockSignals(true);
  device_list_->clear();
  const auto peers = direct_engine_.peers();
  for (const Peer& p : peers) {
    auto* item = new QListWidgetItem(device_list_);
    const QString id = QString::fromStdString(p.device_id);
    const QString name =
        p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    // T4.2：对端登录账号入列表行（仅作显示；空=未登录）
    const QString acct = p.account.empty()
                             ? QString()
                             : QStringLiteral(" · 协作账号 %1")
                                   .arg(QString::fromStdString(p.account));
    item->setText(QStringLiteral("%1\n%2 · TCP %3%4")
                      .arg(name, p.address.toString(),
                           QString::number(p.tcp_port), acct));
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, QStringLiteral("direct"));
  }

  // 协作会话分组（登录后出现；本地历史 + 本会话窗口期的对端）
  if (collab_engine_.is_logged_in() && !collab_peers_.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("协作会话 · 服务端归档"));
    header->setFlags(Qt::NoItemFlags); // 分组标题：不可选（空 id 不进会话）
    device_list_->addItem(header);
    QStringList sorted(collab_peers_.begin(), collab_peers_.end());
    // T4.5：星标置顶（最近优先）、其余按最近联系、再按账号名
    std::sort(sorted.begin(), sorted.end(), [this](const QString& a, const QString& b) {
      const bool sa = fav_is_starred_(a), sb = fav_is_starred_(b);
      if (sa != sb) return sa;
      const qint64 ta = fav_last_ms_(a), tb = fav_last_ms_(b);
      if (ta != tb) return ta > tb;
      return a < b;
    });
    for (const QString& account : sorted) {
      auto* item = new QListWidgetItem(device_list_);
      // T4.3：在线标识随服务端推送刷新（在线表含自己；未登录不显示本分组）
      const QString presence = online_accounts_.contains(account)
                                   ? QStringLiteral("●在线")
                                   : QStringLiteral("○离线");
      const QString star = fav_is_starred_(account)
                               ? QStringLiteral("★ ")
                               : QString();
      item->setText(QStringLiteral("%1%2\n协作态 · 已归档 · %3")
                        .arg(star, account, presence));
      item->setData(Qt::UserRole, account);
      item->setData(Qt::UserRole + 1, QStringLiteral("collab"));
    }
  }

  // 群聊分组（T4.1）：服务端群，消息经服务端扇出并全量归档
  if (collab_engine_.is_logged_in() && !groups_.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("群聊 · 服务端归档"));
    header->setFlags(Qt::NoItemFlags);
    device_list_->addItem(header);
    for (auto it = groups_.constBegin(); it != groups_.constEnd(); ++it) {
      auto* item = new QListWidgetItem(device_list_);
      item->setText(QStringLiteral("%1\n%2 人 · 群 %3 · 已归档")
                        .arg(it->name)
                        .arg(it->members.size())
                        .arg(it.key()));
      item->setData(Qt::UserRole,
                    QStringLiteral("group:%1").arg(it.key()));
      item->setData(Qt::UserRole + 1, QStringLiteral("group"));
    }
  }

  // 免服务端临时群（直连态，T4.1）：点对点扇出，不进归档
  if (!dgroup_members_.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("临时群 · 不进归档"));
    header->setFlags(Qt::NoItemFlags);
    device_list_->addItem(header);
    for (auto it = dgroup_members_.constBegin(); it != dgroup_members_.constEnd();
         ++it) {
      auto* item = new QListWidgetItem(device_list_);
      item->setText(QStringLiteral("%1\n%2 台设备 · 点对点扇出 · 不归档")
                        .arg(dgroup_title(it.key()))
                        .arg(it->size()));
      item->setData(Qt::UserRole, it.key());
      item->setData(Qt::UserRole + 1, QStringLiteral("dgroup"));
    }
  }
  device_list_->blockSignals(false);
  device_count_->setText(peers.isEmpty()
                             ? QStringLiteral("未发现设备")
                             : QStringLiteral("%1 台在线").arg(peers.size()));

  // 当前会话设备离线 → 保留视图但提示；列表选中态恢复
  if (!selected.isEmpty()) {
    for (int i = 0; i < device_list_->count(); ++i) {
      if (device_list_->item(i)->data(Qt::UserRole).toString() == selected) {
        device_list_->setCurrentRow(i, QItemSelectionModel::NoUpdate);
        break;
      }
    }
  }

  // 未选中会话时的空态：无设备显示发现引导；有设备提示先选择
  if (selected.isEmpty()) {
    if (peers.isEmpty()) {
      show_guidance();
    } else if (chat_showing_guidance_) {
      chat_showing_guidance_ = false;
      chat_view_->clear();
      append_system_line(QStringLiteral("从左侧选择设备，开始点对点会话"));
    }
  }
}

void MainWindow::open_peer(const QString& device_id) {
  open_chat(QStringLiteral("direct"), device_id);
}

// 打开会话（kind=direct 设备 / collab 账号）：历史取自共享本地库——
// 直连与协作消息同库合并，按 source 字段标注来源（T2.4 合并展示）。
void MainWindow::open_chat(const QString& kind, const QString& id) {
  current_kind_ = kind;
  current_peer_ = id;
  chat_showing_guidance_ = false;
  chat_view_->clear();

  const bool collab = kind == QStringLiteral("collab");
  const bool group = kind == QStringLiteral("group");
  const QString my_id = (collab || group)
                             ? collab_engine_.account()
                             : QString::fromStdString(direct_engine_.device_id());
  if (collab) {
    chat_title_->setText(id);
    chat_meta_->setText(QStringLiteral("%1 · 协作态 · 消息进入服务端归档")
                            .arg(id));
  } else if (group) {
    // 服务端群会话（T4.1）：标题群名，公告与成员数入 meta
    const quint64 gid = id.mid(QStringLiteral("group:").size()).toULongLong();
    const GroupEntry g = groups_.value(gid);
    chat_title_->setText(g.name.isEmpty()
                             ? QStringLiteral("群 %1").arg(gid)
                             : g.name);
    QString meta = QStringLiteral("%1 人 · 群 %2 · 协作态 · 消息进入服务端归档")
                       .arg(g.members.size())
                       .arg(gid);
    if (!g.announcement.isEmpty()) {
      meta.prepend(QStringLiteral("公告：%1　·　").arg(g.announcement));
    }
    chat_meta_->setText(meta);
  } else if (kind == QStringLiteral("dgroup")) {
    chat_title_->setText(dgroup_title(id));
    chat_meta_->setText(
        QStringLiteral("%1 台设备 · 点对点扇出 · 消息不进归档；对端回复落在"
                       "与其的单聊会话")
            .arg(dgroup_members_.value(id).size()));
  } else {
    const Peer p = direct_engine_.peer(id.toStdString());
    const QString name = p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    chat_title_->setText(name);
    // T4.2：跨态会话 meta 固定「跨态 · 未归档」前缀（与横幅同口径，A7）
    if (is_cross_state(id)) {
      chat_meta_->setText(
          QStringLiteral("跨态 · 未归档 · %1 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    } else {
      chat_meta_->setText(
          QStringLiteral("%1 · 点对点直连 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    }
  }
  update_banner();

  // T4.2 跨态会话固定标识（A7）＋归档起点提示（A8 客户端面）：
  // 标识常驻不可关闭，无论历史空否都打。
  if (kind == QStringLiteral("direct") && is_cross_state(id)) {
    if (collab_engine_.is_logged_in()) {
      append_system_line(QStringLiteral(
          "跨态会话 · 未归档：与未登录终端点对点传输（标记常驻不可关闭）；"
          "归档自 %1（本次协作态登录时刻，此前跨态直连消息不进归档）")
                             .arg(QDateTime::fromMSecsSinceEpoch(collab_login_ms_)
                                      .toString(QStringLiteral(
                                          "yyyy-MM-dd HH:mm:ss"))));
    } else {
      append_system_line(QStringLiteral(
          "跨态会话 · 未归档：对端已登录协作态，本机未登录——本机消息不进归档"
          "（标记常驻不可关闭）"));
    }
  }

  const auto hist = direct_engine_.history(id);
  if (hist.isEmpty()) {
    if (collab) {
      append_system_line(
          QStringLiteral("已与 %1 建立协作会话 · 消息经服务端转发并全量归档")
              .arg(esc(id)));
    } else if (group) {
      append_system_line(
          QStringLiteral("已打开群「%1」· 消息经服务端按成员扇出并全量归档")
              .arg(esc(chat_title_->text())));
    } else if (kind == QStringLiteral("dgroup")) {
      append_system_line(
          QStringLiteral("已建立临时群「%1」（%2 台设备）· 点对点扇出，"
                         "本会话不进归档")
              .arg(esc(chat_title_->text()))
              .arg(dgroup_members_.value(id).size()));
    } else {
      append_system_line(
          QStringLiteral("已与 %1 建立点对点会话 · 本对话不归档")
              .arg(esc(chat_title_->text())));
    }
  }
  for (const StoredMessage& m : hist) {
    append_message(QString::fromStdString(m.from),
                   QString::fromStdString(m.text), m.ts_ms,
                   m.from == my_id.toStdString(),
                   QString::fromStdString(m.source));
  }
  // T4.3：打开协作单聊=已读历史——对最近一条收到的协作消息上报已读
  //（打开前到达的消息此前未上报；逐条全报是噪音，只报最新一条）。
  if (collab) {
    for (auto it = hist.crbegin(); it != hist.crend(); ++it) {
      if (it->from == id.toStdString() && !it->msg_id.empty()) {
        collab_engine_.mark_read(QString::fromStdString(it->msg_id));
        break;
      }
    }
  }
}

void MainWindow::append_message(const QString& from_id, const QString& text,
                                qint64 ts_ms, bool outgoing,
                                const QString& source) {
  QString name = from_id;
  if (!outgoing) {
    if (current_kind_ == QStringLiteral("collab") ||
        current_kind_ == QStringLiteral("group")) {
      name = from_id; // 协作单聊／群聊对端即账号（群内显账号便于 @）
    } else {
      const Peer p = direct_engine_.peer(from_id.toStdString());
      if (!p.name.empty()) name = QString::fromStdString(p.name);
    }
  } else {
    name = QStringLiteral("我");
  }
  // 来源字段（合并展示）：直连＝仅本机；协作＝服务端归档
  const QString tag =
      source == QStringLiteral("collab")
          ? QStringLiteral(" · 协作·已归档")
          : QStringLiteral(" · 直连·仅本机");
  // 群聊启用 @成员 高亮（T4.1）
  chat_view_->append(bubble_html(name + tag, text, ts_ms, outgoing,
                                 current_kind_ == QStringLiteral("group")));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::append_system_line(const QString& text) {
  chat_view_->append(QStringLiteral(
      "<div align=\"center\"><span style=\"color:#9b8f86; "
      "font-size:small;\">%1</span></div>")
                         .arg(text));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::show_guidance() {
  chat_showing_guidance_ = true;
  chat_view_->clear();
  chat_view_->append(QStringLiteral(
      "<div align=\"center\" style=\"margin-top:48px;\">"
      "<span style=\"color:#6b6157;\">尚未发现同网段设备。</span><br><br>"
      "<span style=\"color:#9b8f86; font-size:small;\">"
      "请确认对方已安装 Memex 且与本机同一局域网；<br>"
      "直连发现使用 UDP 2425、点对点传输使用 TCP 2426–2437，"
      "请检查终端防火墙放行。<br><br>"
      "如需组织架构、云端历史与归档检索，请经左上角「协作」菜单登录协作态。"
      "</span></div>"));
}

void MainWindow::show_status(const QString& text) {
  status_hint_ = text;
  statusBar()->showMessage(
      QString::fromStdString(direct_engine_.status_text()) +
          QStringLiteral("　|　") +
          QString::fromStdString(collab_engine_.status_text()) +
          QStringLiteral("　|　") + status_hint_,
      0);
}

} // namespace memex::client
