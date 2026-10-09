// T4.9 主题切换验收（R19）＋需求批⑬ 默认 20 套：
// ① 令牌完备性与品牌橙恒定、亮暗两套关键面明显不同；
// ② 跟随系统解析：系统亮暗 → light／dark，手动选择覆盖系统；
// ③ 持久化往返：手动选择落盘，新实例读回（重启后保持）；
// ④ 令牌化 QSS 生成：占位符零残留、关键选择器齐、切换后样式变化（即时生效）；
// ⑤ 多主题扩展位：只注册一套令牌即可被选中／解析／落盘；
// ⑥ 默认 20 套（⑬）：22 套 builtin 全部令牌/排版完备、对比度门槛过、
//    品牌橙恒同、底色互异、字体/密度成套、tokens_for 出真令牌、
//    选择持久化往返、离屏渲染像素差（另可 MEMEX_THEME_SHOT_DIR 落实截）；
// ⑦ 应用级落地：调色板色与令牌一致、系统亮暗变化仅在跟随模式下重应用；
// ⑧ 设置页交互：列表点选暗色／⑬ 默认主题／跟随系统即时生效并回灌选中态；
//    ⑭ 面板改色（改色→自定义槽＋恢复默认）与毛玻璃开关（降级提示）；
// ⑨ 主窗接线：控件样式里的颜色全部来自令牌（零字面量）、主题切换后
//    控件样式与聊天区富文本同步重渲、「设置 → 主题…」菜单入口存在；
// ⑩ 自定义主题（⑭）：8 枚中性令牌放行面、品牌／语义拒绝、基线锁定＋
//    自动切槽、重启往返、恢复默认回基线、覆盖层丢失回落、纵深防御；
// ⑪ 毛玻璃特效（⑭）：默认关、开↔关落盘往返、不支持平台降级（生效态恒
//    false）、支持腿探针注入、apply 后不崩；
// ⑫ 皮肤包体系（⑮）：清单校验面、导出导入往返、篡改一字节拒、先验签再
//    解析（缺字段＋错签名报签名）、format 不符拒、防坏包硬底线（缺条目/
//    路径穿越/条目数/单条/总量/清单大小/压缩方式/非 zip）、semver 比较、
//    ThemeManager 安装流（自动选中＋令牌生效＋品牌恒同）、版本比较（同版
//    拒/低版拒/高版替换）、撞名拒、导出缝（包原样/内置·皮肤/自定义·改色/
//    不可导出腿）、重启重注册（坏包残留跳过）、设置页两钮路径（成功/篡改
//    拒/「（皮肤包）」标注/目录记忆/内置导出腿）。
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPalette>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QWidget>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#include <app/main_window.hpp>
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

// QSS 里有未替换的令牌占位符即视为漏色（颜色又散落回字面量的信号）；
// ⑬ 后字体/密度五槽位同口径（漏替换＝排版取值散落）
bool has_placeholder(const QString& qss) {
  for (const char* token :
       {"%text%", "%surface%", "%brand%", "%font_family%", "%font_pt%",
        "%radius%", "%pad_sm%", "%pad_md%"}) {
    if (qss.contains(QLatin1String(token))) return true;
  }
  return false;
}

// 抓样式里的十六进制字面量：#rgb／#rrggbb（排除锚点 # 之外的都算色值）
std::set<QString> hex_literals(const QString& qss) {
  static const QRegularExpression re(QStringLiteral("#[0-9a-fA-F]{3,8}"));
  std::set<QString> found;
  auto it = re.globalMatch(qss);
  while (it.hasNext()) found.insert(it.next().captured());
  return found;
}

// —— 亮暗两套都必须给出的新增令牌（主窗接线用到的语义面）——
void check_new_token_contrast(const ThemeTokens& t, const QString& label) {
  CHECK(contrast(t.brand_text, t.surface_raised) > 4.5);
  CHECK(contrast(t.brand_text, t.brand_tint) > 3.0);
  CHECK(contrast(t.success_text, t.success_wash) > 4.5);
  CHECK(contrast(t.brand_wash_text, t.brand_wash) > 4.5);
  CHECK(contrast(t.bubble_out_text, t.bubble_out) > 3.0);
  CHECK(luminance(t.disabled_bg) > 0.0);
  Q_UNUSED(label);
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

}  // namespace

