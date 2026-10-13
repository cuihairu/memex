// 勾选框可点性回归（缺陷单 2026-10-13「设置里的打勾都无法选择」）：
// 根因＝theme.cpp 的 QCheckBox::indicator/QRadioButton::indicator 规则设了
// border/background（drawable），QSS 接管子控件绘制后勾与点不再画——内部状态
// 仍切换、信号仍发，视觉全空＝看着"点不动"。修复＝删该规则（指示框交回原生
// 样式按调色板绘制；见 theme.cpp 注释）。
// 本测试：真主题 QSS 经 ThemeManager::apply 落到 app（生产路径），逐设置页
// 逐项 QTest::mouseClick（与用户点击同路径）——双重断言：
//   ① 点击后 isChecked 翻转；
//   ② 指示框区域像素确实变化（勾真画出来了，不只内部状态翻转）；
// 另加回归闸：全局 QSS 不得再出现 QCheckBox::indicator 选择器（ drawable
// 规则复活即红灯）。截图证据：MEMEX_CHECKBOX_SHOT_DIR 指定时落指示框
// 点击前/后裁图，常规 ctest 跑无副作用。
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <vector>

#include <app/away_settings.hpp>
#include <app/lock_screen.hpp>
#include <app/main_window.hpp>
#include <app/net_guard.hpp>
#include <app/net_remote_settings.hpp>
#include <app/notify_center.hpp>
#include <app/notify_prefs.hpp>
#include <app/theme.hpp>
#include <app/theme_settings_page.hpp>

using memex::client::AwayLockSettingsDialog;
using memex::client::NetRemoteSettingsDialog;
using memex::client::NotificationCenter;
using memex::client::ThemeManager;
using memex::client::ThemeSettingsPage;

namespace memex::client {
// 链接桩：两对话框 win_ 恒空指针（回调不可达），符号存在即可，不实例化
// MainWindow（避免拖入整个主窗编译单元）
void MainWindow::apply_net_settings() {}
void MainWindow::apply_away_lock_settings() {}
}  // namespace memex::client

