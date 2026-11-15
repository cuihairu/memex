// test_theme 拆分模块（LAY-011）：本域场景体自原文件逐字搬移；公共面见 common.hpp。
#include "common.hpp"

void test_custom_theme() {
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());

  // 放行面＝8 枚核心中性令牌；品牌橙与语义族名一律拒绝（不落盘）
  const QStringList allowed = ThemeManager::customizable_tokens();
  CHECK(allowed.size() == 8);
  for (const char* expect :
       {"surface", "surface_alt", "surface_raised", "border", "text",
        "text_muted", "chat_bg", "input_bg"}) {
    CHECK(allowed.contains(QLatin1String(expect)));
  }
  for (const char* locked : {"brand", "brand_hover", "brand_text", "on_brand",
                             "brand_tint", "brand_wash", "brand_wash_text",
                             "success", "success_wash", "success_text",
                             "warning", "danger", "disabled_bg", "bubble_out",
                             "bubble_out_text", "bubble_in", "bubble_in_text",
                             "selection"}) {
    CHECK(!allowed.contains(QLatin1String(locked)));
  }

  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });
  manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
  CHECK(!manager.has_custom_overrides());
  CHECK(!manager.set_custom_override(QStringLiteral("brand"),
                                     QColor(QStringLiteral("#ff0000"))));
  CHECK(!manager.set_custom_override(QStringLiteral("success"),
                                     QColor(QStringLiteral("#00ff00"))));
  CHECK(!manager.set_custom_override(QStringLiteral("nope"),
                                     QColor(QStringLiteral("#123456"))));
  CHECK(!manager.has_custom_overrides());  // 拒绝＝零副作用

  // 首个改色：基线锁定当前主题＋自动切自定义槽＋未覆盖令牌走基线
  const ThemeTokens light = ThemeManager::tokens_for(
      QString::fromUtf8(ThemeManager::kLight));
  CHECK(manager.set_custom_override(QStringLiteral("surface"),
                                    QColor(QStringLiteral("#112233"))));
  CHECK(manager.mode() == ThemeManager::customThemeName());
  CHECK(manager.effective_theme() == ThemeManager::customThemeName());
  CHECK(manager.custom_theme_base() == QString::fromUtf8(ThemeManager::kLight));
  CHECK(manager.tokens().surface.name() == QStringLiteral("#112233"));
  CHECK(manager.tokens().text.name() == light.text.name());  // 未覆盖走基线
  CHECK(manager.tokens().brand.name() == QStringLiteral("#e16531"));  // 品牌恒同
  CHECK(manager.tokens().is_complete());
  // 自定义槽不随系统亮暗漂移（手动选择优先；基线已锁定）
  manager.set_system_dark_probe([] { return true; });
  CHECK(manager.tokens().surface.name() == QStringLiteral("#112233"));
  manager.set_system_dark_probe([] { return false; });

  // apply_overrides 纵深防御：外部构造的覆盖表带品牌／语义名也不放行
  QHash<QString, QColor> hostile;
  hostile.insert(QStringLiteral("brand"), QColor(QStringLiteral("#ff0000")));
  hostile.insert(QStringLiteral("success"), QColor(QStringLiteral("#00ff00")));
  hostile.insert(QStringLiteral("danger"), QColor(QStringLiteral("#0000ff")));
  const ThemeTokens defended =
      ThemeManager::apply_overrides(light, hostile);
  CHECK(defended.brand.name() == QStringLiteral("#e16531"));
  CHECK(defended.success.name() == light.success.name());
  CHECK(defended.danger.name() == light.danger.name());
  CHECK(defended.surface.name() == light.surface.name());  // 无放行覆盖＝原样

  // 重启往返：新实例读回覆盖层＋基线＋自定义态；在槽上继续改色基线不变
  {
    ThemeManager m2;
    m2.set_system_dark_probe([] { return false; });
    CHECK(m2.mode() == ThemeManager::customThemeName());
    CHECK(m2.custom_theme_base() == QString::fromUtf8(ThemeManager::kLight));
    CHECK(m2.tokens().surface.name() == QStringLiteral("#112233"));
    CHECK(m2.set_custom_override(QStringLiteral("text"),
                                  QColor(QStringLiteral("#fedcba"))));
    CHECK(m2.custom_theme_base() == QString::fromUtf8(ThemeManager::kLight));
    CHECK(m2.tokens().text.name() == QStringLiteral("#fedcba"));
    CHECK(m2.custom_overrides().size() == 2);
    CHECK(m2.custom_overrides().value(QStringLiteral("surface")) ==
          QColor(QStringLiteral("#112233")));
  }

  // 恢复默认：清覆盖＋回基线；重启后仍在基线（覆盖层与槽名皆不残留）
  {
    ThemeManager m3;
    m3.clear_custom_overrides();
    CHECK(!m3.has_custom_overrides());
    CHECK(m3.mode() == QString::fromUtf8(ThemeManager::kLight));
    CHECK(m3.tokens().surface.name() == light.surface.name());
  }
  {
    ThemeManager m4;
    CHECK(m4.mode() == QString::fromUtf8(ThemeManager::kLight));
    CHECK(!m4.has_custom_overrides());
    CHECK(m4.custom_overrides().isEmpty());
  }

  // 覆盖层丢了（换机后只剩槽名）：read_persisted_mode 回落基线不悬空。
  // 落盘真口径是 QSettings(org,app)＝NativeFormat（与 ThemeManager 同构），
  // setPath(INI) 只影响 QSettings() 默认构造——直接清 conf 里的覆盖组
  {
    QSettings wipe(QCoreApplication::organizationName(),
                   QCoreApplication::applicationName());
    wipe.remove(QStringLiteral("appearance/custom_colors"));
    ThemeManager m5;
    CHECK(m5.mode() == QString::fromUtf8(ThemeManager::kLight));
    CHECK(!m5.has_custom_overrides());
  }
}

