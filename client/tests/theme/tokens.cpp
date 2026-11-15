// test_theme 拆分模块（LAY-011）：本域场景体自原文件逐字搬移；公共面见 common.hpp。
#include "common.hpp"

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
  // muted 用于正文级说明/次要信息，按 WCAG AA 正文 4.5:1 守住；
  // surface_alt 与 brand_tint（选中行）是两个最暗承载面，一并断言
  CHECK(contrast(light.text_muted, light.surface_alt) > 4.5);
  CHECK(contrast(dark.text_muted, dark.surface_alt) > 4.5);
  CHECK(contrast(light.text_muted, light.brand_tint) > 4.5);
  CHECK(contrast(dark.text_muted, dark.brand_tint) > 4.5);
  CHECK(contrast(light.on_brand, light.brand) > 3.0);
  CHECK(contrast(dark.on_brand, dark.brand) > 3.0);
  CHECK(contrast(light.bubble_in_text, light.bubble_in) > 4.5);
  CHECK(contrast(dark.bubble_in_text, dark.bubble_in) > 4.5);
  CHECK(contrast(light.bubble_out_text, light.bubble_out) > 3.0);
  CHECK(contrast(dark.bubble_out_text, dark.bubble_out) > 3.0);
  // 主窗接线新增的语义面（描边按钮文字／成功横幅／禁用底）
  check_new_token_contrast(light, QStringLiteral("light"));
  check_new_token_contrast(dark, QStringLiteral("dark"));
  // 亮暗不得共用同一批底色（否则「切换」在这些面上看不出差别）
  CHECK(light.brand_text.name() != dark.brand_text.name());
  CHECK(light.success_wash.name() != dark.success_wash.name());
  CHECK(light.disabled_bg.name() != dark.disabled_bg.name());
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

// —— ⑥ 需求批⑬：默认 20 套主题全链 ——
