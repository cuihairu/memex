// 系统集成（LAY-002 拆分）：托盘、系统通知与闪烁、关于页、离开
// 锁屏、开机启动与 closeEvent。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QIcon>
#include <QTimer>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTextStream>
#include "notify_center.hpp"
#include "notify_prefs.hpp"
#ifndef MEMEX_VERSION // 测试目标未传版本定义时兜底（与 main.cpp 同款）
#define MEMEX_VERSION "dev"
#endif
namespace memex::client {

// —— T4.7 系统集成 ——

QString MainWindow::last_notify() const { return last_notify_; }

bool MainWindow::tray_available() const {
  return tray_ != nullptr && QSystemTrayIcon::isSystemTrayAvailable();
}

// A23 图标面：关于页——logo（qrc 多尺寸，与窗口/任务栏/安装器同源）＋版本
void MainWindow::show_about() {
  QDialog dlg(this);
  dlg.setObjectName(QStringLiteral("about_dialog"));
  dlg.setWindowTitle(QStringLiteral("关于 Memex"));
  auto* lay = new QVBoxLayout(&dlg);
  auto* logo = new QLabel(&dlg);
  logo->setObjectName(QStringLiteral("about_logo"));
  logo->setPixmap(brand_icon().pixmap(128, 128));
  logo->setAlignment(Qt::AlignCenter);
  lay->addWidget(logo);
  auto* name =
      new QLabel(QStringLiteral("<b>Memex</b> 内网办公即时通讯"), &dlg);
  name->setAlignment(Qt::AlignCenter);
  lay->addWidget(name);
  auto* ver = new QLabel(
      QStringLiteral("版本 %1").arg(QStringLiteral(MEMEX_VERSION)), &dlg);
  ver->setObjectName(QStringLiteral("about_version"));
  ver->setAlignment(Qt::AlignCenter);
  lay->addWidget(ver);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::close);
  lay->addWidget(buttons);
  dlg.setMinimumWidth(320);
  dlg.exec();
}

// BUG-004 托盘激活：隐藏→弹出、最小化→还原（保留最大化态），随后置顶聚焦。
// 三平台同一路径（Qt 抽象层），无平台分支。
void MainWindow::activate_from_tray() {
  setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
  show();
  raise();
  activateWindow();
}

// BUG-004 主流托盘交互：单击/双击都激活主界面（Qt 惯例，微信/企业微信同类）；
// 右键（Context）留给上下文菜单，中键/未知不响应。
// 「最小化到托盘」气泡（closeEvent 里 tray_notify）在托盘图标上方挡 5 秒，
// 第一击常落在气泡上而非图标——气泡点击（messageClicked）同样激活主界面，
// 保证隐藏后第一击必弹（实测：不接线时第一击只把气泡点没，窗口不出）。
void MainWindow::wire_tray_activation(QSystemTrayIcon* tray) {
  connect(tray, &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger ||
                reason == QSystemTrayIcon::DoubleClick) {
              activate_from_tray();
            }
          });
  connect(tray, &QSystemTrayIcon::messageClicked, this,
          [this] { activate_from_tray(); });
}

void MainWindow::setup_tray() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) return; // 无托盘环境跳过
  tray_ = new QSystemTrayIcon(brand_icon(), this);
  tray_->setToolTip(QStringLiteral("Memex（直连态：消息不进归档）"));
  auto* menu = new QMenu(this);
  auto* act_show = menu->addAction(QStringLiteral("显示主窗口"));
  connect(act_show, &QAction::triggered, this, [this] { activate_from_tray(); });
  auto* act_tray_auto = menu->addAction(QStringLiteral("开机启动"));
  act_tray_auto->setCheckable(true);
  act_tray_auto->setChecked(autostart_enabled());
  connect(act_tray_auto, &QAction::triggered, this, [this, act_tray_auto](bool on) {
    set_autostart(on);
    act_tray_auto->setChecked(autostart_enabled());
    if (act_autostart_) act_autostart_->setChecked(autostart_enabled());
  });
  auto* act_notify_prefs = menu->addAction(QStringLiteral("通知偏好"));
  connect(act_notify_prefs, &QAction::triggered, this, [] {
    NotificationCenter::instance().show_settings();
  });
  auto* act_quit = menu->addAction(QStringLiteral("退出"));
  connect(act_quit, &QAction::triggered, this, &MainWindow::quit_app);
  tray_->setContextMenu(menu);
  tray_->show();
  wire_tray_activation(tray_);
}

