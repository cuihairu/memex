// Memex 客户端入口（T1.4 直连态界面）。
// --smoke：烟测模式，起窗后自动退出（ctest 用，offscreen 平台）。
// --screenshot <path>：起窗 4.5s（含设备发现窗口期）截图后退出。
#include <QApplication>
#include <QTimer>

#include <iostream>
#include <string_view>

#include "crash_report.hpp"
#include "main_window.hpp"
#include "theme.hpp"

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--version") {
    std::cout << "memex-client " << MEMEX_VERSION << std::endl;
    return 0;
  }

  QString screenshot_path;
  bool smoke = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--smoke") {
      smoke = true;
    } else if (arg == "--screenshot" && i + 1 < argc) {
      screenshot_path = QString::fromUtf8(argv[++i]);
    }
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName("Memex");
  QApplication::setOrganizationName("memex");

  // 崩溃采集（Crashpad）：QApplication 构造后、业务引擎/窗口构造前接管
  // 异常路径——启动早期崩溃也能落 dump；只本地落盘不外发（简档
  // docs/src/guide/crash-reporting.md）。
  memex::client::init_crash_reporting();

  // 故意崩溃验收开关（简档是唯一入口，不进任何菜单）：MEMEX_CRASH_TEST=1
  // 时 2s 后空指针写入，验证 handler 接管与 dump 落盘；默认零影响。
  if (qEnvironmentVariable("MEMEX_CRASH_TEST") == QLatin1String("1")) {
    QTimer::singleShot(2000, &app, [] {
      qWarning("[崩溃采集] MEMEX_CRASH_TEST=1 触发故意崩溃（空指针写入）");
      *static_cast<volatile int*>(nullptr) = 0;
    });
  }

  // 主题（R19）：起窗前应用令牌化调色板与全局 QSS；跟随系统时系统亮暗变化
  // 由 ThemeManager 自动重应用。窗口内控件各自设的样式优先级更高，
  // 不受此处影响。
  memex::client::ThemeManager::instance().apply(&app);

  memex::client::MainWindow window;
  window.show();

  if (smoke) {
    QTimer::singleShot(500, &app, &QApplication::quit);
  }
  if (!screenshot_path.isEmpty()) {
    QTimer::singleShot(4500, &window, [&window, screenshot_path] {
      window.grab().save(screenshot_path);
      QApplication::quit();
    });
  }

  return QApplication::exec();
}
