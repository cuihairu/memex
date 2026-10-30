// 二期·统一设置窗口冒烟（原型 settings-general/account/message/privacy
// /advanced.html）：侧栏导航五页堆叠（通用/账号/消息/隐私/高级）＋通用页
// 真实接线（主题下拉→ThemeManager、截图快捷键→
// MainWindow::screenshot_shortcut、开机启动→set_autostart（freedesktop
// 文件，MEMEX_TEST_AUTOSTART_DIR 隔离）、下载位置只读真实目录；界面字号/
// 语言置灰「未提供」）＋账号页登录态回填＋消息页 NotifyPrefs 直写
// （load→改→save 保未展示字段；已读回执走主窗 receipts/send_read 全局键；
// 迁移备份置灰「暂未提供」）＋隐私页主窗共用入口（网段黑名单/离开锁屏
// 点击真开对话框；存储说明列真实本机目录）＋高级页三组接线（网段黑名单
// 开关→net_blacklist 真实键直写；版本标签列 MEMEX_VERSION；域名解析覆盖/
// 关于按钮接主窗入口但不点击——目标对话框 exec() 模态会卡死离屏测试）。
// 无服务端（MainWindow 独立构造，同 test_window_layout）；离屏。
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTime>
#include <QTimeEdit>

#include <iostream>
#include <unistd.h>

#include <app/away_settings.hpp>
#include <app/main_window.hpp>
#include <app/net_guard.hpp>
#include <app/net_remote_settings.hpp>
#include <app/notify_prefs.hpp>
#include <app/settings_window.hpp>
#include <app/theme.hpp>

#ifndef MEMEX_DOCS_SHOT_DIR
#error "MEMEX_DOCS_SHOT_DIR 未定义"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

