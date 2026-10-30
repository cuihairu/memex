// 二期·统一设置窗口冒烟（原型 settings-general.html）：
// 侧栏导航五页堆叠（通用/账号/消息/隐私/高级）＋通用页真实接线
// （主题下拉→ThemeManager、截图快捷键→MainWindow::screenshot_shortcut、
// 开机启动→set_autostart（freedesktop 文件，MEMEX_TEST_AUTOSTART_DIR 隔离）、
// 下载位置只读真实目录；界面字号/语言置灰「未提供」）＋四页诚实占位。
// 无服务端（MainWindow 独立构造，同 test_window_layout）；离屏。
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>
#include <unistd.h>

#include <app/main_window.hpp>
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
  }

  std::cout << (g_failures == 0 ? "OK" : "FAILURES") << "\n";
  return g_failures == 0 ? 0 : 1;
}