namespace {

int g_failures = 0;
int g_clicks = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

struct ClickHit {
  QString page;
  QString name;
  bool before = false;
  bool after = false;
  int indicator_diff = 0;
  QString note;
};

std::vector<ClickHit> g_hits;
std::vector<QString> g_skips;

int diff_pixels(const QImage& a, const QImage& b) {
  QImage x = a.convertToFormat(QImage::Format_ARGB32);
  QImage y = b.convertToFormat(QImage::Format_ARGB32);
  int diff = 0;
  for (int r = 0; r < qMin(x.height(), y.height()); ++r)
    for (int c = 0; c < qMin(x.width(), y.width()); ++c)
      if (x.pixel(c, r) != y.pixel(c, r)) ++diff;
  return diff;
}

QRect indicator_rect(QWidget* w) {
  QStyleOptionButton opt;
  opt.initFrom(w);
  const bool is_check = qobject_cast<QCheckBox*>(w) != nullptr;
  return w->style()->subElementRect(
      is_check ? QStyle::SE_CheckBoxIndicator : QStyle::SE_RadioButtonIndicator,
      &opt, w);
}

// 点击热区＝指示框＋文字（SE_*ClickRect）；整行拉宽的复选框几何中心
// 落在文字右侧死区，真用户点的就是热区本身——合成点取热区中心
QRect click_rect(QWidget* w) {
  QStyleOptionButton opt;
  opt.initFrom(w);
  const bool is_check = qobject_cast<QCheckBox*>(w) != nullptr;
  const QRect area = w->style()->subElementRect(
      is_check ? QStyle::SE_CheckBoxClickRect
               : QStyle::SE_RadioButtonClickRect,
      &opt, w);
  if (area.isValid()) return area;
  return indicator_rect(w);
}

void save_shot(const QString& page, const QString& name, const QImage& img) {
  const QString dir = qEnvironmentVariable("MEMEX_CHECKBOX_SHOT_DIR");
  if (dir.isEmpty()) return;
  QDir().mkpath(dir);
  img.save(QDir(dir).filePath(
      QStringLiteral("%1_%2.png")
          .arg(page, name)
          .replace(QChar(0x00b7), QLatin1Char('-'))
          .replace(QLatin1Char('/'), QLatin1Char('_'))));
}

// 与 QTest::mouseClick 同机制：合成鼠标按下/抬起直接送控件事件处理
// （QTest 内部即 sendEvent），走 QCheckBox 完整 press/release→toggle 路径
void mouse_click_widget(QWidget* w) {
  const QPoint local = click_rect(w).center();
  const QPoint global = w->mapToGlobal(local);
  QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(w, &press);
  QApplication::sendEvent(w, &release);
  QApplication::processEvents();
}

// 点击一项勾选控件：状态翻转＋指示框像素变化双断言（radio 语义：已选项
// 点击不再取消，只要求状态不变且若有切换则像素变化）
void click_and_verify(const QString& page, QAbstractButton* btn) {
  ClickHit hit;
  hit.page = page;
  hit.name = btn->objectName().isEmpty() ? btn->text() : btn->objectName();
  hit.before = btn->isChecked();

  const QImage before_img = btn->grab(indicator_rect(btn)).toImage();
  const bool want_full =
      !qEnvironmentVariable("MEMEX_CHECKBOX_FULL_SHOT").isEmpty();
  const QImage full_before = want_full ? btn->grab(btn->rect()).toImage()
                                       : QImage();
  const QRect hit_area = click_rect(btn);
  const QRect ind = indicator_rect(btn);
  qInfo("  [%s] %s: widget %dx%d, indicator %dx%d@%d,%d, click area %dx%d@%d,%d",
        qPrintable(page), qPrintable(hit.name), btn->width(), btn->height(),
        ind.width(), ind.height(), ind.x(), ind.y(), hit_area.width(),
        hit_area.height(), hit_area.x(), hit_area.y());
  mouse_click_widget(btn);
  hit.after = btn->isChecked();
  const QImage after_img = btn->grab(indicator_rect(btn)).toImage();
  hit.indicator_diff = diff_pixels(before_img, after_img);
  ++g_clicks;

  save_shot(page + "_before", hit.name, before_img);
  save_shot(page + "_after", hit.name, after_img);
  // 带标签全控件件（环境指定时）：看的是页面里真实的样子
  if (want_full) {
    save_shot(page + "_full_before", hit.name, full_before);
    save_shot(page + "_full_after", hit.name,
              btn->grab(btn->rect()).toImage());
  }

  const bool is_check = qobject_cast<QCheckBox*>(btn) != nullptr;
  if (is_check) {
    CHECK(hit.after != hit.before);            // 状态真翻转
    CHECK(hit.indicator_diff > 0);             // 勾真画出来了（视觉反馈）
  } else {
    // radio：未选→点击必选中且像素变；已选→再点击不取消（auto-exclusive 语义）
    CHECK(hit.after);                          // 点击后必处于选中
    if (!hit.before) CHECK(hit.indicator_diff > 0);
  }
  qInfo("  [%s] %s: checked %d->%d, indicator diff px %d%s",
        qPrintable(page), qPrintable(hit.name), hit.before ? 1 : 0,
        hit.after ? 1 : 0, hit.indicator_diff,
        hit.note.isEmpty() ? "" : qPrintable(" (" + hit.note + ")"));
  g_hits.push_back(hit);
}

void verify_page(const QString& page, QWidget* root) {
  int boxes = 0, radios = 0, skipped = 0;
  for (QCheckBox* cb : root->findChildren<QCheckBox*>()) {
    if (!cb->isEnabled()) {  // 设计内禁用项（如提示音未开"按事件类型"）
      g_skips.push_back(page + "/" + (cb->objectName().isEmpty()
                                    ? cb->text() : cb->objectName()));
      ++skipped;
      continue;
    }
    click_and_verify(page, cb);
    ++boxes;
  }
  for (QRadioButton* rb : root->findChildren<QRadioButton*>()) {
    if (!rb->isEnabled()) {
      g_skips.push_back(page + "/" + (rb->objectName().isEmpty()
                                    ? rb->text() : rb->objectName()));
      ++skipped;
      continue;
    }
    click_and_verify(page, rb);
    ++radios;
  }
  qInfo("page %s: %d checkbox(es), %d radio(s) clicked, %d disabled skipped",
        qPrintable(page), boxes, radios, skipped);
  CHECK(boxes + radios > 0);  // 每页至少一项（防测试空跑假绿）
}

// —— 通知偏好页：show_settings() 内部 dlg.exec() 模态，定时器在 exec 事件
//    循环里逐项真点（提示音三档默认禁用，先切"按事件类型"再逐项验证）——
void test_notify_settings() {
  QTimer::singleShot(80, [] {
    QWidget* top = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
      if (w->objectName() == QLatin1String("notify_settings_dialog")) {
        top = w;
        break;
      }
    if (top == nullptr) {
      qCritical("notify settings dialog not found during exec");
      ++g_failures;
      return;
    }
    if (auto* cmb =
            top->findChild<QComboBox*>(QStringLiteral("cmb_sound_mode")))
      cmb->setCurrentIndex(2);  // 「按事件类型」＝三档提示音开关启用
    QApplication::processEvents();
    verify_page(QStringLiteral("notify_settings"), top);
    if (auto* box =
            top->findChild<QDialogButtonBox*>(QStringLiteral("btn_settings")))
      if (auto* ok = box->button(QDialogButtonBox::Ok)) ok->click();
  });
  NotificationCenter::instance().show_settings();
}

void test_settings_pages(QApplication& app) {
  for (const char* mode : {ThemeManager::kLight, ThemeManager::kDark}) {
    ThemeManager& manager = ThemeManager::instance();
    manager.set_system_dark_probe([] { return false; });
    manager.set_mode(QString::fromUtf8(mode));
    manager.apply(&app);  // 生产路径：全局 QSS＋调色板落 app
    QApplication::processEvents();

    {
      AwayLockSettingsDialog dlg(nullptr, nullptr);
      dlg.show();
      QApplication::processEvents();
      verify_page(QStringLiteral("away_lock") + QString::fromUtf8(mode), &dlg);
      dlg.close();
    }
    {
      NetRemoteSettingsDialog dlg(nullptr, nullptr);
      dlg.show();
      QApplication::processEvents();
      verify_page(QStringLiteral("net_remote") + QString::fromUtf8(mode), &dlg);
      dlg.close();
    }
    {
      ThemeSettingsPage page(&manager);
      page.show();
      QApplication::processEvents();
      verify_page(QStringLiteral("theme_settings") + QString::fromUtf8(mode),
                  &page);
      page.close();
    }
    manager.set_mode(QString::fromUtf8(ThemeManager::kFollowSystem));
  }
  app.setStyleSheet(QString());  // 复位，避免影响同进程后续用例
}

}  // namespace

