// T4.9 主题切换验收（R19）：
// ① 令牌完备性与品牌橙恒定、亮暗两套关键面明显不同；
// ② 跟随系统解析：系统亮暗 → light／dark，手动选择覆盖系统；
// ③ 持久化往返：手动选择落盘，新实例读回（重启后保持）；
// ④ 令牌化 QSS 生成：占位符零残留、关键选择器齐、切换后样式变化（即时生效）；
// ⑤ 多主题扩展位：只注册一套令牌即可被选中／解析／落盘；
// ⑥ 应用级落地：调色板色与令牌一致、系统亮暗变化仅在跟随模式下重应用；
// ⑦ 设置页交互：点选暗色／跟随系统即时生效并回灌选中态。
#include <QApplication>
#include <QPalette>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <functional>

#include <app/theme.hpp>
#include <app/theme_settings_page.hpp>

using memex::client::ThemeManager;
using memex::client::ThemeSettingsPage;
using memex::client::ThemeTokens;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

double linearized(double c) {
  return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& c) {
  return 0.2126 * linearized(c.redF()) + 0.7152 * linearized(c.greenF()) +
         0.0722 * linearized(c.blueF());
}

// WCAG 对比度：亮者在上、暗者在下，与书写顺序无关
double contrast(const QColor& a, const QColor& b) {
  const double la = luminance(a);
  const double lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// QSS 里有未替换的令牌占位符即视为漏色（颜色又散落回字面量的信号）
bool has_placeholder(const QString& qss) {
  return qss.contains(QStringLiteral("%text%")) ||
         qss.contains(QStringLiteral("%surface%")) ||
         qss.contains(QStringLiteral("%brand%"));
}

// —— ① 令牌完备性／品牌橙恒定／亮暗差异 ——
void test_tokens() {
  const ThemeTokens light = ThemeManager::tokens_for(
      QString::fromUtf8(ThemeManager::kLight));
  const ThemeTokens dark =
      ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kDark));

  CHECK(light.is_complete());
  CHECK(dark.is_complete());
  CHECK(light.as_map().size() == dark.as_map().size());
  CHECK(light.as_map().size() >= 20);

  // 品牌橙 #e16531 是全产品唯一主色，亮暗恒同
  CHECK(light.brand.name() == QStringLiteral("#e16531"));
  CHECK(dark.brand.name() == QStringLiteral("#e16531"));
  CHECK(light.brand.name() == dark.brand.name());

  // 亮暗底色与文本必须显著分离（否则「切换」看不出变化）
  CHECK(luminance(light.surface) > 0.5);
  CHECK(luminance(dark.surface) < 0.2);
  CHECK(luminance(light.text) < 0.3);
  CHECK(luminance(dark.text) > 0.7);
  // 文本与底色对比度可读（WCAG AA 大字口径 3:1 以上），气泡同理
  CHECK(contrast(light.text, light.surface) > 4.5);
  CHECK(contrast(dark.text, dark.surface) > 4.5);
  CHECK(contrast(light.text_muted, light.surface_alt) > 3.0);
  CHECK(contrast(dark.text_muted, dark.surface_alt) > 3.0);
  CHECK(contrast(light.on_brand, light.brand) > 3.0);
  CHECK(contrast(dark.on_brand, dark.brand) > 3.0);
  CHECK(contrast(light.bubble_in_text, light.bubble_in) > 4.5);
  CHECK(contrast(dark.bubble_in_text, dark.bubble_in) > 4.5);
  CHECK(contrast(light.bubble_out_text, light.bubble_out) > 3.0);
  CHECK(contrast(dark.bubble_out_text, dark.bubble_out) > 3.0);
}

