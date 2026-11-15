// test_theme 拆分模块（LAY-011）：本域场景体自原文件逐字搬移；公共面见 common.hpp。
#include "common.hpp"

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

// —— ⑧ 设置页交互：列表点选即时生效并回灌选中态；⑭ 面板改色与特效开关 ——


void test_settings_page(QApplication& app) {
  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });
  manager.apply(&app);

  ThemeSettingsPage page(&manager);
  CHECK(page.effective_theme() == QString::fromUtf8(ThemeManager::kLight));

  // ⑬ 起 23+ 项（跟随系统＋内置 22＋⑭ 自定义槽）走 QListWidget，
  // item UserRole 携带模式名
  auto* list = page.findChild<QListWidget*>(QStringLiteral("themeModes"));
  CHECK(list != nullptr);
  CHECK(list->count() == 24);
  const auto row_of = [&](QListWidget* widget, const QString& mode) {
    for (int i = 0; i < widget->count(); ++i) {
      if (widget->item(i)->data(Qt::UserRole).toString() == mode) return i;
    }
    return -1;
  };

  const int dark_row = row_of(list, QString::fromUtf8(ThemeManager::kDark));
  CHECK(dark_row >= 0);
  if (dark_row >= 0) {
    list->itemClicked(list->item(dark_row));  // 真点选：与用户操作同路径
    CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kDark));
    CHECK(page.effective_theme() == QString::fromUtf8(ThemeManager::kDark));
    CHECK(list->currentRow() == dark_row);  // 选中态回灌
  }

  // ⑬ 默认主题同样可点选即时生效
  const int mist_row = row_of(list, QStringLiteral("晨雾·亮"));
  CHECK(mist_row >= 0);
  if (mist_row >= 0) {
    list->itemClicked(list->item(mist_row));
    CHECK(manager.mode() == QStringLiteral("晨雾·亮"));
    CHECK(page.effective_theme() == QStringLiteral("晨雾·亮"));
    CHECK(list->currentRow() == mist_row);
  }

  const int system_row =
      row_of(list, QString::fromUtf8(ThemeManager::kFollowSystem));
  CHECK(system_row >= 0);
  if (system_row >= 0) {
    list->itemClicked(list->item(system_row));
    CHECK(manager.mode() == QString::fromUtf8(ThemeManager::kFollowSystem));
    CHECK(manager.effective_theme() == QString::fromUtf8(ThemeManager::kLight));
    CHECK(list->currentRow() == system_row);
  }

  // 注册扩展主题后设置页立即出现对应选项（只新增令牌这一件事就够），
  // 且带「（扩展主题）」标注；⑬ 内置主题不标注
  ThemeTokens extra = ThemeManager::tokens_for(
      QString::fromUtf8(ThemeManager::kDark));
  extra.surface = QColor(QStringLiteral("#101820"));
  manager.register_theme(QStringLiteral("deepsea"), extra);
  ThemeSettingsPage page2(&manager);
  auto* list2 = page2.findChild<QListWidget*>(QStringLiteral("themeModes"));
  CHECK(list2 != nullptr);
  const int deepsea_row = list2 ? row_of(list2, QStringLiteral("deepsea")) : -1;
  CHECK(deepsea_row >= 0);
  if (list2 && deepsea_row >= 0) {
    CHECK(list2->item(deepsea_row)->text().contains(
        QStringLiteral("扩展主题")));
    const int builtin_row = row_of(list2, QStringLiteral("晨雾·亮"));
    CHECK(builtin_row >= 0);
    if (builtin_row >= 0) {
      CHECK(list2->item(builtin_row)->text() == QStringLiteral("晨雾·亮"));
    }
  }
  CHECK(page2.select_mode(QStringLiteral("deepsea")));
  CHECK(manager.effective_theme() == QStringLiteral("deepsea"));
  CHECK(!page2.select_mode(QStringLiteral("nope")));

  // —— ⑭ 面板改色（UI 路径）：改色→自动切自定义槽＋恢复默认回基线 ——
  auto* reset_btn =
      page2.findChild<QPushButton*>(QStringLiteral("resetCustomColors"));
  CHECK(reset_btn != nullptr);
  CHECK(reset_btn && !reset_btn->isEnabled());  // 无覆盖＝不可用
  CHECK(page2.apply_custom_color(QStringLiteral("surface"),
                                 QColor(QStringLiteral("#112233"))));
  CHECK(manager.mode() == ThemeManager::customThemeName());
  CHECK(manager.tokens().surface.name() == QStringLiteral("#112233"));
  CHECK(reset_btn && reset_btn->isEnabled());  // 有覆盖＝可用
  // 品牌／语义令牌不放行（UI 同径拒绝）
  CHECK(!page2.apply_custom_color(QStringLiteral("brand"),
                                  QColor(QStringLiteral("#ff0000"))));
  CHECK(!page2.apply_custom_color(QStringLiteral("success"),
                                  QColor(QStringLiteral("#00ff00"))));
  // 恢复默认：回基线（deepsea 改色前所在）……实际基线＝首个改色时锁定的
  // 生效主题（deepsea，已注册扩展主题也可作基线）
  CHECK(manager.custom_theme_base() == QStringLiteral("deepsea"));
  reset_btn->click();
  CHECK(!manager.has_custom_overrides());
  CHECK(manager.mode() == QStringLiteral("deepsea"));
  CHECK(manager.tokens().surface.name() == QStringLiteral("#101820"));

  // —— ⑭ 毛玻璃开关（UI 路径）：本机不支持＝开而降级，提示语如实 ——
  auto* frosted =
      page2.findChild<QCheckBox*>(QStringLiteral("frostedEffectCheck"));
  auto* hint = page2.findChild<QLabel*>(QStringLiteral("frostedEffectHint"));
  CHECK(frosted != nullptr);
  CHECK(hint != nullptr);
  if (frosted && hint) {
    frosted->setChecked(true);  // 与用户勾选同路径（toggled→set_frosted）
    CHECK(manager.frosted_effect_enabled());
    CHECK(!manager.frosted_effect_active());  // Linux 恒不支持＝降级
    CHECK(hint->text().contains(QStringLiteral("不支持")));
    frosted->setChecked(false);
    CHECK(!manager.frosted_effect_enabled());
  }
}

