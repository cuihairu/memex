// BUG-001/002/003 验收：聊天窗「截图／发文件／表情」三入口点击接线。
// 离屏点击真按钮，按可观测后果断言：无会话时守卫把原因写进状态栏（截图
// 遮罩拖拽链路另由 test_screenshot 端到端覆盖）；表情走全链——面板弹出→
// 点内置表情→落地输入框→面板自关（顺带暴露面板 lambda 悬垂引用类缺陷）。
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QThread>

#include <functional>

#include <app/main_window.hpp>
#include <app/screenshot_tool.hpp>
#include <app/theme.hpp>

using memex::client::MainWindow;

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

QPushButton* find_button(QWidget& root, const QString& text) {
  for (QPushButton* b : root.findChildren<QPushButton*>()) {
    if (b->text() == text) return b;
  }
  return nullptr;
}

QLineEdit* find_input(MainWindow& window) {
  for (QLineEdit* e : window.findChildren<QLineEdit*>()) {
    if (e->placeholderText().contains(QStringLiteral("回车发送"))) return e;
  }
  return nullptr;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  // 与 test_theme 同口径：测试驱动生产 ThemeManager 单例
  memex::client::ThemeManager& manager = memex::client::ThemeManager::instance();
  manager.set_system_dark_probe([] { return false; });
  manager.apply(&app);

  MainWindow window;
  window.show();
  CHECK(wait_until([&] { return window.isVisible(); }, 5000));

  // —— BUG-001 截图：本机捕获无设备前置——无会话点击即起遮罩，Esc 退出 ——
  // （发送端落点校验与标注确认链路由 test_screenshot 端到端覆盖）
  QPushButton* shot = find_button(window, QStringLiteral("截图"));
  CHECK(shot != nullptr);
  CHECK(window.screenshot_tool() != nullptr);
  CHECK(!window.screenshot_tool()->isRunning());
  if (shot) {
    shot->click();
    CHECK(wait_until([&] { return window.screenshot_tool()->isRunning(); },
                     3000));
    // Esc 退出取景（keyPressEvent → escapePressed → 遮罩全关）
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    for (QWidget* w : QApplication::topLevelWidgets()) {
      if (qobject_cast<memex::client::RegionSelectOverlay*>(w) != nullptr) {
        QApplication::sendEvent(w, &esc);
      }
    }
    CHECK(wait_until([&] { return !window.screenshot_tool()->isRunning(); },
                     3000));
  }

  // —— BUG-002 发文件：无会话点击 → 状态栏明示原因 ——
  QPushButton* file = find_button(window, QStringLiteral("发文件"));
  CHECK(file != nullptr);
  if (file) {
    file->click();
    CHECK(wait_until([&] {
      return window.status_text().contains(
          QStringLiteral("先选择设备再发送文件"));
    }, 3000));
  }

  // —— BUG-003 表情：点击弹面板（锚定可见）→ 点 😀 落输入框 → 面板自关 ——
  QLineEdit* input = find_input(window);
  CHECK(input != nullptr);
  QPushButton* emoji = find_button(window, QStringLiteral("表情"));
  CHECK(emoji != nullptr);
  if (emoji && input) {
    input->clear();
    emoji->click();
    QDialog* panel = nullptr;
    CHECK(wait_until(
        [&] {
          for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* d = qobject_cast<QDialog*>(w);
            if (d && d->isVisible() &&
                d->windowTitle() == QStringLiteral("表情")) {
              panel = d;
              return true;
            }
          }
          return false;
        },
        3000));
    if (panel) {
      QPushButton* smile = nullptr;
      for (QPushButton* b : panel->findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("😀")) {
          smile = b;
          break;
        }
      }
      CHECK(smile != nullptr);
      if (smile) {
        smile->click();
        CHECK(wait_until([&] { return !panel->isVisible(); }, 3000));
        CHECK(input->text().contains(QStringLiteral("😀")));
      }
    }
  }

  if (g_failures == 0) {
    qInfo("chat action buttons: all passed");
    return 0;
  }
  qCritical("chat action buttons: %d failure(s)", g_failures);
  return 1;
}
