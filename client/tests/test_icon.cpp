// A23 图标面验收：窗口/任务栏/托盘/关于页同源 logo。
// ① qrc 资产：:/icons/memex-{16..512}.png 全部可载、尺寸相符、含品牌橙像素；
// ② 主窗 windowIcon 非空（资源在＝多尺寸真 logo）；
// ③ 帮助→「关于 Memex…」真弹：logo 标签有图、抓图入库（walkthrough 同源）；
// ④ 开机启动 .desktop 带 Icon=memex（Linux 快捷方式图标解析来源，XDG 隔离）。
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QPixmap>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <functional>

#include "main_window.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return true;
}

QWidget* find_top(const QString& name) {
  const auto tops = QApplication::topLevelWidgets();
  for (auto* w : tops) {
    if (w->objectName() == name) return w;
  }
  return nullptr;
}

// 品牌橙 #e16531 在图内出现（容差放宽容栅格化差异；logo 单色，命中即真）
bool has_brand_orange(const QImage& img) {
  for (int y = 0; y < img.height(); ++y) {
    for (int x = 0; x < img.width(); ++x) {
      const QColor c = img.pixelColor(x, y);
      if (c.alpha() < 200) continue;
      if (qAbs(c.red() - 0xE1) <= 25 && qAbs(c.green() - 0x65) <= 25 &&
          qAbs(c.blue() - 0x31) <= 25)
        return true;
    }
  }
  return false;
}

// —— ① qrc 资产多尺寸可载＋品牌橙恒定（R19：品牌色不许漂） ——
void test_resources() {
  for (int s : {16, 32, 48, 64, 128, 256, 512}) {
    const QPixmap pm(QStringLiteral(":/icons/memex-%1.png").arg(s));
    CHECK(!pm.isNull());
    CHECK(pm.width() == s && pm.height() == s);
    CHECK(has_brand_orange(pm.toImage()));
  }
}

// —— ②③ 主窗窗口图标＋关于页真弹 ——
void test_window_and_about() {
  memex::client::MainWindow w;
  w.show();
  CHECK(wait_until([&] { return w.isVisible(); }, 3000));
  CHECK(!w.windowIcon().availableSizes().isEmpty()); // 资源 logo 生效＝多尺寸

  // 帮助菜单找「关于 Memex…」（macOS 上 AboutRole 挪进应用菜单，同样可触发）
  QAction* about = nullptr;
  for (auto* top : w.menuBar()->actions()) {
    if (!top->menu()) continue;
    for (auto* act : top->menu()->actions()) {
      if (act->text() == QStringLiteral("关于 Memex…")) about = act;
    }
  }
  CHECK(about != nullptr);
  if (!about) return;

  // exec() 阻塞期间定时器照常派发（同 test_notify 设置页口径）：
  // 找到弹窗→校验 logo 标签→抓图→点 Close 收场
  QTimer::singleShot(300, [] {
    auto* dlg = find_top(QStringLiteral("about_dialog"));
    if (!dlg) return;
    auto* logo = dlg->findChild<QLabel*>(QStringLiteral("about_logo"));
    CHECK(logo != nullptr);
    if (logo) CHECK(!logo->pixmap().isNull());
    auto* ver = dlg->findChild<QLabel*>(QStringLiteral("about_version"));
    CHECK(ver != nullptr);
    const QString png =
        QDir::current().absoluteFilePath(QStringLiteral("about_dialog.png"));
    CHECK(dlg->grab().save(png));
    qInfo() << "关于页截图：" << png;
    auto* box = dlg->findChild<QDialogButtonBox*>();
    if (box) {
      if (auto* close = box->button(QDialogButtonBox::Close)) close->click();
    }
  });
  about->trigger(); // → show_about() → exec()
  CHECK(find_top(QStringLiteral("about_dialog")) == nullptr); // 已关闭
}

// —— ④ 开机启动 .desktop 带 Icon=memex ——
// 本进程自登记（隔离的 XDG_CONFIG_HOME，不碰用户真配置），再读回断言。
void test_autostart_desktop_icon() {
  memex::client::MainWindow w; // 菜单触发（与用户路径同款：设置→开机启动）
  QAction* autostart = nullptr;
  for (auto* top : w.menuBar()->actions()) {
    if (!top->menu()) continue;
    for (auto* act : top->menu()->actions()) {
      if (act->text().startsWith(QStringLiteral("开机启动"))) autostart = act;
    }
  }
  CHECK(autostart != nullptr);
  if (autostart) {
    if (!autostart->isChecked()) autostart->trigger(); // 登记＝写 .desktop
    CHECK(autostart->isChecked());
  }

  const QString path = QStandardPaths::writableLocation(
                           QStandardPaths::ConfigLocation) +
                       QStringLiteral("/autostart/memex-client.desktop");
  CHECK(QFileInfo::exists(path));
  QFile f(path);
  CHECK(f.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(f.readAll());
  f.close();
  CHECK(content.contains(QStringLiteral("Icon=memex\n")));
}

} // namespace

int main(int argc, char** argv) {
  QTemporaryDir env;
  CHECK(env.isValid());
  qputenv("XDG_CONFIG_HOME", env.filePath(QStringLiteral("cfg")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("Memex"));
  QCoreApplication::setOrganizationName(QStringLiteral("memex"));

  test_resources();
  test_window_and_about();
  test_autostart_desktop_icon();

  if (g_failures == 0) {
    qInfo("icon tests: all passed");
    return 0;
  }
  qCritical("icon tests: %d failure(s)", g_failures);
  return 1;
}