int main(int argc, char** argv) {
  QTemporaryDir tmp;  // 隔离 QSettings（勾选开关初值确定性）
  if (!tmp.isValid()) return 1;
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME",
          tmp.filePath(QStringLiteral("xdg-config")).toUtf8());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     tmp.filePath(QStringLiteral("settings")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("checkbox-glyph-test"));

  // 回归闸：修复后的全局 QSS 不得再含 ::indicator drawable 选择器
  {
    ThemeManager manager;
    manager.set_system_dark_probe([] { return false; });
    manager.apply(&app);
    CHECK(!app.styleSheet().contains(QLatin1String("QCheckBox::indicator")));
    CHECK(!app.styleSheet().contains(QLatin1String("QRadioButton::indicator")));
  }

  test_settings_pages(app);
  test_notify_settings();

  qInfo("checkbox glyph: %d clicks, %d disabled-by-design skipped",
        g_clicks, static_cast<int>(g_skips.size()));
  for (const QString& s : g_skips) qInfo("  skipped (disabled): %s",
                                          qPrintable(s));

  if (g_failures == 0) {
    qInfo("test_checkbox_glyph: ALL PASS");
    return 0;
  }
  qCritical("test_checkbox_glyph: %d FAILURES", g_failures);
  return 1;
}
