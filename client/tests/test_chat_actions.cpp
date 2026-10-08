// BUG-001/002/003 验收：聊天窗「截图／发文件／表情」三入口点击接线。
// 离屏点击真按钮，按可观测后果断言：无会话时守卫把原因写进状态栏（截图
// 遮罩拖拽链路另由 test_screenshot 端到端覆盖）；表情走全链——面板弹出→
// 点内置表情→落地输入框→面板自关（顺带暴露面板 lambda 悬垂引用类缺陷）。
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenuBar>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QThread>
#include <QTimer>

#include <functional>

#include <app/emoji_pack_dialog.hpp>
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

  // —— 需求批⑤ 发文件夹：入口在场；无会话点击守卫（与发文件同口径）——
  QPushButton* folder = find_button(window, QStringLiteral("发文件夹"));
  CHECK(folder != nullptr);
  if (folder) {
    folder->click();
    CHECK(wait_until([&] {
      return window.status_text().contains(
          QStringLiteral("先选择设备再发送文件夹"));
    }, 3000));
  }

  // —— 需求批⑥ 振屏：入口在场；无会话点击守卫（同口径）——
  QPushButton* nudge = find_button(window, QStringLiteral("振屏"));
  CHECK(nudge != nullptr);
  if (nudge) {
    nudge->click();
    CHECK(wait_until([&] {
      return window.status_text().contains(
          QStringLiteral("先选择会话再发送振屏"));
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

  // —— 需求批② 表情包：面板「云表情包…」开云素材管理窗（模态自收场）
  //     ＋图片气泡右键「收藏到表情包」落本地表情目录 ——
  if (emoji) {
    emoji->click();
    QDialog* panel3 = nullptr;
    CHECK(wait_until(
        [&] {
          for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* d = qobject_cast<QDialog*>(w);
            if (d && d->isVisible() &&
                d->windowTitle() == QStringLiteral("表情")) {
              panel3 = d;
              return true;
            }
          }
          return false;
        },
        3000));
    if (panel3) {
      QPushButton* cloud = nullptr;
      for (QPushButton* b : panel3->findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("云表情包…")) {
          cloud = b;
          break;
        }
      }
      CHECK(cloud != nullptr);
      if (cloud) {
        bool cloud_opened = false;
        QTimer::singleShot(300, [&cloud_opened] {
          for (QWidget* w : QApplication::topLevelWidgets()) {
            auto* d = qobject_cast<memex::client::EmojiPackDialog*>(w);
            if (d && d->isVisible()) {
              cloud_opened = true;
              d->reject(); // 模态 exec 在事件循环内收场
            }
          }
        });
        cloud->click();
        CHECK(cloud_opened);
      }
      // 面板 WA_DeleteOnClose：cloud 处理器内已 close＝析构，不可再触碰
    }

    // 右键收藏：图片气泡在场 → 上下文菜单点「收藏到表情包」→ 文件落
    // 本地表情目录（MEMEX_TEST_EMOJI_DIR 指向临时目录，不污染真实数据）
    QTemporaryDir tmp2;
    const QString src = tmp2.filePath(QStringLiteral("fave.png"));
    {
      // 最小合法 PNG（1×1 RGBA）：QTextBrowser 能真加载，cursorRect 才有效
      QFile f(src);
      CHECK(f.open(QIODevice::WriteOnly));
      f.write(QByteArray::fromBase64(
          "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk"
          "YPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="));
      f.close();
    }
    const QString emoji_dir = tmp2.filePath(QStringLiteral("emoji"));
    qputenv("MEMEX_TEST_EMOJI_DIR", emoji_dir.toUtf8());
    window.open_direct_peer(QStringLiteral("dev-B9"));
    CHECK(window.chat_panel_visible());
    window.inject_image(QStringLiteral("dev-B9"), src, false);
    QTextBrowser* view = window.chat_widget();
    CHECK(view != nullptr);
    QPoint hit;
    for (QTextBlock blk = view->document()->firstBlock(); blk.isValid();
         blk = blk.next()) {
      for (QTextBlock::iterator it = blk.begin(); !it.atEnd(); ++it) {
        QTextCursor cur(view->document());
        cur.setPosition(it.fragment().position());
        if (cur.charFormat().toImageFormat().isValid()) {
          hit = view->cursorRect(cur).center(); // 视口坐标
          break;
        }
      }
      if (!hit.isNull()) break;
    }
    CHECK(!hit.isNull());
    if (!hit.isNull() && view) {
      QContextMenuEvent ev(QContextMenuEvent::Mouse, hit,
                           view->viewport()->mapToGlobal(hit));
      QApplication::sendEvent(view->viewport(), &ev);
      QAction* fav = nullptr;
      for (QWidget* w : QApplication::topLevelWidgets()) {
        auto* m = qobject_cast<QMenu*>(w);
        if (m == nullptr) continue;
        for (QAction* a : m->actions()) {
          if (a->text() == QStringLiteral("收藏到表情包")) {
            fav = a;
            break;
          }
        }
      }
      CHECK(fav != nullptr);
      if (fav) {
        fav->trigger();
        CHECK(wait_until([&] {
          return QFileInfo::exists(emoji_dir + QStringLiteral("/fave.png"));
        }, 3000));
        CHECK(wait_until([&] {
          return window.status_text().contains(
              QStringLiteral("已收藏到表情包"));
        }, 3000));
      }
    }
  }

  // —— 需求批⑦ 消息回执：气泡旁状态可见（✓ 送达/✓✓ 已读、只升不降）
  //     ＋已读回执开关（全局缺省＋会话级覆盖）与设置菜单接线 ——
  {
    // QSettings 持久化残留清场（重跑幂等——上次运行留下的开关值会使
    // 缺省断言假红），段尾同样还原缺省态
    QSettings s(QCoreApplication::organizationName(),
                QCoreApplication::applicationName());
    s.remove(QStringLiteral("receipts/send_read"));
    s.remove(QStringLiteral("receipts/peer/peer-x"));
    s.remove(QStringLiteral("receipts/peer/peer-y"));
    // 开关裁决链：全局默认开 → 全局关 → 会话级显式覆盖全局
    CHECK(window.read_receipts_enabled(QStringLiteral("peer-x")));
    window.set_read_receipts_enabled(QString(), false);
    CHECK(!window.read_receipts_enabled(QStringLiteral("peer-y")));
    window.set_read_receipts_enabled(QStringLiteral("peer-x"), true);
    CHECK(window.read_receipts_enabled(QStringLiteral("peer-x"))); // 覆盖全局关
    CHECK(!window.read_receipts_enabled(QStringLiteral("peer-y")));
    window.set_read_receipts_enabled(QStringLiteral("peer-x"), false);
    CHECK(!window.read_receipts_enabled(QStringLiteral("peer-x")));
    window.set_read_receipts_enabled(QString(), true); // 还原全局默认开
    CHECK(window.read_receipts_enabled(QStringLiteral("peer-y")));

    // 设置菜单全局开关接线（勾选态即 QSettings 值）
    QAction* gact = nullptr;
    for (QMenu* m : window.menuBar()->findChildren<QMenu*>()) {
      for (QAction* a : m->actions()) {
        if (a->text() == QStringLiteral("发送已读回执")) gact = a;
      }
    }
    CHECK(gact != nullptr);
    if (gact) {
      CHECK(gact->isCheckable());
      const bool before = gact->isChecked();
      gact->setChecked(!before);
      CHECK(window.read_receipts_enabled(QString()) == !before);
      gact->setChecked(before); // 还原
    }

    // 气泡旁回执标注：delivered=✓ 已送达 → read=✓✓ 已读（升）；
    // 乱序后到的 delivered 不倒退已读终态
    window.open_direct_peer(QStringLiteral("dev-C7"));
    CHECK(window.chat_panel_visible());
    window.inject_message(QStringLiteral("dev-C7"),
                          QStringLiteral("回执标注验收"), true,
                          QStringLiteral("msg-rc1"));
    CHECK(!window.chat_html().contains(QStringLiteral("已送达")));
    window.apply_message_receipt(QStringLiteral("msg-rc1"),
                                 QStringLiteral("delivered"));
    CHECK(window.chat_html().contains(QStringLiteral("✓ 已送达")));
    window.apply_message_receipt(QStringLiteral("msg-rc1"),
                                 QStringLiteral("read"));
    CHECK(window.chat_html().contains(QStringLiteral("✓✓ 已读")));
    CHECK(!window.chat_html().contains(QStringLiteral("✓ 已送达")));
    // 乱序回退：后到的 delivered 通知不倒退 read
    window.apply_message_receipt(QStringLiteral("msg-rc1"),
                                 QStringLiteral("delivered"));
    CHECK(window.chat_html().contains(QStringLiteral("✓✓ 已读")));

    // 还原缺省态（不留开关残留——QSettings 落盘，残留会污染重跑）
    s.remove(QStringLiteral("receipts/send_read"));
    s.remove(QStringLiteral("receipts/peer/peer-x"));
    s.remove(QStringLiteral("receipts/peer/peer-y"));
  }

  if (g_failures == 0) {
    qInfo("chat action buttons: all passed");
    return 0;
  }
  qCritical("chat action buttons: %d failure(s)", g_failures);
  return 1;
}
