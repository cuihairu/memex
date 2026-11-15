// test_theme 拆分模块（LAY-011）：本域场景体自原文件逐字搬移；公共面见 common.hpp。
#include "common.hpp"

void test_default_themes() {
  const QStringList builtins = ThemeManager::builtin_themes();
  CHECK(builtins.size() == 22);  // light/dark 锚点＋20 套默认
  CHECK(builtins.contains(QString::fromUtf8(ThemeManager::kLight)));
  CHECK(builtins.contains(QString::fromUtf8(ThemeManager::kDark)));
  CHECK(builtins.contains(QStringLiteral("晨雾·亮")));
  CHECK(builtins.contains(QStringLiteral("曜石·暗")));

  QSet<QString> surfaces;   // 底色三元组互异：套间真实可辨，非换皮同色
  QSet<QString> fonts;      // 字体族 ≥3
  QSet<int> font_pts;       // 字号 ≥2
  QSet<QString> densities;  // 圆角×边距组合 ≥3（密度成套）
  for (const QString& name : builtins) {
    const ThemeTokens t = ThemeManager::tokens_for(name);
    CHECK(t.is_complete());
    CHECK(t.is_typography_valid());
    // 品牌橙 #e16531 恒同：20 套默认与 light/dark 锚点一致
    CHECK(t.brand.name() == QStringLiteral("#e16531"));
    // 每套都要过对比度门槛（WCAG 口径与 ① 一致）
    CHECK(contrast(t.text, t.surface) > 4.5);
    CHECK(contrast(t.text_muted, t.surface_alt) > 3.0);
    CHECK(contrast(t.bubble_in_text, t.bubble_in) > 4.5);
    CHECK(contrast(t.on_brand, t.brand) > 3.0);
    CHECK(contrast(t.bubble_out_text, t.bubble_out) > 3.0);
    check_new_token_contrast(t, name);
    // 名实相符：亮套底必亮、暗套底必暗
    if (name.endsWith(QStringLiteral("·亮"))) {
      CHECK(luminance(t.surface) > 0.5);
    }
    if (name.endsWith(QStringLiteral("·暗"))) {
      CHECK(luminance(t.surface) < 0.2);
    }
    surfaces.insert(QStringLiteral("%1/%2/%3")
                        .arg(t.surface.name(), t.surface_alt.name(),
                             t.surface_raised.name()));
    fonts.insert(t.font_family);
    font_pts.insert(t.font_pt);
    densities.insert(QStringLiteral("%1/%2/%3")
                         .arg(t.radius)
                         .arg(t.pad_sm)
                         .arg(t.pad_md));

    // 每套 QSS：占位符零残留、hex 全是本套令牌值、字体/密度确实进场
    const QString qss = ThemeManager::stylesheet_for(t);
    CHECK(!has_placeholder(qss));
    QList<QString> allowed;
    const QHash<QString, QColor> token_map = t.as_map();
    for (const QColor& c : token_map.values()) allowed << c.name();
    for (const QString& color : hex_literals(qss)) {
      if (!allowed.contains(color)) {
        qCritical("主题 %s QSS 含非令牌色 %s", qPrintable(name),
                  qPrintable(color));
        CHECK(false);
      }
    }
    CHECK(qss.contains(t.font_family));
    CHECK(qss.contains(QStringLiteral("font-size: %1pt").arg(t.font_pt)));
    CHECK(qss.contains(QStringLiteral("border-radius: %1px").arg(t.radius)));
    CHECK(qss.contains(
        QStringLiteral("padding: %1px %2px").arg(t.pad_sm).arg(t.pad_md)));
  }
  CHECK(surfaces.size() == builtins.size());
  CHECK(fonts.size() >= 3);
  CHECK(font_pts.size() >= 2);
  CHECK(densities.size() >= 3);

  // tokens_for 出真令牌：默认主题不得静默回落 light
  CHECK(ThemeManager::tokens_for(QStringLiteral("晨雾·亮")).surface.name() !=
        ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kLight))
            .surface.name());
  CHECK(ThemeManager::tokens_for(QStringLiteral("曜石·暗")).surface.name() !=
        ThemeManager::tokens_for(QString::fromUtf8(ThemeManager::kLight))
            .surface.name());

  // has_theme/modes/set_mode 全链认得默认主题（非实例注册也可达）
  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });
  CHECK(manager.has_theme(QStringLiteral("晨雾·亮")));
  CHECK(manager.modes().contains(QStringLiteral("晨雾·亮")));
  manager.set_mode(QStringLiteral("晨雾·亮"));
  CHECK(manager.mode() == QStringLiteral("晨雾·亮"));
  CHECK(manager.effective_theme() == QStringLiteral("晨雾·亮"));
  CHECK(manager.tokens().surface.name() ==
        ThemeManager::tokens_for(QStringLiteral("晨雾·亮")).surface.name());

  // 持久化往返：选 ⑬ 默认主题名，重启后仍在（read_persisted_mode 认 builtin）
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
  {
    ThemeManager m1;
    m1.set_system_dark_probe([] { return false; });
    m1.set_mode(QStringLiteral("曜石·暗"));
    CHECK(m1.effective_theme() == QStringLiteral("曜石·暗"));
  }
  {
    ThemeManager m2;
    m2.set_system_dark_probe([] { return false; });
    CHECK(m2.mode() == QStringLiteral("曜石·暗"));
    CHECK(m2.effective_theme() == QStringLiteral("曜石·暗"));
    CHECK(m2.tokens().surface.name() ==
          ThemeManager::tokens_for(QStringLiteral("曜石·暗")).surface.name());
    m2.set_mode(QString::fromUtf8(ThemeManager::kFollowSystem));
  }

  // 离屏渲染像素差：一亮一暗两套新主题真实出图不同（非字符串层面成立）；
  // MEMEX_THEME_SHOT_DIR 指定时顺带落 PNG（xvfb 实截验收用，常规跑无副作用）。
  // probe 用 QLabel：裸 QWidget 不画 stylesheet background（WA_StyledBackground
  // 缺省关），grab 只会出默认色——曾误判为「三张图全同」。
  QLabel probe;
  probe.resize(240, 120);
  probe.show();
  QApplication::processEvents();
  const QStringList shots{QStringLiteral("晨雾·亮"), QStringLiteral("暖砂·亮"),
                          QStringLiteral("曜石·暗")};
  QList<QImage> images;
  for (const QString& name : shots) {
    const ThemeTokens t = ThemeManager::tokens_for(name);
    probe.setStyleSheet(
        QStringLiteral("background: %1;").arg(t.surface.name()));
    QApplication::processEvents();
    const QImage img = probe.grab().toImage();
    // 本套主题底色确实落到了像素上（不靠图间差异，单图自证）
    CHECK(img.pixelColor(4, 4).name() == t.surface.name());
    images << img;
  }
  CHECK(images.size() == shots.size());
  for (const QImage& img : images) {
    CHECK(img.size() == QSize(240, 120));
  }
  const auto diff_pixels = [](const QImage& a, const QImage& b) {
    int diff = 0;
    for (int y = 0; y < b.height() && y < a.height(); ++y) {
      for (int x = 0; x < b.width() && x < a.width(); ++x) {
        if (a.pixel(x, y) != b.pixel(x, y)) ++diff;
      }
    }
    return diff;
  };
  const int total = 240 * 120;
  CHECK(diff_pixels(images[0], images[2]) > total / 4);  // 亮 vs 暗：大面积差
  CHECK(diff_pixels(images[0], images[1]) > total / 10);  // 两套亮：底色色相差
  const QString shot_dir = qEnvironmentVariable("MEMEX_THEME_SHOT_DIR");
  if (!shot_dir.isEmpty()) {
    QDir().mkpath(shot_dir);
    for (int i = 0; i < images.size(); ++i) {
      images[i].save(QDir(shot_dir).filePath(
          QStringLiteral("%1.png")
              .arg(QString(shots[i]).replace(QChar(0x00b7), QLatin1Char('-')))));
    }
  }
  probe.hide();

  manager.set_mode(QString::fromUtf8(ThemeManager::kFollowSystem));
}

// —— ⑦ 应用级落地：调色板＋系统变化仅跟随模式重应用 ——
