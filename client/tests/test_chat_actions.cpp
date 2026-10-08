// BUG-001/002/003 验收：聊天窗「截图／发文件／表情」三入口点击接线。
// 离屏点击真按钮，按可观测后果断言：无会话时守卫把原因写进状态栏（截图
// 遮罩拖拽链路另由 test_screenshot 端到端覆盖）；表情走全链——面板弹出→
// 点内置表情→落地输入框→面板自关（顺带暴露面板 lambda 悬垂引用类缺陷）。
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
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

  // —— 需求批① 文字颜色：选区着色＝受控标记入输入框（线上纯文本不变）；
  // 气泡渲染＝esc 后替换颜色 span；坏色/不成对保持字面（无害降级） ——
  QPushButton* color = find_button(window, QStringLiteral("颜色"));
  CHECK(color != nullptr);
  if (input && color) {
    input->clear();
    input->setText(QStringLiteral("重点内容"));
    input->setSelection(0, 2);
    window.apply_input_color(QColor(0xff, 0x00, 0x00));
    CHECK(input->text() == QStringLiteral("〔#ff0000〕重点〔/〕内容"));
    // 无选区：状态行明示原因，输入框不动
    const QString before = input->text();
    input->deselect();
    window.apply_input_color(QColor(0x00, 0xff, 0x00));
    CHECK(input->text() == before);
    CHECK(wait_until([&] {
      return window.status_text().contains(
          QStringLiteral("先选中要着色的文字"));
    }, 3000));
    // 展开会话面板（截图前提：列表态下面板隐藏，主窗 grab 拍不到气泡）
    window.open_direct_peer(QStringLiteral("dev-A2"));
    CHECK(window.chat_panel_visible());
    // 渲染腿：inject_message 测试缝（转 append_message）→ chat_html 断言
    window.inject_message(QStringLiteral("leg1"),
                          QStringLiteral("〔#ff0000〕重点〔/〕内容"), false);
    // Qt toHtml 重排 span：style 值带前导空格（探针实证），按实际形态断言
    CHECK(window.chat_html().contains(
        QStringLiteral("<span style=\" color:#ff0000;\">重点</span>")));
    window.inject_message(
        QStringLiteral("leg2"),
        QStringLiteral("〔#gggggg〕坏色〔/〕＋〔#ff0000〕未闭合"), false);
    CHECK(!window.chat_html().contains(
        QStringLiteral("color:#gggggg")));
    CHECK(window.chat_html().contains(QStringLiteral("〔#gggggg〕坏色〔/〕")));
    // 验收截图（需求批①）：输入框带标记文本＋气泡彩色渲染实况
    {
      const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
      CHECK(QDir().mkpath(dir));
      CHECK(window.grab().save(dir + QStringLiteral("/rich-color.png")));
    }
  }

  // —— 需求批③ 消息内图标：表情面板图标组——点 ⚠ 落输入框、面板自关 ——
  if (emoji && input) {
    input->clear();
    emoji->click();
    QDialog* panel2 = nullptr;
    CHECK(wait_until(
        [&] {
          for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* d = qobject_cast<QDialog*>(w);
            if (d && d->isVisible() &&
                d->windowTitle() == QStringLiteral("表情")) {
              panel2 = d;
              return true;
            }
          }
          return false;
        },
        3000));
    if (panel2) {
      QPushButton* warn = nullptr;
      for (QPushButton* b : panel2->findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("⚠")) {
          warn = b;
          break;
        }
      }
      CHECK(warn != nullptr);
      if (warn) {
        warn->click();
        CHECK(wait_until([&] { return !panel2->isVisible(); }, 3000));
        CHECK(input->text().contains(QStringLiteral("⚠")));
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