// 托盘菜单「退出」真身（2026-10-11 用户实录：窗口最小化到托盘的隐藏态
// 点退出，进程不退——close() 不可见窗口不触发 lastWindowClosed（Qt 的
// 「最后一个可见窗口关闭」语义），仅摘窗不退进程＝无窗运行态，托盘图标
// 还在（QSystemTrayIcon 对象未析构），用户得把窗口拉起来再退一次才死。
// 退出即显式 quit：不依赖窗口可见性。先摘 tray_ 让 closeEvent 不再按
// 「最小化到托盘」分支拦截，close() 走完关闭语义（隐藏＋收尾）再退。
void MainWindow::quit_app() {
  tray_ = nullptr;
  close();
  QApplication::quit();
}

// 新消息闪烁提醒（用户令 2026-10-05）：QWindow::alert 走平台原生注意 API
// ——Windows FlashWindowEx／macOS requestUserAttention／X11 urgency 任务栏
// 闪烁；Wayland 无该协议（平台降级边界，BUGS/走查注明）。开关＝通知偏好
// 的 flash_alert（默认开），关了完全不闪；合并窗内只触发一次不叠加。
void MainWindow::alert_attention() {
  if (!NotifyPrefs::load().flash_alert) return;
  if (isActiveWindow()) return;   // 激活中不闪（触发点已判，双保险）
  if (alert_active_) return;      // 合并窗内不叠加
  alert_active_ = true;
  ++alert_count_;
  if (QWindow* handle = windowHandle()) handle->alert(kFlashAlertMs);
  QTimer::singleShot(kFlashAlertMs, this, [this] { alert_active_ = false; });
}

int MainWindow::alert_count() const { return alert_count_; }

void MainWindow::tray_notify(const QString& title, const QString& text) {
  last_notify_ = title + QStringLiteral("：") + text; // 无托盘也记录（断言面）
  if (tray_ && QSystemTrayIcon::supportsMessages()) {
    tray_->showMessage(title, text, QSystemTrayIcon::Information, 5000);
  }
}

void MainWindow::event_notify(bool enabled, SoundEvent ev,
                              const QString& title, const QString& text) {
  // 锁屏期间（用户令 2026-10-08 ④）：内容不出锁屏面——弹窗与提示音全静默，
  // 只累计未读条数（上线不是消息不计）
  if (lock_) {
    if (ev != SoundEvent::Online) lock_->bump_unread();
    return;
  }
  const NotifyPrefs p = NotifyPrefs::load();
  if (enabled) tray_notify(title, text); // 弹窗受事件开关裁决
  // 提示音三档独立裁决（弹窗关了声音仍可按档播）；beep 兜底＝无
  // Multimedia 依赖（真音频文件走 Qt Multimedia 另批，文档注明）
  if (sound_should_play(p.sound_mode, p.sound_msg, p.sound_file,
                        p.sound_online, ev)) {
    QApplication::beep();
  }
}

