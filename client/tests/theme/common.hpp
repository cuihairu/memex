// test_theme 共性面（LAY-011 自 test_theme.cpp 匿名空间逐字搬移）。
// g_failures 改 inline 变量保跨 TU 单例计数；helper 函数 inline 化。
#pragma once

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

inline int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

inline double linearized(double c) {
  return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
inline double luminance(const QColor& c) {
  return 0.2126 * linearized(c.redF()) + 0.7152 * linearized(c.greenF()) +
         0.0722 * linearized(c.blueF());
}

// WCAG 对比度：亮者在上、暗者在下，与书写顺序无关
inline double contrast(const QColor& a, const QColor& b) {
  const double la = luminance(a);
  const double lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// QSS 里有未替换的令牌占位符即视为漏色（颜色又散落回字面量的信号）；
// ⑬ 后字体/密度五槽位同口径（漏替换＝排版取值散落）
inline bool has_placeholder(const QString& qss) {
  for (const char* token :
       {"%text%", "%surface%", "%brand%", "%font_family%", "%font_pt%",
        "%radius%", "%pad_sm%", "%pad_md%"}) {
    if (qss.contains(QLatin1String(token))) return true;
  }
  return false;
}

// 抓样式里的十六进制字面量：#rgb／#rrggbb（排除锚点 # 之外的都算色值）
inline std::set<QString> hex_literals(const QString& qss) {
  static const QRegularExpression re(QStringLiteral("#[0-9a-fA-F]{3,8}"));
  std::set<QString> found;
  auto it = re.globalMatch(qss);
  while (it.hasNext()) found.insert(it.next().captured());
  return found;
}

// —— 亮暗两套都必须给出的新增令牌（主窗接线用到的语义面）——
void check_new_token_contrast(const ThemeTokens& t, const QString& label) {
  CHECK(contrast(t.brand_text, t.surface_raised) > 4.5);
  CHECK(contrast(t.brand_text, t.brand_tint) > 4.5);
  CHECK(contrast(t.success_text, t.success_wash) > 4.5);
  CHECK(contrast(t.brand_wash_text, t.brand_wash) > 4.5);
  CHECK(contrast(t.bubble_out_text, t.bubble_out) > 3.0);
  CHECK(luminance(t.disabled_bg) > 0.0);
  Q_UNUSED(label);
}

// —— ① 令牌完备性／品牌橙恒定／亮暗差异 ——

}

void test_tokens();
void test_system_follow();
void test_persistence();
void test_stylesheet();
void test_theme_extension();
void test_default_themes();
void test_apply_to_app(QApplication& app);
void test_settings_page(QApplication& app);
void test_main_window_wiring(QApplication& app);
void test_custom_theme();
void test_frosted_effect(QApplication& app);
void test_skin_pack();