// —— ⑨ 主窗接线：控件样式走令牌、切换即重渲、菜单入口可达 ——
// 真起一个 MainWindow（生产路径：ThemeManager::instance()——main.cpp
// 起窗前 apply、设置页与主窗重刷都接在 instance 上，测试驱动同一实例）。
// 「颜色全部来自令牌」的运行时口径：样式里的每个十六进制值都必须是
// 当前主题某个令牌的取值（令牌值经 QColor::name() 以 hex 写入样式是
// 设计内行为；要抓的是绕开令牌散落的硬编码色）。


void test_main_window_wiring(QApplication& app) {
  ThemeManager& manager = ThemeManager::instance();
  manager.set_system_dark_probe([] { return false; });
  manager.apply(&app);

  memex::client::MainWindow window;
  window.show();

  // 当前主题令牌取值集合：样式里允许出现的全部 hex
  QList<QString> token_values;
  const QHash<QString, QColor> token_map =
      ThemeManager::tokens_for(manager.effective_theme()).as_map();
  for (const QColor& c : token_map.values()) token_values << c.name();

  const auto strays = [&](const QString& sheet) {
    std::set<QString> bad;
    for (const QString& color : hex_literals(sheet)) {
      if (!token_values.contains(color)) bad.insert(color);
    }
    return bad;
  };

  // 收集主窗全部控件样式表：非令牌色零容忍
  std::set<QString> literals;
  const auto widgets = window.findChildren<QWidget*>();
  CHECK(!widgets.isEmpty());
  for (const QWidget* w : widgets) {
    for (const QString& color : strays(w->styleSheet())) {
      qCritical("控件 %s [%s] 样式含非令牌色：%s",
                w->metaObject()->className(), qPrintable(w->objectName()),
                qPrintable(color));
      literals.insert(color);
    }
  }
  CHECK(literals.empty());

  // 全局 QSS：占位符零残留，且每个 hex 都是令牌值
  //（④ 已断言 QSS 必含品牌橙等令牌取值——与「零 hex」互斥，故口径统一为令牌值）
  CHECK(!has_placeholder(app.styleSheet()));
  {
    const std::set<QString> bad = strays(app.styleSheet());
    for (const QString& c : bad) {
      qCritical("全局 QSS 含非令牌色：%s", qPrintable(c));
    }
    CHECK(bad.empty());
  }

  // 「设置 → 主题…」入口在菜单里
  bool has_theme_action = false;
  const auto menus = window.menuBar()->findChildren<QMenu*>();
  for (const QMenu* menu : menus) {
    for (const QAction* action : menu->actions()) {
      if (action->text().contains(QStringLiteral("主题"))) {
        has_theme_action = true;
      }
    }
  }
  CHECK(has_theme_action);

  // 切换暗色：控件样式立即换成暗色令牌（品牌橙恒同，故查 surface）
  auto* device_list =
      window.findChild<QListWidget*>(QStringLiteral("device_list"));
  CHECK(device_list != nullptr);
  const QString light_qss = device_list ? device_list->styleSheet() : QString();
  auto* banner = window.findChild<QLabel*>(QStringLiteral("mode_banner"));
  CHECK(banner != nullptr);
  manager.set_mode(QString::fromUtf8(ThemeManager::kDark));
  if (device_list) {
    CHECK(device_list->styleSheet() != light_qss);
    CHECK(device_list->styleSheet().contains(
        ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kDark))
            .surface_alt.name()));
  }
  if (banner) {
    CHECK(banner->styleSheet().contains(
        ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kDark))
            .brand_wash.name()));
  }

  // 复位到亮色（下面富文本重渲用例要在亮色下起手）
  manager.set_mode(QString::fromUtf8(ThemeManager::kFollowSystem));

  // 聊天区富文本重渲：内联色（系统行/气泡底）不随全局 QSS 自动变，
  // 必须靠 chat_rows_ 重放。打开一个会话（有系统行）后切主题验证。
  window.open_direct_peer(QStringLiteral("theme-wiring-peer"));
  auto* chat = window.findChild<QTextBrowser*>();
  CHECK(chat != nullptr);
  const QString light_tokens_muted =
      ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kLight))
          .text_muted.name();
  const QString dark_tokens_muted =
      ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kDark))
          .text_muted.name();
  if (chat) {
    const QString light_html = chat->toHtml();
    CHECK(light_html.contains(light_tokens_muted));
    CHECK(!light_html.contains(QStringLiteral("#9b8f86"))); // 旧字面量已除
    manager.set_mode(QString::fromUtf8(ThemeManager::kDark));
    const QString dark_html = chat->toHtml();
    CHECK(dark_html.contains(dark_tokens_muted));
    CHECK(!dark_html.contains(light_tokens_muted));
    // 消息正文仍在（重渲不是清空）
    CHECK(!dark_html.trimmed().isEmpty());
  }

  manager.set_mode(QString::fromUtf8(ThemeManager::kFollowSystem));
  window.close();
}

// —— ⑩ 需求批⑭：自定义主题（面板改色覆盖层）——
