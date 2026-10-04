// BUG-004 托盘交互验收：单击/双击托盘图标 → 激活主界面。
// ① 双击：隐藏时弹出、最小化时还原（保留最大化态）＋置顶聚焦；
// ② 单击：同激活（Qt 惯例，对齐微信/企业微信同类软件）；
// ③ 右键（Context）留给上下文菜单，中键/未知不响应；
// ④ 菜单「显示主窗口」与激活同一路径（activate_from_tray）；
// ⑦ 气泡点击（messageClicked，关窗提示气泡被点时）同激活。
// 手法：offscreen 无系统托盘 → 用裸 QSystemTrayIcon 接生产接线
// wire_tray_activation()，发真实 activated 信号驱动（全信号路径，无 DE 依赖）。
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QMetaObject>
#include <QSystemTrayIcon>
#include <QTemporaryDir>

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

// 以真实信号驱动接线（信号是 moc 可调用方法，invokeMethod 即发射）
void click_tray(QSystemTrayIcon* tray, QSystemTrayIcon::ActivationReason reason) {
  QMetaObject::invokeMethod(tray, "activated",
                            Q_ARG(QSystemTrayIcon::ActivationReason, reason));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

void settle() {
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

} // namespace

int main(int argc, char** argv) {
  QTemporaryDir env;
  CHECK(env.isValid());
  qputenv("XDG_CONFIG_HOME", env.filePath(QStringLiteral("cfg")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("Memex"));
  QCoreApplication::setOrganizationName(QStringLiteral("memex"));

  using memex::client::MainWindow;

  MainWindow window;
  window.show();
  settle();
  CHECK(window.isVisible());

  // 裸 QSystemTrayIcon（不依赖系统托盘存在）＋ 生产接线
  QSystemTrayIcon tray;
  window.wire_tray_activation(&tray);

  // ① 双击：隐藏 → 弹出
  {
    window.hide();
    settle();
    CHECK(!window.isVisible());
    click_tray(&tray, QSystemTrayIcon::DoubleClick);
    CHECK(window.isVisible());
  }
  // ② 单击同样激活（Qt 惯例）；已可见时幂等不隐藏
  {
    window.hide();
    settle();
    click_tray(&tray, QSystemTrayIcon::Trigger);
    CHECK(window.isVisible());
    click_tray(&tray, QSystemTrayIcon::Trigger);
    CHECK(window.isVisible());
  }
  // ③ 右键/中键/未知不弹（Context 留给菜单）
  {
    window.hide();
    settle();
    click_tray(&tray, QSystemTrayIcon::Context);
    CHECK(!window.isVisible());
    click_tray(&tray, QSystemTrayIcon::MiddleClick);
    CHECK(!window.isVisible());
    click_tray(&tray, QSystemTrayIcon::Unknown);
    CHECK(!window.isVisible());
  }
  // ④ 最小化 → 双击还原
  {
    window.showMinimized();
    settle();
    CHECK(window.isMinimized());
    click_tray(&tray, QSystemTrayIcon::DoubleClick);
    CHECK(!window.isMinimized());
    CHECK(window.isVisible());
  }
  // ⑤ 最大化后最小化 → 双击还原仍是最大化（还原不清最大化态）
  {
    window.showMaximized();
    settle();
    window.showMinimized();
    settle();
    CHECK(window.isMinimized());
    click_tray(&tray, QSystemTrayIcon::DoubleClick);
    CHECK(!window.isMinimized());
    CHECK(window.isMaximized());
  }
  // ⑥ 直调 activate_from_tray（菜单「显示主窗口」同一路径）：隐藏 → 弹出
  {
    window.hide();
    settle();
    window.activate_from_tray();
    settle();
    CHECK(window.isVisible());
  }
  // ⑦ 气泡点击（messageClicked）同样激活：关窗弹「已最小化到托盘」气泡挡住
  // 图标 5 秒，第一击常落在气泡上——气泡路径也必须弹（BUG-004 实测回归点）
  {
    window.hide();
    settle();
    QMetaObject::invokeMethod(&tray, "messageClicked");
    settle();
    CHECK(window.isVisible());
  }

  if (g_failures == 0) {
    qInfo("tray tests: all passed");
    return 0;
  }
  qCritical("tray tests: %d failure(s)", g_failures);
  return 1;
}