// —— ⑫ 皮肤包体系（⑮）：导出导入往返、先验签再解析、防坏包硬底线、
//    版本比较、ThemeManager 注册与重启往返、设置页两钮路径 ——
namespace {

QByteArray signed_zip_json(const QByteArray& json) {
  return memex::client::build_skin_zip(
      {{QStringLiteral("manifest.json"), json},
       {QStringLiteral("signature.txt"), memex::client::sign_skin_bytes(json)}},
      nullptr);
}

memex::client::SkinPackManifest sample_manifest() {
  memex::client::SkinPackManifest m;
  m.name = QStringLiteral("晨雾·改·皮肤包");
  m.version = QStringLiteral("1.0.0");
  m.author = QStringLiteral("测试作者");
  m.base = QStringLiteral("light");
  m.overrides.insert(QStringLiteral("surface"), QColor(QStringLiteral("#223344")));
  m.overrides.insert(QStringLiteral("input_bg"), QColor(QStringLiteral("#aabbcc")));
  return m;
}

}  // namespace

void test_skin_pack() {
  using memex::client::build_skin_zip;
  using memex::client::export_skin_pack;
  using memex::client::import_skin_pack;
  using memex::client::parse_skin_manifest;
  using memex::client::sign_skin_bytes;
  using memex::client::SkinPackImport;
  using memex::client::SkinPackManifest;

  // —— 静态校验面：包名／版本／基线／覆盖面 ——
  const SkinPackManifest sample = sample_manifest();
  CHECK(memex::client::validate_skin_manifest(sample).isEmpty());
  SkinPackManifest bad = sample;
  bad.name = QStringLiteral("light");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 撞内置
  bad.name = ThemeManager::customThemeName();
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 撞自定义槽
  bad.name = QStringLiteral("a/b");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 非法字符
  bad = sample;
  bad.version = QStringLiteral("1.2");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 非 semver
  bad.base = QStringLiteral("不存在");
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());
  bad = sample;
  bad.overrides.insert(QStringLiteral("brand"), QColor(QStringLiteral("#ff0000")));
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 品牌锁死
  bad.overrides.clear();
  bad.overrides.insert(QStringLiteral("danger"), QColor(QStringLiteral("#0000ff")));
  CHECK(!memex::client::validate_skin_manifest(bad).isEmpty());  // 语义锁死

  // —— 清单 JSON 往返＋缺字段／坏值（签名链之外的清单腿）——
  const QByteArray json = memex::client::skin_manifest_json(sample);
  QString err;
  const SkinPackManifest parsed = parse_skin_manifest(json, &err);
  CHECK(err.isEmpty());
  CHECK(parsed.same_as(sample));
  {
    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject obj = doc.object();
    obj.remove(QStringLiteral("version"));
    parse_skin_manifest(QJsonDocument(obj).toJson(QJsonDocument::Compact), &err);
    CHECK(err.contains(QStringLiteral("version")));
    obj.insert(QStringLiteral("version"), QStringLiteral("not-semver"));
    parse_skin_manifest(QJsonDocument(obj).toJson(QJsonDocument::Compact), &err);
    CHECK(err.contains(QStringLiteral("semver")));
    obj.insert(QStringLiteral("version"), sample.version);
    obj.insert(QStringLiteral("overrides"),
               QJsonObject{{QStringLiteral("surface"),
                            QStringLiteral("#12")}});
    parse_skin_manifest(QJsonDocument(obj).toJson(QJsonDocument::Compact), &err);
    CHECK(err.contains(QStringLiteral("HexArgb")));
  }

  // —— 导出→导入往返一致（含中文名与多枚覆盖）——
  QString export_err;
  const QByteArray zip = export_skin_pack(sample, &export_err);
  CHECK(export_err.isEmpty());
  CHECK(!zip.isEmpty());
  const SkinPackImport round = import_skin_pack(zip);
  CHECK(round.ok);
  CHECK(round.error.isEmpty());
  CHECK(round.manifest.same_as(sample));

  // —— 篡改一字节拒（在 CRC 结构层即拒；签名层语义由重打包错签腿覆盖）——
  {
    QByteArray tampered = zip;
    const int json_pos = tampered.indexOf(json);
    CHECK(json_pos > 0);
    tampered[json_pos + 2] =
        static_cast<char>(quint8(tampered[json_pos + 2]) ^ 0x01);
    const SkinPackImport bad_pack = import_skin_pack(tampered);
    CHECK(!bad_pack.ok);
    CHECK(!bad_pack.error.isEmpty());
  }

  // —— 先验签再解析：缺字段＋错签名 → 报签名而非缺字段 ——
  {
    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject obj = doc.object();
    obj.remove(QStringLiteral("version"));
    const QByteArray bad_json =
        QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QList<QPair<QString, QByteArray>> entries;
    entries.append({QStringLiteral("manifest.json"), bad_json});
    entries.append({QStringLiteral("signature.txt"),
                    QStringLiteral("deadbeef").toLatin1()});
    const SkinPackImport bad_pack = import_skin_pack(build_skin_zip(entries, nullptr));
    CHECK(!bad_pack.ok);
    CHECK(bad_pack.error.contains(QStringLiteral("签名")));
    // 同包＋正确签名 → 报缺字段（验签过了才轮到清单）
    const SkinPackImport missing = import_skin_pack(signed_zip_json(bad_json));
    CHECK(!missing.ok);
    CHECK(missing.error.contains(QStringLiteral("version")));
  }

  // —— format 字段不符（外来 JSON）拒 ——
  {
    QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonObject obj = doc.object();
    obj.remove(QStringLiteral("format"));
    const SkinPackImport foreign =
        import_skin_pack(signed_zip_json(QJsonDocument(obj).toJson(
            QJsonDocument::Compact)));
    CHECK(!foreign.ok);
    CHECK(foreign.error.contains(QStringLiteral("memex")));
  }

  // —— 防坏包硬底线 ——
  {
    // 缺 manifest.json / 缺 signature.txt
    QList<QPair<QString, QByteArray>> only_sig;
    only_sig.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    CHECK(!import_skin_pack(build_skin_zip(only_sig, nullptr)).ok);
    QList<QPair<QString, QByteArray>> only_manifest;
    only_manifest.append({QStringLiteral("manifest.json"), json});
    const SkinPackImport no_sig = import_skin_pack(build_skin_zip(only_manifest, nullptr));
    CHECK(!no_sig.ok);
    CHECK(no_sig.error.contains(QStringLiteral("signature")));
    // 路径穿越／绝对路径条目名
    QList<QPair<QString, QByteArray>> traversal;
    traversal.append({QStringLiteral("../evil/manifest.json"), json});
    traversal.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    const SkinPackImport evil = import_skin_pack(build_skin_zip(traversal, nullptr));
    CHECK(!evil.ok);
    CHECK(evil.error.contains(QStringLiteral("路径")));
    QList<QPair<QString, QByteArray>> absolute;
    absolute.append({QStringLiteral("/etc/manifest.json"), json});
    absolute.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    CHECK(!import_skin_pack(build_skin_zip(absolute, nullptr)).ok);
    // 条目数超限（40 条）
    QList<QPair<QString, QByteArray>> many;
    for (int i = 0; i < 40; ++i) {
      many.append({QStringLiteral("extra%1.bin").arg(i), QByteArray(8, 'x')});
    }
    many.append({QStringLiteral("manifest.json"), json});
    many.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    const SkinPackImport overflow = import_skin_pack(build_skin_zip(many, nullptr));
    CHECK(!overflow.ok);
    CHECK(overflow.error.contains(QStringLiteral("条目数")));
    // 单条过大（2 MiB）
    QList<QPair<QString, QByteArray>> huge;
    huge.append({QStringLiteral("manifest.json"),
                 QByteArray(2 * 1024 * 1024, 'x')});
    const SkinPackImport big = import_skin_pack(build_skin_zip(huge, nullptr));
    CHECK(!big.ok);
    CHECK(big.error.contains(QStringLiteral("单条")));
    // 内容总量超限（5 × 900 KiB > 4 MiB）
    QList<QPair<QString, QByteArray>> heavy;
    for (int i = 0; i < 5; ++i) {
      heavy.append({QStringLiteral("extra%1.bin").arg(i),
                    QByteArray(900 * 1024, 'x')});
    }
    heavy.append({QStringLiteral("manifest.json"), json});
    heavy.append({QStringLiteral("signature.txt"), sign_skin_bytes(json)});
    const SkinPackImport fat_import =
        import_skin_pack(build_skin_zip(heavy, nullptr));
    CHECK(!fat_import.ok);
    CHECK(fat_import.error.contains(QStringLiteral("总量")));
    // 清单过大（300 KiB，签名合法→在清单大小底线拒）
    const QByteArray big_json(300 * 1024, 'a');
    const SkinPackImport big_manifest = import_skin_pack(signed_zip_json(big_json));
    CHECK(!big_manifest.ok);
    CHECK(big_manifest.error.contains(QStringLiteral("清单过大")));
    // 非 stored 压缩方式（字节手术改 method 字段）
    QByteArray deflated = zip;
    const int cd = deflated.indexOf(QByteArrayLiteral("\x50\x4b\x01\x02"));
    const int local = deflated.indexOf(QByteArrayLiteral("\x50\x4b\x03\x04"));
    CHECK(cd >= 0 && local >= 0);
    deflated[cd + 10] = 0x08;
    deflated[local + 8] = 0x08;
    const SkinPackImport compressed = import_skin_pack(deflated);
    CHECK(!compressed.ok);
    CHECK(compressed.error.contains(QStringLiteral("压缩")));
    // 非 zip 字节
    CHECK(!import_skin_pack(QByteArrayLiteral("not a zip at all")).ok);
  }

  // —— semver 比较 ——
  CHECK(memex::client::compare_skin_versions(QStringLiteral("1.2.3"),
                                             QStringLiteral("1.10.0")) < 0);
  CHECK(memex::client::compare_skin_versions(QStringLiteral("2.0.0"),
                                             QStringLiteral("1.99.99")) > 0);
  CHECK(memex::client::compare_skin_versions(QStringLiteral("1.2.3"),
                                             QStringLiteral("1.2.3")) == 0);

  // —— ThemeManager 安装流：注册＋选中＋令牌生效＋版本比较 ——
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());

  ThemeManager manager;
  manager.set_system_dark_probe([] { return false; });
  manager.set_mode(QString::fromUtf8(ThemeManager::kLight));

  QString install_err;
  CHECK(manager.install_skin_pack(zip, &install_err));
  CHECK(install_err.isEmpty());
  CHECK(manager.mode() == sample.name);  // 导入成功自动选中
  CHECK(manager.has_theme(sample.name));
  CHECK(manager.skin_pack_themes().contains(sample.name));
  CHECK(manager.tokens().surface.name() == QStringLiteral("#223344"));
  CHECK(manager.tokens().input_bg.name() == QStringLiteral("#aabbcc"));
  CHECK(manager.tokens().text.name() ==
        ThemeManager::tokens_for(QStringLiteral("light")).text.name());
  CHECK(manager.tokens().brand.name() == QStringLiteral("#e16531"));  // 品牌恒同
  CHECK(manager.tokens().is_complete());

  // 版本比较：同版本／低版本拒（如实注明语义＝须严格更高），高版本替换
  {
    QString err2;
    CHECK(!manager.install_skin_pack(zip, &err2));  // 同版本
    CHECK(err2.contains(QStringLiteral("更高")));
    SkinPackManifest older = sample;
    older.version = QStringLiteral("0.9.0");
    CHECK(!manager.install_skin_pack(export_skin_pack(older, nullptr), &err2));
    CHECK(err2.contains(QStringLiteral("更高")));
    SkinPackManifest newer = sample;
    newer.version = QStringLiteral("1.1.0");
    newer.overrides.insert(QStringLiteral("surface"),
                           QColor(QStringLiteral("#445566")));
    CHECK(manager.install_skin_pack(export_skin_pack(newer, nullptr), &err2));
    CHECK(manager.tokens().surface.name() == QStringLiteral("#445566"));
  }

  // 撞非包扩展主题：注册表占用拒绝，不覆盖
  {
    ThemeTokens foreign_tokens;
    foreign_tokens.surface = QColor(QStringLiteral("#123123"));
    manager.register_theme(QStringLiteral("外部扩展"), foreign_tokens);
    SkinPackManifest clash = sample;
    clash.name = QStringLiteral("外部扩展");
    clash.version = QStringLiteral("9.0.0");
    QString err3;
    CHECK(!manager.install_skin_pack(export_skin_pack(clash, nullptr), &err3));
    CHECK(err3.contains(QStringLiteral("占用")));
  }

  // —— 导出缝：皮肤包原样；内置「·皮肤」；自定义槽「·改色」；不可导出腿 ——
  {
    const SkinPackManifest current = manager.current_skin_manifest();
    // 皮肤包主题原样导出（此刻已是 1.1.0 替换装）
    CHECK(current.name == sample.name);
    CHECK(current.version == QStringLiteral("1.1.0"));
    CHECK(current.overrides.value(QStringLiteral("surface")) ==
          QColor(QStringLiteral("#445566")));
    // 内置主题：「<名>·皮肤」无覆盖；导出包可再导入并注册
    manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
    const SkinPackManifest builtin_out = manager.current_skin_manifest();
    CHECK(builtin_out.name == QStringLiteral("light·皮肤"));
    CHECK(builtin_out.base == QStringLiteral("light"));
    CHECK(builtin_out.overrides.isEmpty());
    const QByteArray builtin_zip =
        export_skin_pack(builtin_out, nullptr);
    const SkinPackImport reimport = import_skin_pack(builtin_zip);
    CHECK(reimport.ok);
    CHECK(reimport.manifest.same_as(builtin_out));
    QString install_err4;
    CHECK(manager.install_skin_pack(builtin_zip, &install_err4));
    CHECK(manager.mode() == QStringLiteral("light·皮肤"));
    CHECK(manager.tokens().surface.name() ==
          ThemeManager::tokens_for(QStringLiteral("light")).surface.name());
    // 自定义槽：先回内置基线再改色（基线为皮肤包时导出按口径拒绝，
    // 不在本腿覆盖）；带覆盖表导出并往返
    manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
    CHECK(manager.set_custom_override(QStringLiteral("surface"),
                                      QColor(QStringLiteral("#010203"))));
    const SkinPackManifest custom_out = manager.current_skin_manifest();
    CHECK(custom_out.name == QStringLiteral("light·改色"));
    CHECK(custom_out.base == QStringLiteral("light"));
    CHECK(custom_out.overrides.value(QStringLiteral("surface")) ==
          QColor(QStringLiteral("#010203")));
    const SkinPackImport custom_re =
        import_skin_pack(export_skin_pack(custom_out, nullptr));
    CHECK(custom_re.ok);
    CHECK(custom_re.manifest.same_as(custom_out));
    // 不可导出腿：非包扩展主题
    manager.set_mode(QStringLiteral("外部扩展"));
    QString export_error;
    CHECK(manager.current_skin_manifest(&export_error).name.isEmpty());
    CHECK(!export_error.isEmpty());
    manager.set_mode(sample.name);  // 收尾回到包主题：重启腿从持久化选中态读起
  }

  // —— 重启往返：已装包重注册＋选中持久化；坏包残留跳过不阻断 ——
  {
    QSettings writer;
    writer.setValue(QStringLiteral("appearance/skins/坏包/manifest"),
                    QStringLiteral("{not json"));
    ThemeManager m2;
    m2.set_system_dark_probe([] { return false; });
    CHECK(m2.mode() == sample.name);  // 版本 1.1.0 已是选中态
    CHECK(m2.skin_pack_themes().contains(sample.name));
    CHECK(m2.tokens().surface.name() == QStringLiteral("#445566"));
    CHECK(!m2.skin_pack_themes().contains(QStringLiteral("坏包")));
    CHECK(m2.has_theme(QStringLiteral("light·皮肤")));  // 首包同批重注册
  }

  // —— 设置页两钮路径（免对话框缝）＋「（皮肤包）」标注＋目录记忆 ——
  // 隔离口径：ThemeManager 落盘走 QSettings(org,app)＝NativeFormat（全 app
  // 统一，setDefaultFormat/setPath(INI) 只影响 QSettings() 默认构造）——
  // 真隔离＝清同一 conf
  {
    QTemporaryDir ui_tmp;
    CHECK(ui_tmp.isValid());
    QSettings(QCoreApplication::organizationName(),
              QCoreApplication::applicationName())
        .clear();
    ThemeManager ui_manager;
    ui_manager.set_system_dark_probe([] { return false; });
    ThemeSettingsPage page(&ui_manager);
    QPushButton* import_btn =
        page.findChild<QPushButton*>(QStringLiteral("importSkinButton"));
    QPushButton* export_btn =
        page.findChild<QPushButton*>(QStringLiteral("exportSkinButton"));
    QLabel* status = page.findChild<QLabel*>(QStringLiteral("skinStatus"));
    CHECK(import_btn && export_btn && status);
    CHECK(status->text().isEmpty());

    // 导入：好包→成功状态＋自动选中＋列表项带「（皮肤包）」
    const QString pack_path =
        ui_tmp.filePath(QStringLiteral("pack.zip"));
    QFile pack_file(pack_path);
    CHECK(pack_file.open(QIODevice::WriteOnly));
    pack_file.write(zip);
    pack_file.close();
    CHECK(page.import_skin_from_path(pack_path));
    CHECK(status->text().contains(QStringLiteral("成功")));
    CHECK(ui_manager.mode() == sample.name);
    QListWidget* list = page.findChild<QListWidget*>(QStringLiteral("themeModes"));
    CHECK(list);
    bool labeled = false;
    for (int i = 0; i < list->count(); ++i) {
      if (list->item(i)->data(Qt::UserRole).toString() == sample.name) {
        CHECK(list->item(i)->text().contains(QStringLiteral("皮肤包")));
        labeled = true;
      }
    }
    CHECK(labeled);
    // 目录记忆落盘（app_settings 同口径：QSettings(org,app)＝NativeFormat）
    QSettings memory(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
    CHECK(memory.value(QStringLiteral("ui/skin_pack_dir")).toString() ==
          QFileInfo(pack_path).absolutePath());

    // 导入：篡改包→语义化拒绝＋模式不变（单字节翻转在 CRC 层拒）
    {
      QByteArray tampered = zip;
      const int json_pos = tampered.indexOf(json);
      tampered[json_pos + 4] =
          static_cast<char>(quint8(tampered[json_pos + 4]) ^ 0x02);
      const QString bad_path = ui_tmp.filePath(QStringLiteral("bad.zip"));
      QFile bad_file(bad_path);
      CHECK(bad_file.open(QIODevice::WriteOnly));
      bad_file.write(tampered);
      bad_file.close();
      CHECK(!page.import_skin_from_path(bad_path));
      CHECK(!status->text().isEmpty());
      CHECK(ui_manager.mode() == sample.name);
    }

    // 导出：皮肤包原样落文件，可再导入往返
    const QString out_path = ui_tmp.filePath(QStringLiteral("out.zip"));
    CHECK(page.export_skin_to_path(out_path));
    CHECK(status->text().contains(QStringLiteral("已导出")));
    CHECK(QFile(out_path).size() > 0);
    QFile back_file(out_path);
    CHECK(back_file.open(QIODevice::ReadOnly));
    const SkinPackImport back = import_skin_pack(back_file.readAll());
    CHECK(back.ok);
    CHECK(back.manifest.same_as(sample));

    // 内置导出腿（选中 light）：文件可再导入注册「light·皮肤」
    ui_manager.set_mode(QString::fromUtf8(ThemeManager::kLight));
    CHECK(ui_manager.mode() == QString::fromUtf8(ThemeManager::kLight));
    const QString light_out = ui_tmp.filePath(QStringLiteral("light.zip"));
    CHECK(page.export_skin_to_path(light_out));
    CHECK(QFile(light_out).size() > 0);
  }
}

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
  test_default_themes();
  test_apply_to_app(app);
  test_settings_page(app);
  test_main_window_wiring(app);
  test_custom_theme();
  test_frosted_effect(app);
  test_skin_pack();

  if (g_failures == 0) {
    qInfo("test_theme: ALL PASS");
    return 0;
  }
  qCritical("test_theme: %d FAILURES", g_failures);
  return 1;
}