// —— ② 跟随系统解析与手动覆盖 ——
void test_system_follow() {
  ThemeManager manager;
  bool system_dark = false;
  manager.set_system_dark_probe([&] { return system_dark; });

  CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kFollowSystem));
  CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kLight));

  system_dark = true;  // 系统切暗
  CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kDark));

  // 手动选择覆盖系统：系统仍暗，但用户选了亮色
  manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
  CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kLight));
  CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kLight));
  CHECK(manager.tokens().surface.name() ==
        ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kLight))
            .surface.name());

  // 未知主题名不静默接受，回落跟随系统（不把界面停在无法解析的取值）
  manager.set_mode(QStringLiteral("no-such-theme"));
  CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kFollowSystem));
}

// —— ③ 持久化往返（重启后保持）——
void test_persistence() {
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());

  {
    ThemeManager manager;
    manager.set_system_dark_probe([] { return false; });
    manager.set_mode(QString::fromUtf8(ThemeManager::kDark));
    CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kDark));
  }
  {
    // 新实例＝重启后：读回落盘值（此时系统是亮的，主题仍须是暗色）
    ThemeManager manager;
    manager.set_system_dark_probe([] { return false; });
    CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kDark));
    CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kDark));
    manager.set_mode(QString::fromUtf8(ThemeManager::kFollowSystem));
  }
  {
    ThemeManager manager;
    manager.set_system_dark_probe([] { return false; });
    CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kFollowSystem));
  }
}

// —— ④ 令牌化 QSS ——
void test_stylesheet() {
  const ThemeTokens light = ThemeManager::tokens_for(
      QString::fromUtf8(ThemeManager::kLight));
  const ThemeTokens dark =
      ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kDark));
  const QString light_qss = ThemeManager::stylesheet_for(light);
  const QString dark_qss = ThemeManager::stylesheet_for(dark);

  CHECK(!light_qss.isEmpty());
  CHECK(!has_placeholder(light_qss));
  CHECK(light_qss != dark_qss);
  // 关键选择器覆盖：基础控件面不能只给一个 QWidget 规则
  for (const char* selector :
       {"QWidget", "QTextBrowser", "QListWidget", "QPushButton", "QLineEdit",
        "QComboBox", "QHeaderView::section", "QMenu", "QToolTip",
        "QScrollBar::handle", "QGroupBox", "QTabBar::tab", "QStatusBar"}) {
    CHECK(light_qss.contains(QLatin1String(selector)));
  }
  // 令牌色确实进了样式（品牌橙与底色各抽查一处）
  CHECK(light_qss.contains(light.brand.name()));
  CHECK(light_qss.contains(light.surface.name()));
  CHECK(dark_qss.contains(dark.brand.name()));
}

// —— ⑤ 多主题扩展位：新增主题仅需新增一套令牌 ——
void test_theme_extension() {
  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });

  ThemeTokens midnight = ThemeManager::tokens_for(
      QString::fromUtf8(ThemeManager::kDark));
  midnight.surface = QColor(QStringLiteral("#0b1020"));
  midnight.text = QColor(QStringLiteral("#dbe4ff"));
  midnight.brand = QColor(QStringLiteral("#00ff00"));  // 试图漂移品牌色
  manager.register_theme(QStringLiteral("midnight"), midnight);

  CHECK(manager.has_theme(QStringLiteral("midnight")));
  CHECK(manager.modes().contains(QStringLiteral("midnight")));
  // 注册即成可选项（无需改任何切换逻辑）
  manager.set_mode(QStringLiteral("midnight"));
  CHECK(manager.mode() == QStringLiteral("midnight"));
  CHECK(manager.effective_theme() == QStringLiteral("midnight"));
  CHECK(manager.tokens().surface.name() == QStringLiteral("#0b1020"));
  // 品牌橙不因扩展主题漂移
  CHECK(manager.tokens().brand.name() == QStringLiteral("#e16531"));
  CHECK(ThemeManager::stylesheet_for(manager.tokens())
            .contains(QStringLiteral("#0b1020")));
}