// —— ⑪ 需求批⑭：毛玻璃特效开关（默认关；不支持平台降级不透明）——


void test_frosted_effect(QApplication& app) {
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());

  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });

  // 默认关；本机（Linux）平台恒不支持
  CHECK(!manager.frosted_effect_enabled());
  CHECK(!manager.frosted_effect_active());
  CHECK(!ThemeManager::frosted_effect_supported());

  // 开↔关往返＋落盘：不支持时开关可置位（切支持平台即生效）但生效态恒 false
  CHECK(!manager.set_frosted_effect_enabled(true));  // 返回 false＝降级未生效
  CHECK(manager.frosted_effect_enabled());
  CHECK(!manager.frosted_effect_active());
  {
    ThemeManager m2;
    CHECK(m2.frosted_effect_enabled());  // 重启读回开
  }
  CHECK(!manager.set_frosted_effect_enabled(false));
  CHECK(!manager.frosted_effect_enabled());
  {
    ThemeManager m3;
    CHECK(!m3.frosted_effect_enabled());  // 重启读回关
  }

  // 支持腿（探针注入＝模拟 Windows）：开且支持＝生效；关＝不生效
  manager.set_frosted_support_probe([] { return true; });
  CHECK(!manager.frosted_effect_active());  // 开关仍关
  CHECK(manager.set_frosted_effect_enabled(true));  // 返回 true＝真生效
  CHECK(manager.frosted_effect_active());

  // apply 到应用（离屏）：不崩、样式正常（无原生窗口句柄＝跳过特效路径）；
  // 运行中失去平台支持→立即降级（生效态 false，不花屏不崩）
  manager.apply(&app);
  CHECK(manager.applied());
  CHECK(!app.styleSheet().isEmpty());
  manager.set_frosted_support_probe([] { return false; });
  CHECK(!manager.frosted_effect_active());
  manager.set_frosted_support_probe([] { return true; });
  CHECK(manager.frosted_effect_active());

  // 收尾：关掉并复位（避免影响后续用例）
  manager.set_frosted_support_probe(nullptr);
  manager.set_frosted_effect_enabled(false);
  CHECK(!manager.frosted_effect_active());
}