// —— 离开锁屏（用户令 2026-10-08 ④）——
// 全局输入事件喂计时器：鼠标移动/点击、键盘、滚轮任一即重计。
// 锁屏遮罩在屏时不再重置（锁已锁上，输密码动作不算「回来了」）。
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  // @ 成员菜单（Qt::Popup）抢键盘：键入转发回输入框（连续输入继续按词头
  // 过滤），菜单重建延到事件循环空转帧——分发中清 QMenu actions 会悬垂
  // （textChanged 在转发栈内同步触发，见 rebuild_at_menu 调用方）
  if (watched == at_menu_ && event->type() == QEvent::KeyPress) {
    auto* ke = static_cast<QKeyEvent*>(event);
    if (ke->key() == Qt::Key_Escape) return false; // 菜单自己收（不吞 Esc）
    if (input_box_) {
      in_at_key_forward_ = true;
      input_box_->event(event); // 键入回输入框（textChanged 被标记拦下）
      in_at_key_forward_ = false;
      QTimer::singleShot(0, this, [this] {
        if (at_menu_) rebuild_at_menu(input_box_->text());
      });
    }
    return true;
  }
  switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::Wheel:
      if (idle_timer_ && !lock_) idle_timer_->start();
      break;
    default:
      break;
  }
  return QMainWindow::eventFilter(watched, event);
}

// 重读 away_lock 设置（设置对话框关闭/测试接线即调＝即时生效）：
// 计时器间隔换算毫秒重启；未启用或没密码则停表（没密码锁不住人，不装锁）。
void MainWindow::apply_away_lock_settings() {
  if (!idle_timer_) return;
  if (away_lock::enabled() && away_lock::has_password()) {
    idle_timer_->start(away_lock::timeout_seconds() * 1000);
  } else {
    idle_timer_->stop();
  }
}

// 重读 net_blacklist：开关关＝不装过滤器（全放行）；开＝段表转谓词注入
// 直连引擎（发现包 sender 与 TCP 入站 peerAddress 两处同一谓词）。
// CIDR 匹配只在 net_guard 内实现，这里只拼装不重复实现。
void MainWindow::apply_net_settings() {
  direct_engine_.set_ipmsg_enabled(ipmsg_compat::enabled()); // 飞秋兼容即时生效
  if (!net_blacklist::enabled()) {
    direct_engine_.set_address_filter({});
    return;
  }
  const QStringList cidrs = net_blacklist::entries();
  direct_engine_.set_address_filter(
      [cidrs](const QHostAddress& addr) {
        return !net_guard::blocked(cidrs, addr); // 谓词=放行；黑名单命中=false 拒
      });
}

void MainWindow::maybe_lock_screen() {
  if (lock_) return;
  if (!away_lock::enabled() || !away_lock::has_password()) return;
  lock_ = new LockScreenDialog(
      [](const QString& pwd) { return away_lock::verify_password(pwd); },
      this);
  connect(lock_, &QDialog::finished, this, [this](int) {
    lock_->deleteLater();
    lock_ = nullptr;
    apply_away_lock_settings(); // 解锁后重新计时（下次离开再锁）
  });
  lock_->showFullScreen(); // 无边框置顶全屏；Esc/close 拒，唯一出路＝密码
}

bool MainWindow::lock_screen_visible() const {
  return lock_ && lock_->isVisible();
}

int MainWindow::lock_unread_count() const {
  return lock_ ? lock_->unread_count() : 0;
}

QString MainWindow::lock_screen_text() const {
  return lock_ ? lock_->unread_text() : QString();
}

void MainWindow::lock_try_unlock(const QString& pwd) {
  if (lock_) lock_->try_unlock(pwd);
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
  save_window_geometry(); // 真退出：存窗口几何，下次启动恢复（首次启动用默认）
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
      << "Icon=memex\n" // hicolor 图标（deb/rpm 装到 /usr/share/icons/hicolor）
      << "Comment=Memex 内网办公即时通讯\n"
      << "Exec=" << QCoreApplication::applicationFilePath() << "\n"
      << "Terminal=false\n"
      << "Categories=Network;InstantMessaging;\n"
      << "X-GNOME-Autostart-enabled=true\n";
  f.close();
  if (act_autostart_) act_autostart_->setChecked(true);
}

} // namespace memex::client