QSettings& test_settings() {
  static QSettings s(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  return s;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  // QSettings 隔离：org/app 独立命名带 pid＋Ini/Native 双格式重定向临时
  // 目录（主窗/主题/快捷键内部 QSettings 走默认/双参构造＝NativeFormat，
  // 只重定向 IniFormat 管不住——上轮实录存档漏进 ~/.config 跨跑串味）。
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(
      QStringLiteral("settings-win-test-%1").arg(static_cast<int>(::getpid())));
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QByteArray settings_dir =
      tmp.filePath(QStringLiteral("settings")).toUtf8();
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir);
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                     settings_dir);
  qputenv("MEMEX_TEST_BRAND_DIR",
          tmp.filePath(QStringLiteral("brand-cache")).toUtf8());
  // 开机启动隔离：autostart_dir 由 MEMEX_TEST_AUTOSTART_DIR 接管（环境变量
  // 在首次 autostart_dir() 调用前设置；autostart_enabled 检查该目录文件）
  const QString autostart_dir = tmp.filePath(QStringLiteral("autostart"));
  qputenv("MEMEX_TEST_AUTOSTART_DIR", autostart_dir.toUtf8());
  test_settings().clear();

  // 无服务端独立构造（同 test_window_layout）；SettingsWindow 带主窗指针
  // 才能接线开机启动/快捷键
  memex::client::MainWindow w;
  memex::client::SettingsWindow sw(&w, nullptr);

  // —— ① 导航：默认通用页；show_page 五页互切；未知 id 拒 ——
  CHECK(sw.current_page() == QStringLiteral("general"));
  CHECK(sw.page_title() == QStringLiteral("通用"));
  CHECK(sw.show_page(QStringLiteral("account")));
  CHECK(sw.current_page() == QStringLiteral("account"));
  CHECK(sw.page_title() == QStringLiteral("账号"));
  CHECK(sw.show_page(QStringLiteral("message")));
  CHECK(sw.current_page() == QStringLiteral("message"));
  CHECK(sw.show_page(QStringLiteral("privacy")));
  CHECK(sw.current_page() == QStringLiteral("privacy"));
  CHECK(sw.show_page(QStringLiteral("advanced")));
  CHECK(sw.current_page() == QStringLiteral("advanced"));
  CHECK(sw.page_title() == QStringLiteral("高级"));
  CHECK(!sw.show_page(QStringLiteral("bogus")));
  CHECK(sw.show_page(QStringLiteral("general")));

  // —— ② 通用页控件存在（objectName 定位） ——
  auto* theme_combo = sw.findChild<QComboBox*>(QStringLiteral("settings_theme"));
  auto* autostart_check =
      sw.findChild<QCheckBox*>(QStringLiteral("settings_autostart"));
  auto* shortcut_btn =
      sw.findChild<QPushButton*>(QStringLiteral("settings_shortcut"));
  auto* download_dir =
      sw.findChild<QLineEdit*>(QStringLiteral("settings_download_dir"));
  CHECK(theme_combo);
  CHECK(autostart_check);
  CHECK(shortcut_btn);
  CHECK(download_dir);
  if (!(theme_combo && autostart_check && shortcut_btn && download_dir)) {
    std::cerr << "controls missing\n";
    return 1;
  }

  // —— ③ 主题：默认随 ThemeManager；选暗色即生效并可回读 ——
  const QString orig_mode = memex::client::ThemeManager::instance().mode();
  CHECK(sw.theme_mode() == orig_mode);
  CHECK(theme_combo->count() == 3);
  const int dark_idx = theme_combo->findData(QStringLiteral("dark"));
  CHECK(dark_idx >= 0);
  if (dark_idx >= 0) {
    theme_combo->setCurrentIndex(dark_idx);
    CHECK(memex::client::ThemeManager::instance().mode() ==
          QStringLiteral("dark"));
    CHECK(sw.theme_mode() == QStringLiteral("dark"));
    const int sys_idx = theme_combo->findData(orig_mode);
    if (sys_idx >= 0) theme_combo->setCurrentIndex(sys_idx);
    CHECK(memex::client::ThemeManager::instance().mode() == orig_mode);
  }

  // —— ④ 下载位置：只读真实系统下载目录 ——
  CHECK(download_dir->isReadOnly());
  CHECK(sw.download_dir_text() ==
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));

  // —— ⑤ 快捷键：与主窗权威口径一致（静态读 QSettings，隔离默认
  //    Ctrl+Alt+A）；带主窗＝按钮可用 ——
  CHECK(shortcut_btn->isEnabled());
  CHECK(sw.shortcut_text() == memex::client::MainWindow::screenshot_shortcut());

  // —— ⑥ 开机启动：初始无自启项；勾选→写 freedesktop .desktop→可回读；
  //    取消→删除——与 MainWindow::autostart_enabled 同源文件 ——
  CHECK(!QFile::exists(autostart_dir + QStringLiteral("/memex-client.desktop")));
  CHECK(!sw.autostart_checked());
  autostart_check->setChecked(true);
  CHECK(QFile::exists(autostart_dir + QStringLiteral("/memex-client.desktop")));
  CHECK(sw.autostart_checked());
  CHECK(w.autostart_enabled());
  autostart_check->setChecked(false);
  CHECK(!QFile::exists(autostart_dir + QStringLiteral("/memex-client.desktop")));
  CHECK(!sw.autostart_checked());
  CHECK(!w.autostart_enabled());

  // —— ⑦ win 空＝测试隔离：开机启动/快捷键行置灰且不开写 ——
  {
    memex::client::SettingsWindow sw0(nullptr, nullptr);
    auto* check0 =
        sw0.findChild<QCheckBox*>(QStringLiteral("settings_autostart"));
    auto* shortcut0 =
        sw0.findChild<QPushButton*>(QStringLiteral("settings_shortcut"));
    CHECK(check0);
    CHECK(shortcut0);
    if (check0 && shortcut0) {
      CHECK(!check0->isEnabled());
      CHECK(!shortcut0->isEnabled());
    }
    CHECK(!sw0.autostart_checked());
  }

  // —— ⑦b 账号页：无服务端＝未登录诚实态（头像空位＋「未登录」＋登录态
  //    行置灰）；控件齐三组结构 ——
  {
    CHECK(sw.show_page(QStringLiteral("account")));
    auto* name =
        sw.findChild<QLineEdit*>(QStringLiteral("settings_acct_name"));
    auto* avatar_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_acct_avatar_btn"));
    auto* logout_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_acct_logout_btn"));
    CHECK(name);
    CHECK(avatar_btn);
    CHECK(logout_btn);
    if (name && avatar_btn && logout_btn) {
      CHECK(name->text() == QStringLiteral("未登录"));
      CHECK(!avatar_btn->isEnabled());
      CHECK(!logout_btn->isEnabled());
    }
    CHECK(sw.show_page(QStringLiteral("general")));
  }

  // —— ⑦c 消息页：NotifyPrefs 直写（load→改→save 保未展示字段）＋
  //    已读回执走主窗全局键；默认态与原型展示态一致 ——
  {
    CHECK(sw.show_page(QStringLiteral("message")));
    CHECK(sw.page_title() == QStringLiteral("消息"));
    const memex::client::NotifyPrefs def;
    CHECK(sw.msg_desktop_notif() == def.popup_message);
    CHECK(sw.msg_flash() == def.flash_alert);
    CHECK(sw.msg_peer_online() == def.popup_peer_online);
    CHECK(sw.msg_file_arrive() == def.popup_file_arrive);
    CHECK(sw.msg_sound_mode() == def.sound_mode);
    CHECK(sw.msg_dnd() == def.dnd);
    CHECK(sw.msg_dnd_start() == def.dnd_start);
    CHECK(sw.msg_dnd_end() == def.dnd_end);
    CHECK(sw.msg_read_receipts() == w.read_receipts_enabled(QString()));

    auto* desktop =
        sw.findChild<QCheckBox*>(QStringLiteral("settings_msg_desktop"));
    auto* sound =
        sw.findChild<QComboBox*>(QStringLiteral("settings_msg_sound"));
    auto* dnd_check =
        sw.findChild<QCheckBox*>(QStringLiteral("settings_msg_dnd"));
    auto* dnd_start =
        sw.findChild<QTimeEdit*>(QStringLiteral("settings_msg_dnd_start"));
    auto* dnd_end =
        sw.findChild<QTimeEdit*>(QStringLiteral("settings_msg_dnd_end"));
    auto* receipts =
        sw.findChild<QCheckBox*>(QStringLiteral("settings_msg_receipts"));
    auto* backup =
        sw.findChild<QPushButton*>(QStringLiteral("settings_msg_backup"));
    CHECK(desktop);
    CHECK(sound);
    CHECK(dnd_check);
    CHECK(dnd_start);
    CHECK(dnd_end);
    CHECK(receipts);
    CHECK(backup);
    if (backup) CHECK(!backup->isEnabled()); // 迁移备份＝诚实「暂未提供」

    if (desktop) {
      desktop->setChecked(false);
      CHECK(!memex::client::NotifyPrefs::load().popup_message);
      // 本页未展示的字段不被整存清掉（load→改→save）
      CHECK(memex::client::NotifyPrefs::load().flash_alert);
      CHECK(memex::client::NotifyPrefs::load().popup_important);
      desktop->setChecked(true);
      CHECK(memex::client::NotifyPrefs::load().popup_message);
    }
    if (sound) {
      sound->setCurrentIndex(1);
      CHECK(memex::client::NotifyPrefs::load().sound_mode == 1);
      sound->setCurrentIndex(0);
      CHECK(memex::client::NotifyPrefs::load().sound_mode == 0);
    }
    if (dnd_check && dnd_start && dnd_end) {
      dnd_check->setChecked(true);
      CHECK(memex::client::NotifyPrefs::load().dnd);
      dnd_start->setTime(QTime(23, 30));
      dnd_end->setTime(QTime(6, 15));
      const memex::client::NotifyPrefs p =
          memex::client::NotifyPrefs::load();
      CHECK(p.dnd_start == QStringLiteral("23:30"));
      CHECK(p.dnd_end == QStringLiteral("06:15"));
      // 还原默认（抓图态＝原型展示态：免打扰关 22:00–08:00）
      dnd_check->setChecked(false);
      dnd_start->setTime(QTime(22, 0));
      dnd_end->setTime(QTime(8, 0));
      CHECK(!memex::client::NotifyPrefs::load().dnd);
    }
    if (receipts) {
      receipts->setChecked(false);
      CHECK(!w.read_receipts_enabled(QString()));
      receipts->setChecked(true);
      CHECK(w.read_receipts_enabled(QString()));
    }
    CHECK(sw.show_page(QStringLiteral("general")));
  }

  // —— ⑦d 隐私页：网段黑名单/离开锁屏＝主窗共用入口（点击真开对话框）；
  //    存储说明列真实本机目录；加好友验证置灰「暂未提供」——
  {
    CHECK(sw.show_page(QStringLiteral("privacy")));
    CHECK(sw.page_title() == QStringLiteral("隐私"));
    auto* net_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_priv_net"));
    auto* friend_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_priv_friend"));
    auto* storage_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_priv_storage"));
    auto* away_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_priv_away"));
    auto* net_tag =
        sw.findChild<QLabel*>(QStringLiteral("settings_priv_net_tag"));
    auto* online_tag =
        sw.findChild<QLabel*>(QStringLiteral("settings_priv_online_tag"));
    CHECK(net_btn);
    CHECK(friend_btn);
    CHECK(storage_btn);
    CHECK(away_btn);
    CHECK(net_tag);
    CHECK(online_tag);
    if (net_tag)
      CHECK(net_tag->text() == QStringLiteral("当前提供网段黑名单"));
    if (online_tag) CHECK(online_tag->text() == QStringLiteral("自动"));
    if (friend_btn) CHECK(!friend_btn->isEnabled()); // 无好友验证流程＝置灰

    if (net_btn) {
      net_btn->click();
      QApplication::processEvents(QEventLoop::AllEvents, 30);
      auto* net_dlg = w.findChild<memex::client::NetRemoteSettingsDialog*>();
      CHECK(net_dlg);
      if (net_dlg) net_dlg->close(); // WA_DeleteOnClose（关即毁，不残留）
      QApplication::processEvents(QEventLoop::AllEvents, 30);
    }
    if (away_btn) {
      away_btn->click();
      QApplication::processEvents(QEventLoop::AllEvents, 30);
      auto* away_dlg = w.findChild<memex::client::AwayLockSettingsDialog*>();
      CHECK(away_dlg);
      if (away_dlg) away_dlg->close();
      QApplication::processEvents(QEventLoop::AllEvents, 30);
    }
    if (storage_btn) {
      storage_btn->click();
      QApplication::processEvents(QEventLoop::AllEvents, 30);
      auto* note = sw.findChild<QMessageBox*>(
          QStringLiteral("settings_priv_storage_note"));
      CHECK(note);
      if (note) {
        CHECK(note->informativeText().contains(QStringLiteral("本机数据目录")));
        note->close(); // WA_DeleteOnClose
      }
      QApplication::processEvents(QEventLoop::AllEvents, 30);
    }
    CHECK(sw.show_page(QStringLiteral("general")));
  }

  // —— ⑦e 高级页：黑名单开关→net_blacklist 真实键；版本标签＝真实
  //    MEMEX_VERSION；代理/开发者选项/运行日志置灰「暂未提供」；域名解析
  //    覆盖/关于按钮可用但不点击（exec() 模态卡死离屏测试）——
  {
    CHECK(sw.show_page(QStringLiteral("advanced")));
    CHECK(sw.page_title() == QStringLiteral("高级"));
    auto* proxy_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_adv_proxy"));
    auto* dns_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_adv_dns"));
    auto* dev_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_adv_dev"));
    auto* log_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_adv_log"));
    auto* about_btn =
        sw.findChild<QPushButton*>(QStringLiteral("settings_adv_about"));
    auto* blacklist =
        sw.findChild<QCheckBox*>(QStringLiteral("settings_adv_blacklist"));
    auto* ver_tag =
        sw.findChild<QLabel*>(QStringLiteral("settings_adv_ver_tag"));
    CHECK(proxy_btn);
    CHECK(dns_btn);
    CHECK(dev_btn);
    CHECK(log_btn);
    CHECK(about_btn);
    CHECK(blacklist);
    CHECK(ver_tag);
    if (proxy_btn) CHECK(!proxy_btn->isEnabled()); // 无代理配置面
    if (dev_btn) CHECK(!dev_btn->isEnabled());     // 无开发者选项面板
    if (log_btn) CHECK(!log_btn->isEnabled());     // 本版不写日志文件
    if (dns_btn) CHECK(dns_btn->isEnabled()); // 模态对话框：只验结构不点击
    if (about_btn) CHECK(about_btn->isEnabled());
    if (ver_tag)
      CHECK(ver_tag->text() == QStringLiteral(MEMEX_VERSION));
    if (blacklist) {
      CHECK(blacklist->isChecked() ==
            memex::client::net_blacklist::enabled()); // 建页回读真实键
      blacklist->setChecked(true);
      CHECK(memex::client::net_blacklist::enabled()); // 直写同一键
      blacklist->setChecked(false); // 还原默认关（抓图态＝原型 toggle off）
      CHECK(!memex::client::net_blacklist::enabled());
    }
    CHECK(sw.show_page(QStringLiteral("general")));
  }

  // —— ⑧ 文档站实况抓图（grab_pair 同式：亮暗各一、抓完显式回亮）——
  {
    const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
    CHECK(QDir().mkpath(dir));
    sw.show();
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    auto& tm = memex::client::ThemeManager::instance();
    tm.apply(qApp);
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-general-light.png")));
    tm.set_mode(QStringLiteral("dark"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-general-dark.png")));
    tm.set_mode(QStringLiteral("light"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);

    // 账号页抓图（未登录诚实态：头像空位＋「未登录」＋三组结构；登录态下
    // 头像/昵称/退出登录随真实状态回填，同布局）
    CHECK(sw.show_page(QStringLiteral("account")));
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.page_title() == QStringLiteral("账号"));
    tm.apply(qApp);
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-account-light.png")));
    tm.set_mode(QStringLiteral("dark"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-account-dark.png")));
    tm.set_mode(QStringLiteral("light"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);

    // 消息页抓图（默认态＝原型展示态：通知四开关开、提示音关闭、
    // 免打扰关、时段 22:00–08:00、已读回执开）
    CHECK(sw.show_page(QStringLiteral("message")));
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.page_title() == QStringLiteral("消息"));
    tm.apply(qApp);
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-message-light.png")));
    tm.set_mode(QStringLiteral("dark"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-message-dark.png")));
    tm.set_mode(QStringLiteral("light"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);

    // 隐私页抓图（默认态＝原型展示态：标签「当前提供网段黑名单」/「自动」）
    CHECK(sw.show_page(QStringLiteral("privacy")));
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.page_title() == QStringLiteral("隐私"));
    tm.apply(qApp);
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-privacy-light.png")));
    tm.set_mode(QStringLiteral("dark"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-privacy-dark.png")));
    tm.set_mode(QStringLiteral("light"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);

    // 高级页抓图（默认态＝原型展示态：黑名单开关关、版本标签真实版本号）
    CHECK(sw.show_page(QStringLiteral("advanced")));
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.page_title() == QStringLiteral("高级"));
    tm.apply(qApp);
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-advanced-light.png")));
    tm.set_mode(QStringLiteral("dark"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    CHECK(sw.grab().save(dir + QStringLiteral("/live-settings-advanced-dark.png")));
    tm.set_mode(QStringLiteral("light"));
    QThread::msleep(120);
    QApplication::processEvents(QEventLoop::AllEvents, 30);
  }

  std::cout << (g_failures == 0 ? "OK" : "FAILURES") << "\n";
  return g_failures == 0 ? 0 : 1;
}