// —— ⑥ 应用级落地：调色板＋系统变化仅跟随模式重应用 ——
void test_apply_to_app(QApplication& app) {
  const QString app_qss_before = app.styleSheet();

  ThemeManager manager;
  bool system_dark = false;
  manager.set_system_dark_probe([&] { return system_dark; });
  manager.apply(&app);
  CHECK(manager.applied());

  // 跟随模式：调色板底色＝令牌底色，全局 QSS 非空
  CHECK(app.palette().color(QPalette::Window).name() ==
        manager.tokens().surface.name());
  CHECK(app.palette().color(QPalette::WindowText).name() ==
        manager.tokens().text.name());
  CHECK(app.palette().color(QPalette::Highlight).name() ==
        QStringLiteral("#e16531"));
  CHECK(!app.styleSheet().isEmpty());
  CHECK(app.styleSheet() != app_qss_before);

  // 切换即时生效：全局 QSS 与调色板同步变化
  const QString light_qss = app.styleSheet();
  manager.set_mode(QString::fromUtf8(ThemeManager::kDark));
  CHECK(app.styleSheet() != light_qss);
  CHECK(app.palette().color(QPalette::Window).name() ==
        ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kDark))
            .surface.name());

  // 手动模式下系统亮暗变化不覆盖用户选择（跟随语义）
  manager.set_system_dark_probe([] { return true; });
  CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kDark));
  manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
  const QString manual_qss = app.styleSheet();
  manager.set_system_dark_probe([] { return false; });
  manager.set_system_dark_probe([] { return true; });
  CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kLight));
  CHECK(app.styleSheet() == manual_qss);

  // 复位到跟随系统，避免影响后续用例的界面状态
  manager.set_mode(QString::fromUtf8(ThemeManager::kFollowSystem));
  app.setStyleSheet(app_qss_before);
}

// —— ⑦ 设置页交互：点选即时生效并回灌选中态 ——
void test_settings_page(QApplication& app) {
  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });
  manager.apply(&app);

  ThemeSettingsPage page(&manager);
  CHECK(page.effective_theme() == QString::fromUtf8(ThemeManager::kLight));

  auto* dark_radio = page.findChild<QRadioButton*>(
      QStringLiteral("themeMode_dark"));
  CHECK(dark_radio != nullptr);
  if (dark_radio) {
    dark_radio->click();  // 真点选：与用户操作同路径
    CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kDark));
    CHECK(page.effective_theme() == QString::fromUtf8(ThemeManager::kDark));
    CHECK(dark_radio->isChecked());
  }

  auto* system_radio = page.findChild<QRadioButton*>(
      QStringLiteral("themeMode_system"));
  CHECK(system_radio != nullptr);
  if (system_radio) {
    system_radio->click();
    CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kFollowSystem));
    CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kLight));
    CHECK(system_radio->isChecked());
  }

  // 注册扩展主题后设置页立即出现对应选项（只新增令牌这一件事就够）
  ThemeTokens extra = ThemeManager::tokens_for(
      QString::fromUtf8(ThemeManager::kDark));
  extra.surface = QColor(QStringLiteral("#101820"));
  manager.register_theme(QStringLiteral("deepsea"), extra);
  ThemeSettingsPage page2(&manager);
  CHECK(page2.findChild<QRadioButton*>(QStringLiteral("themeMode_deepsea")) !=
        nullptr);
  CHECK(page2.select_mode(QStringLiteral("deepsea")));
  CHECK(manager.effective_theme() == QStringLiteral("deepsea"));
  CHECK(!page2.select_mode(QStringLiteral("nope")));
}

}  // namespace

int main(int argc, char** argv) {
  QTemporaryDir tmp;
  if (!tmp.isValid()) return 1;
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME",
          tmp.filePath(QStringLiteral("xdg-config")).toUtf8());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     tmp.filePath(QStringLiteral("settings")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("theme-test"));

  test_tokens();
  test_system_follow();
  test_persistence();
  test_stylesheet();
  test_theme_extension();
  test_apply_to_app(app);
  test_settings_page(app);

  if (g_failures == 0) {
    qInfo("test_theme: ALL PASS");
    return 0;
  }
  qCritical("test_theme: %d FAILURES", g_failures);
  return 1;
}