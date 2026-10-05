// 新消息闪烁提醒验收（用户令 2026-10-05）：真直连对端发消息 → 主窗
// alert_count 断言——非激活才闪（离屏无真焦点系统，辅助窗抢激活或不激活
// 是常态，两分支断言前置）、合并窗内多条只记一次、开关关闭完全不闪、
// 开关落盘往返。QWindow::alert 的视觉效果属平台面，离屏不可截图——
// 以计数为断言面（真机闪烁观感走桌面走查，边界在回账注明）。
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QWidget>

#include <functional>

#include <app/main_window.hpp>
#include <app/notify_prefs.hpp>
#include <engine/direct/direct_engine.hpp>

using memex::client::DirectEngine;
using memex::client::MainWindow;
using memex::client::NotifyPrefs;

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
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return cond();
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  // 开关态收口为本测试独有（空 org 的 QSettings，与真机设置互不串扰）
  NotifyPrefs prefs = NotifyPrefs::load();
  prefs.flash_alert = true;
  prefs.save();
  CHECK(NotifyPrefs::load().flash_alert);

  MainWindow window;
  window.show();

  // 辅助窗抢激活：让主窗处于「非激活」前提（离屏若无真激活则主窗本来
  // 就非激活，前提同样成立——两种环境都先断言这一前提再发消息）
  QWidget helper;
  helper.show();
  helper.activateWindow();
  QApplication::processEvents(QEventLoop::AllEvents, 50);

  // 对端引擎直发消息（默认库同 QSettings 持久标识 → 读到主窗引擎的
  // device_id，send_text 定向投递）
  DirectEngine window_id_probe; // 只读持久标识，不起面
  DirectEngine peer(QStringLiteral("flash-peer").toStdString(), QString());
  CHECK(peer.start());
  CHECK(wait_until([&] { return window.has_direct_peer(QStringLiteral("flash-peer")); },
                   10000));
  // 发现互见不对称：发送侧（peer）也须见到主窗引擎 id 才定向可达
  CHECK(wait_until([&] { return peer.has_peer(window_id_probe.device_id()); },
                   10000));

  const auto send = [&](const char* text) {
    CHECK(peer.send_text(window_id_probe.device_id(), text) != 0);
  };
  const auto inactive = [&] {
    helper.activateWindow();
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    return !window.isActiveWindow();
  };

  // —— ① 开＝非激活收消息即闪：计数 0→1 ——
  CHECK(inactive());
  send("闪一");
  CHECK(wait_until([&] { return window.alert_count() == 1; }, 6000));

  // —— ② 合并窗（2s）内第二条不叠加：仍 1 ——
  send("闪二（合并窗内）");
  QThread::msleep(300);
  QApplication::processEvents(QEventLoop::AllEvents, 100);
  CHECK(window.alert_count() == 1);
  CHECK(wait_until([&] { return window.alert_count() == 1; }, 50)); // 前提不变

  // —— ③ 合并窗过后第三条再闪：1→2 ——
  QThread::msleep(2300); // 越过 kFlashAlertMs=2000 合并窗
  QApplication::processEvents(QEventLoop::AllEvents, 100);
  CHECK(inactive());
  send("闪三（新合并窗）");
  CHECK(wait_until([&] { return window.alert_count() == 2; }, 6000));

  // —— ④ 关＝完全不闪：开关落盘 false 后计数冻结 ——
  NotifyPrefs off = NotifyPrefs::load();
  off.flash_alert = false;
  off.save();
  CHECK(!NotifyPrefs::load().flash_alert); // 落盘往返
  QThread::msleep(2300);                   // 也越过合并窗，排除合并窗假阴性
  CHECK(inactive());
  send("不闪（开关已关）");
  QThread::msleep(500);
  QApplication::processEvents(QEventLoop::AllEvents, 100);
  CHECK(window.alert_count() == 2);

  // 还原默认（开），后续用例/真机不受本测试残留影响
  NotifyPrefs restore = NotifyPrefs::load();
  restore.flash_alert = true;
  restore.save();

  if (g_failures == 0) {
    qInfo("flash alert: all passed");
    return 0;
  }
  qCritical("flash alert: %d failure(s)", g_failures);
  return 1;
}
