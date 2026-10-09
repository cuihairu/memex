#include "theme.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>
#include <QWidget>
#include <QWindow>

#include <cmath>

#if defined(Q_OS_WIN)
#include <dwmapi.h>
#include <windows.h>
#pragma comment(lib, "dwmapi.lib")
#endif

namespace memex::client {
namespace {

// ⑭ Windows 毛玻璃落地：亚克力／Mica 经 DWM 系统背景材质（Win11 22H2+）。
// DwmSetWindowAttribute 属性 38＝DWMWA_SYSTEMBACKDROP_TYPE：
// DWMSBT_MAINWINDOW(2)=Mica（主窗推荐）、DWMSBT_NONE(1)=关闭；另设
// 属性 20＝DWMWA_USE_IMMERSIVE_DARK_MODE 随主题切暗色标题栏。
// 老系统不认识属性 38：调用失败返回值非 0、界面静默保持不透明（降级）。
// 本机无 Windows，此腿只编译不运行（nightly CI Windows 编译腿会覆盖编译）——
// 待 Windows 真机验证。
#if defined(Q_OS_WIN)
void apply_frosted_effect_to_window_impl(QWindow* window, bool enabled) {
  if (!window) return;
  HWND hwnd = reinterpret_cast<HWND>(window->winId());
  if (!hwnd) return;
  constexpr DWORD kSystemBackdropType = 38;   // DWMWA_SYSTEMBACKDROP_TYPE
  constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
  int backdrop = enabled ? 2 : 1;             // 2=Mica（主窗），1=None
  DwmSetWindowAttribute(hwnd, kSystemBackdropType, &backdrop,
                        sizeof(backdrop));
  // 暗色标题栏位（跟随主题；enabled 与否都设，保持视觉一致）
  BOOL dark = enabled ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));
}
#endif

// 品牌橙（R19 主色）：两主题恒同，只随底色调 hover
constexpr auto kBrandOrange = "#e16531";
constexpr auto kSettingsKey = "appearance/theme_mode";
// ⑭ 面板改色覆盖层与毛玻璃开关的落盘键（沿同一 appearance/ 命名空间口径）
constexpr auto kOverridesGroup = "appearance/custom_colors";
constexpr auto kCustomBaseKey = "appearance/custom_base";
constexpr auto kFrostedKey = "appearance/effects/frosted";
// ⑭ 用户可改色的核心中性令牌（品牌橙与 success/warning/danger 语义族锁死
// 不放行；selection/bubble_* 等随内置基线，扩展面留后续）
const QStringList kCustomizableTokens = {
    QStringLiteral("surface"),      QStringLiteral("surface_alt"),
    QStringLiteral("surface_raised"), QStringLiteral("border"),
    QStringLiteral("text"),         QStringLiteral("text_muted"),
    QStringLiteral("chat_bg"),      QStringLiteral("input_bg")};

ThemeTokens light_tokens() {
  ThemeTokens t;
  t.surface = QColor(QStringLiteral("#faf7f3"));
  t.surface_alt = QColor(QStringLiteral("#f4efe8"));
  t.surface_raised = QColor(QStringLiteral("#ffffff"));
  t.border = QColor(QStringLiteral("#e8e0d6"));
  t.divider = QColor(QStringLiteral("#efe8de"));
  t.text = QColor(QStringLiteral("#332b24"));
  // muted 不用旧界面的 #9b8f86——它在浅底上只有 2.75:1，低于可读门槛；
  // 令牌层按对比度守住（test_theme 断言 ≥3:1）
  t.text_muted = QColor(QStringLiteral("#7d7169"));
  t.brand = QColor(QString::fromUtf8(kBrandOrange));
  t.brand_hover = QColor(QStringLiteral("#c9541f"));
  // brand_text＝浅底上的品牌色文本（描边按钮文字、本机保存徽标）：
  // 直接压暗品牌橙到 5.3:1，保住「橙色系文字」观感又守住可读门槛
  t.brand_text = QColor(QStringLiteral("#a05a26"));
  t.on_brand = QColor(QStringLiteral("#ffffff"));
  t.brand_tint = QColor(QStringLiteral("#f6e3d7"));
  t.brand_wash = QColor(QStringLiteral("#fdeee2"));
  t.brand_wash_text = QColor(QStringLiteral("#8a4a1f"));
  t.success = QColor(QStringLiteral("#6f8f6a"));
  t.warning = QColor(QStringLiteral("#c98a3c"));
  t.success_wash = QColor(QStringLiteral("#eaf3e7"));
  t.success_text = QColor(QStringLiteral("#3f6b3a"));
  t.danger = QColor(QStringLiteral("#c0492f"));
  t.disabled_bg = QColor(QStringLiteral("#d9cfc4"));
  t.bubble_out = QColor(QStringLiteral("#e16531"));
  t.bubble_out_text = QColor(QStringLiteral("#ffffff"));
  t.bubble_in = QColor(QStringLiteral("#f0ebe5"));
  t.bubble_in_text = QColor(QStringLiteral("#332b24"));
  t.chat_bg = QColor(QStringLiteral("#ffffff"));
  t.selection = QColor(QStringLiteral("#f6e3d7"));
  t.input_bg = QColor(QStringLiteral("#ffffff"));
  return t;
}

ThemeTokens dark_tokens() {
  ThemeTokens t;
  t.surface = QColor(QStringLiteral("#191614"));
  t.surface_alt = QColor(QStringLiteral("#221e1b"));
  t.surface_raised = QColor(QStringLiteral("#2b2622"));
  t.border = QColor(QStringLiteral("#3b342d"));
  t.divider = QColor(QStringLiteral("#322c27"));
  t.text = QColor(QStringLiteral("#f2ece5"));
  t.text_muted = QColor(QStringLiteral("#a3978a"));
  t.brand = QColor(QString::fromUtf8(kBrandOrange));
  t.brand_hover = QColor(QStringLiteral("#ef7a4a"));
  // 暗底上的品牌色文字须提亮才可读（brand 本身在暗底只有 3.5:1）
  t.brand_text = QColor(QStringLiteral("#f0a07c"));
  t.on_brand = QColor(QStringLiteral("#ffffff"));
  t.brand_tint = QColor(QStringLiteral("#3a2a20"));
  t.brand_wash = QColor(QStringLiteral("#2e241d"));
  t.brand_wash_text = QColor(QStringLiteral("#e9a97f"));
  t.success = QColor(QStringLiteral("#8fae88"));
  t.warning = QColor(QStringLiteral("#d9a066"));
  t.success_wash = QColor(QStringLiteral("#22301f"));
  t.success_text = QColor(QStringLiteral("#a8cfa4"));
  t.danger = QColor(QStringLiteral("#d9705a"));
  t.disabled_bg = QColor(QStringLiteral("#3a352f"));
  t.bubble_out = QColor(QString::fromUtf8(kBrandOrange));
  t.bubble_out_text = QColor(QStringLiteral("#ffffff"));
  t.bubble_in = QColor(QStringLiteral("#2b2622"));
  t.bubble_in_text = QColor(QStringLiteral("#f2ece5"));
  t.chat_bg = QColor(QStringLiteral("#201c19"));
  t.selection = QColor(QStringLiteral("#3a2a20"));
  t.input_bg = QColor(QStringLiteral("#2b2622"));
  return t;
}

// —— 需求批⑬：默认 20 套主题 ——
// 静态生成表（不走 register_theme 实例注册）：tokens_for/builtin_themes/
// read_persisted_mode 全链静态可达，实例无关、无「选了某套却回落 light」
// 的暗坑；register_theme 机制保留给扩展主题（⑭）。每套在恒定品牌族/
// 语义族之上参数化中性色（色相/饱和/明度）＋字体＋字号＋密度，保证：
// 品牌橙恒同、亮暗文本对比度门槛全过、套间 surface 三元组互异（真实可辨，
// 不是只换底色的换皮同色）。
struct DefaultThemeSpec {
  const char* name;  // 展示名即持久化 id（中文＋亮暗后缀）
  bool light;
  int hue;           // 中性色族色相（0-359）
  double sat;        // 中性色族饱和度
  double surface_l;  // surface 明度（亮套 ~0.94-0.97，暗套 ~0.08-0.10）
  const char* font;
  int font_pt;
  int radius;   // 控件圆角 px
  int pad_sm;   // 控件纵向内边距 px
  int pad_md;   // 控件横向内边距 px
};

const DefaultThemeSpec kDefaultThemes[] = {
    // 亮套 10：暖调（晨雾/暖砂/亚麻/杏仁）× 冷调（素笺/月白）× 绿青（青瓷/
    // 雾杉/岚烟）× 近无彩（淡墨），字体 Sans/Serif/Mono、字号 9-11、
    // 密度紧凑/标准/宽松成套
    {"晨雾·亮", true, 30, 0.22, 0.965, "Sans Serif", 10, 8, 6, 12},
    {"暖砂·亮", true, 26, 0.30, 0.955, "Sans Serif", 11, 10, 8, 14},
    {"青瓷·亮", true, 150, 0.16, 0.960, "Serif", 10, 8, 6, 12},
    {"雾杉·亮", true, 185, 0.18, 0.950, "Sans Serif", 9, 6, 4, 10},
    {"亚麻·亮", true, 42, 0.24, 0.945, "Serif", 11, 10, 8, 14},
    {"素笺·亮", true, 210, 0.12, 0.970, "Sans Serif", 10, 8, 6, 12},
    {"月白·亮", true, 222, 0.14, 0.958, "Serif", 9, 6, 4, 10},
    {"杏仁·亮", true, 36, 0.26, 0.950, "Sans Serif", 11, 10, 8, 14},
    {"淡墨·亮", true, 200, 0.07, 0.940, "Monospace", 9, 4, 4, 10},
    {"岚烟·亮", true, 175, 0.14, 0.952, "Sans Serif", 10, 8, 6, 12},
    // 暗套 10：蓝夜（夜幕/玄青/深海/幽蓝）× 绿黑（墨林/松烟/苔原）×
    // 紫夜 × 无彩炭黑 × 暖曜石
    {"夜幕·暗", false, 220, 0.16, 0.085, "Sans Serif", 10, 8, 6, 12},
    {"玄青·暗", false, 205, 0.22, 0.090, "Sans Serif", 9, 6, 4, 10},
    {"墨林·暗", false, 140, 0.13, 0.088, "Serif", 10, 8, 6, 12},
    {"深海·暗", false, 215, 0.30, 0.095, "Sans Serif", 10, 10, 8, 14},
    {"炭黑·暗", false, 0, 0.00, 0.080, "Monospace", 9, 4, 4, 10},
    {"紫夜·暗", false, 270, 0.16, 0.092, "Sans Serif", 11, 10, 8, 14},
    {"松烟·暗", false, 165, 0.11, 0.082, "Serif", 9, 6, 4, 10},
    {"幽蓝·暗", false, 230, 0.24, 0.086, "Sans Serif", 10, 8, 6, 12},
    {"苔原·暗", false, 105, 0.10, 0.090, "Sans Serif", 9, 6, 4, 10},
    {"曜石·暗", false, 28, 0.08, 0.078, "Monospace", 10, 8, 6, 12},
};

QColor hsl(int hue, double sat, double light) {
  QColor c;
  c.setHslF((((hue % 360) + 360) % 360) / 360.0, sat, light);
  return c;
}

ThemeTokens build_default_theme(const DefaultThemeSpec& spec) {
  ThemeTokens t;
  const int h = spec.hue;
  const double s = spec.sat;
  if (spec.light) {
    t.surface = hsl(h, s, spec.surface_l);
    t.surface_alt = hsl(h, s * 0.9, spec.surface_l - 0.030);
    t.surface_raised = hsl(h, s * 0.4, 0.995);
    t.border = hsl(h, s * 0.7, 0.855);
    t.divider = hsl(h, s * 0.8, 0.905);
    t.text = hsl(h, 0.25, 0.16);
    t.text_muted = hsl(h, 0.09, 0.38);
    // 品牌族与语义族同 light_tokens 恒定值：主色全产品唯一（register_theme
    // 同一口径），语义色按亮底固定即可守住对比度门槛
    t.brand = QColor(QString::fromUtf8(kBrandOrange));
    t.brand_hover = QColor(QStringLiteral("#c9541f"));
    t.brand_text = QColor(QStringLiteral("#a05a26"));
    t.on_brand = QColor(QStringLiteral("#ffffff"));
    t.brand_tint = QColor(QStringLiteral("#f6e3d7"));
    t.brand_wash = QColor(QStringLiteral("#fdeee2"));
    t.brand_wash_text = QColor(QStringLiteral("#8a4a1f"));
    t.success = QColor(QStringLiteral("#6f8f6a"));
    t.warning = QColor(QStringLiteral("#c98a3c"));
    t.success_wash = QColor(QStringLiteral("#eaf3e7"));
    t.success_text = QColor(QStringLiteral("#3f6b3a"));
    t.danger = QColor(QStringLiteral("#c0492f"));
    t.disabled_bg = hsl(h, 0.08, 0.83);
    t.bubble_out = QColor(QString::fromUtf8(kBrandOrange));
    t.bubble_out_text = QColor(QStringLiteral("#ffffff"));
    t.bubble_in = hsl(h, s * 0.7, 0.925);
    t.bubble_in_text = hsl(h, 0.25, 0.16);
    t.chat_bg = hsl(h, s * 0.4, 0.988);
    t.selection = QColor(QStringLiteral("#f6e3d7"));
    t.input_bg = t.surface_raised;
  } else {
    t.surface = hsl(h, s, spec.surface_l);
    t.surface_alt = hsl(h, s * 0.9, spec.surface_l + 0.035);
    t.surface_raised = hsl(h, s * 0.8, spec.surface_l + 0.070);
    t.border = hsl(h, s * 0.6, spec.surface_l + 0.165);
    t.divider = hsl(h, s * 0.6, spec.surface_l + 0.115);
    t.text = hsl(h, 0.18, 0.92);
    t.text_muted = hsl(h, 0.10, 0.66);
    // 品牌族与语义族同 dark_tokens 恒定值
    t.brand = QColor(QString::fromUtf8(kBrandOrange));
    t.brand_hover = QColor(QStringLiteral("#ef7a4a"));
    t.brand_text = QColor(QStringLiteral("#f0a07c"));
    t.on_brand = QColor(QStringLiteral("#ffffff"));
    t.brand_tint = QColor(QStringLiteral("#3a2a20"));
    t.brand_wash = QColor(QStringLiteral("#2e241d"));
    t.brand_wash_text = QColor(QStringLiteral("#e9a97f"));
    t.success = QColor(QStringLiteral("#8fae88"));
    t.warning = QColor(QStringLiteral("#d9a066"));
    t.success_wash = QColor(QStringLiteral("#22301f"));
    t.success_text = QColor(QStringLiteral("#a8cfa4"));
    t.danger = QColor(QStringLiteral("#d9705a"));
    t.disabled_bg = hsl(h, 0.06, spec.surface_l + 0.115);
    t.bubble_out = QColor(QString::fromUtf8(kBrandOrange));
    t.bubble_out_text = QColor(QStringLiteral("#ffffff"));
    t.bubble_in = hsl(h, s * 0.8, spec.surface_l + 0.070);
    t.bubble_in_text = hsl(h, 0.18, 0.92);
    t.chat_bg = hsl(h, s * 0.9, spec.surface_l + 0.020);
    t.selection = QColor(QStringLiteral("#3a2a20"));
    t.input_bg = t.surface_raised;
  }
  t.font_family = QString::fromUtf8(spec.font);
  t.font_pt = spec.font_pt;
  t.radius = spec.radius;
  t.pad_sm = spec.pad_sm;
  t.pad_md = spec.pad_md;
  return t;
}

const DefaultThemeSpec* find_default_spec(const QString& name) {
  // spec.name 是 UTF-8 字面量（中文主题名），必须走 fromUtf8——QLatin1String
  // 会按 Latin-1 解码导致全部失配、tokens_for 静默回落 light
  for (const DefaultThemeSpec& spec : kDefaultThemes) {
    if (name == QString::fromUtf8(spec.name)) return &spec;
  }
  return nullptr;
}

// ⑭ 平台是否支持毛玻璃：仅 Windows 且运行时可调 DWM（编译期 + 运行时双检；
// Linux／macOS／无 DWM 恒 false → 上层降级为不透明背景，不崩不花屏）
bool platform_frosted_supported() {
#if defined(Q_OS_WIN)
  // 运行时探测：真正尝试调用即可知；此处以「Windows 平台 + DWM 可用」为准。
  // 真机启用走 apply_frosted_effect()（user32/dwmapi，见下）；
  // 本机无 Windows，此腿只编译不过运行，待真机验证。
  return true;
#else
  return false;
#endif
}

bool valid(const QColor& c) { return c.isValid() && c.alpha() > 0; }

// WCAG 相对亮度（sRGB 线性化）——用于无 colorScheme 提示时的明暗兜底判断
double linearized(double c) {
  return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& c) {
  return 0.2126 * linearized(c.redF()) + 0.7152 * linearized(c.greenF()) +
         0.0722 * linearized(c.blueF());
}

}  // namespace

QHash<QString, QColor> ThemeTokens::as_map() const {
  return {{QStringLiteral("surface"), surface},
          {QStringLiteral("surface_alt"), surface_alt},
          {QStringLiteral("surface_raised"), surface_raised},
          {QStringLiteral("border"), border},
          {QStringLiteral("divider"), divider},
          {QStringLiteral("text"), text},
          {QStringLiteral("text_muted"), text_muted},
          {QStringLiteral("brand"), brand},
          {QStringLiteral("brand_hover"), brand_hover},
          {QStringLiteral("brand_text"), brand_text},
          {QStringLiteral("on_brand"), on_brand},
          {QStringLiteral("brand_tint"), brand_tint},
          {QStringLiteral("brand_wash"), brand_wash},
          {QStringLiteral("brand_wash_text"), brand_wash_text},
          {QStringLiteral("success"), success},
          {QStringLiteral("warning"), warning},
          {QStringLiteral("success_wash"), success_wash},
          {QStringLiteral("success_text"), success_text},
          {QStringLiteral("danger"), danger},
          {QStringLiteral("disabled_bg"), disabled_bg},
          {QStringLiteral("bubble_out"), bubble_out},
          {QStringLiteral("bubble_out_text"), bubble_out_text},
          {QStringLiteral("bubble_in"), bubble_in},
          {QStringLiteral("bubble_in_text"), bubble_in_text},
          {QStringLiteral("chat_bg"), chat_bg},
          {QStringLiteral("selection"), selection},
          {QStringLiteral("input_bg"), input_bg}};
}

bool ThemeTokens::is_complete() const {
  const QHash<QString, QColor> map = as_map();
  for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
    if (!valid(it.value())) return false;
  }
  return true;
}

bool ThemeTokens::is_typography_valid() const {
  return !font_family.trimmed().isEmpty() && font_pt > 0 && radius > 0 &&
         pad_sm > 0 && pad_md > 0;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
  load_custom_overrides();
  {
    QSettings settings(QCoreApplication::organizationName(),
                       QCoreApplication::applicationName());
    frosted_enabled_ = settings.value(QString::fromUtf8(kFrostedKey), false)
                           .toBool();
    const QString base = settings.value(QString::fromUtf8(kCustomBaseKey))
                             .toString();
    if (builtin_themes().contains(base)) custom_base_ = base;  // 坏值回落 light
  }
  mode_ = read_persisted_mode();
  reload();
}

ThemeManager::~ThemeManager() = default;

ThemeManager& ThemeManager::instance() {
  static ThemeManager manager;
  return manager;
}

QStringList ThemeManager::builtin_themes() {
  QStringList list{QString::fromUtf8(kLight), QString::fromUtf8(kDark)};
  for (const DefaultThemeSpec& spec : kDefaultThemes) {
    list << QString::fromUtf8(spec.name);
  }
  return list;
}

ThemeTokens ThemeManager::tokens_for(const QString& name) {
  if (name == QLatin1String(kDark)) return dark_tokens();
  if (name == QLatin1String(kLight)) return light_tokens();
  // ⑬ 默认主题走静态生成表——与 builtin_themes/modes/has_theme 同一张表，
  // reload() 的 fallback 也经由这里拿到真令牌，不回落 light
  if (const DefaultThemeSpec* spec = find_default_spec(name)) {
    return build_default_theme(*spec);
  }
  return light_tokens();
}

QString ThemeManager::read_persisted_mode() const {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const QString stored =
      settings.value(QString::fromUtf8(kSettingsKey)).toString();
  if (stored.isEmpty()) return QString::fromUtf8(kFollowSystem);
  if (stored == QLatin1String(kFollowSystem)) return stored;
  // 内置 22 套（light/dark＋⑬ 默认 20）静态可解析，直接认
  if (builtin_themes().contains(stored)) return stored;
  // ⑭ 自定义槽位：有覆盖层才认（覆盖层丢了＝换机后只剩槽名，回落基线
  // 内置主题，不把界面停在一个点开即空的自定义态上）
  if (stored == customThemeName()) {
    return overrides_.isEmpty() ? custom_base_ : stored;
  }
  // 自定义主题：注册表里没有这个名字（换机／重装后扩展主题可能已不在）→
  // 回落跟随系统，不让界面停在无法解析的取值上
  const ThemeManager* self = this;
  return self->custom_.contains(stored) ? stored
                                        : QString::fromUtf8(kFollowSystem);
}

void ThemeManager::persist_mode(const QString& mode) const {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  settings.setValue(QString::fromUtf8(kSettingsKey), mode);
}

bool ThemeManager::has_theme(const QString& name) const {
  return builtin_themes().contains(name) || custom_.contains(name) ||
         name == customThemeName();
}

void ThemeManager::register_theme(const QString& name,
                                  const ThemeTokens& tokens) {
  if (name.isEmpty() || name == QLatin1String(kFollowSystem)) return;
  // 品牌橙是全产品唯一主色：扩展主题也不得改主色，否则多主题扩展位会漂移
  ThemeTokens copy = tokens;
  copy.brand = QColor(QString::fromUtf8(kBrandOrange));
  // 排版/密度非法取值兜底回默认（QSS 模板五槽位须恒有值可替换）
  if (copy.font_family.trimmed().isEmpty()) {
    copy.font_family = QStringLiteral("Sans Serif");
  }
  if (copy.font_pt <= 0) copy.font_pt = 10;
  if (copy.radius <= 0) copy.radius = 8;
  if (copy.pad_sm <= 0) copy.pad_sm = 6;
  if (copy.pad_md <= 0) copy.pad_md = 12;
  custom_.insert(name, copy);
}

// —— 需求批⑭：面板改色覆盖层 ——

QStringList ThemeManager::customizable_tokens() { return kCustomizableTokens; }

QHash<QString, QColor> ThemeManager::custom_overrides() const {
  return overrides_;
}

bool ThemeManager::set_custom_override(const QString& token,
                                       const QColor& color) {
  // 品牌橙／语义族令牌锁死：不在放行集内一律拒绝（不落盘、不改状态）
  if (!kCustomizableTokens.contains(token)) return false;
  if (!color.isValid()) return false;
  // 首个改色：锁定当前基线（内置或已注册扩展主题，跟随态取解析名）并切到
  // 自定义槽（改色即时可见，所见即所改）
  if (mode_ != customThemeName()) {
    custom_base_ = resolve_theme();
    QSettings settings(QCoreApplication::organizationName(),
                       QCoreApplication::applicationName());
    settings.setValue(QString::fromUtf8(kCustomBaseKey), custom_base_);
    mode_ = QString::fromUtf8(kCustomTheme);
    persist_mode(mode_);
  }
  overrides_.insert(token, color);
  persist_custom_overrides();
  reload();
  return true;
}

void ThemeManager::clear_custom_overrides() {
  overrides_.clear();
  persist_custom_overrides();
  // 当前正停在自定义槽：回到基线内置主题（「恢复默认」语义）
  if (mode_ == customThemeName()) {
    mode_ = custom_base_;
    persist_mode(mode_);
  }
  reload();
}

bool ThemeManager::has_custom_overrides() const { return !overrides_.isEmpty(); }

ThemeTokens ThemeManager::apply_overrides(
    const ThemeTokens& base, const QHash<QString, QColor>& overrides) {
  ThemeTokens out = base;
  for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
    // 纵深防御：即便覆盖表被外部构造带进品牌／语义令牌，这里也不放行
    if (!kCustomizableTokens.contains(it.key())) continue;
    if (!it.value().isValid()) continue;
    if (it.key() == QLatin1String("surface")) out.surface = it.value();
    else if (it.key() == QLatin1String("surface_alt")) out.surface_alt = it.value();
    else if (it.key() == QLatin1String("surface_raised")) out.surface_raised = it.value();
    else if (it.key() == QLatin1String("border")) out.border = it.value();
    else if (it.key() == QLatin1String("text")) out.text = it.value();
    else if (it.key() == QLatin1String("text_muted")) out.text_muted = it.value();
    else if (it.key() == QLatin1String("chat_bg")) out.chat_bg = it.value();
    else if (it.key() == QLatin1String("input_bg")) out.input_bg = it.value();
  }
  // 品牌橙恒同（覆盖层亦不得漂移主色）
  out.brand = base.brand;
  return out;
}

void ThemeManager::load_custom_overrides() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  settings.beginGroup(QString::fromUtf8(kOverridesGroup));
  for (const QString& name : settings.childKeys()) {
    if (!kCustomizableTokens.contains(name)) continue;  // 旧版本残留／坏键丢弃
    const QColor c(settings.value(name).toString());
    if (c.isValid()) overrides_.insert(name, c);
  }
  settings.endGroup();
}

void ThemeManager::persist_custom_overrides() const {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  settings.remove(QString::fromUtf8(kOverridesGroup));  // 先清组再写：删覆盖即时落盘
  settings.beginGroup(QString::fromUtf8(kOverridesGroup));
  for (auto it = overrides_.constBegin(); it != overrides_.constEnd(); ++it) {
    settings.setValue(it.key(), it.value().name(QColor::HexArgb));
  }
  settings.endGroup();
}

// —— 需求批⑭：毛玻璃特效（全局单开关，落 appearance/effects）——

bool ThemeManager::frosted_effect_supported() {
  return platform_frosted_supported();
}

bool ThemeManager::frosted_effect_active() const {
  const bool supported =
      frosted_probe_ ? frosted_probe_() : frosted_effect_supported();
  return frosted_enabled_ && supported;
}

bool ThemeManager::set_frosted_effect_enabled(bool enabled) {
  frosted_enabled_ = enabled;
  persist_frosted(enabled);
  // 平台支持的判定用注入探针优先（测试缝），否则平台能力。
  // 不支持时开关本身仍可置位（切到支持平台即生效），但生效态恒 false；
  // 返回值＝当前是否真生效（调用方据此提示「当前系统不支持」）
  const bool supported =
      frosted_probe_ ? frosted_probe_() : frosted_effect_supported();
  if (applied_) push_to_app();  // 重新应用窗口特效（含降级路径）
  return enabled && supported;
}

void ThemeManager::set_frosted_support_probe(std::function<bool()> probe) {
  frosted_probe_ = std::move(probe);
  if (applied_) push_to_app();
}

void ThemeManager::persist_frosted(bool enabled) const {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  settings.setValue(QString::fromUtf8(kFrostedKey), enabled);
}

QStringList ThemeManager::modes() const {
  QStringList list{QString::fromUtf8(kFollowSystem)};
  list += builtin_themes();
  // ⑭ 自定义槽位：列表恒列出（无覆盖时点选先落基线，改色即时建层）
  list << QString::fromUtf8(kCustomTheme);
  for (auto it = custom_.constBegin(); it != custom_.constEnd(); ++it) {
    list << it.key();
  }
  return list;
}

void ThemeManager::set_mode(const QString& mode) {
  QString next = mode;
  if (next != QLatin1String(kFollowSystem) && !has_theme(next)) {
    next = QString::fromUtf8(kFollowSystem);
  }
  if (next == mode_) return;
  mode_ = next;
  persist_mode(mode_);
  reload();
}

QString ThemeManager::resolve_theme() const {
  if (mode_ != QLatin1String(kFollowSystem)) return mode_;
  return system_is_dark() ? QString::fromUtf8(kDark) : QString::fromUtf8(kLight);
}

QString ThemeManager::effective_theme() const { return resolve_theme(); }

bool ThemeManager::system_is_dark() const {
  if (dark_probe_) return dark_probe_();
  if (auto* hints = QGuiApplication::styleHints()) {
    if (hints->colorScheme() != Qt::ColorScheme::Unknown) {
      return hints->colorScheme() == Qt::ColorScheme::Dark;
    }
  }
  // 老平台／无头环境没有 colorScheme 提示：退回当前调色板自身的明暗关系
  // （窗口底比文本暗即暗色主题），不依赖固定阈值
  const QPalette palette = QGuiApplication::palette();
  return luminance(palette.color(QPalette::Active, QPalette::Window)) <
         luminance(palette.color(QPalette::Active, QPalette::WindowText));
}

void ThemeManager::set_system_dark_probe(std::function<bool()> probe) {
  dark_probe_ = std::move(probe);
  reload();
}

void ThemeManager::reload() {
  const QString name = resolve_theme();
  // ⑭ 自定义槽位：基线＝首个改色时锁定的主题（内置或已注册扩展；不递归回
  // kCustomTheme），再叠加用户覆盖层；apply_overrides 内再锁品牌／语义令牌
  ThemeTokens next;
  if (name == customThemeName()) {
    const ThemeTokens base = custom_.contains(custom_base_)
                                 ? custom_.value(custom_base_)
                                 : tokens_for(custom_base_);
    next = apply_overrides(base, overrides_);
  } else if (custom_.contains(name)) {
    next = custom_.value(name);
  } else {
    next = tokens_for(name);
  }
  tokens_ = next;
  if (applied_) push_to_app();
}

QString ThemeManager::stylesheet_for(const ThemeTokens& tokens) {
  // 模板里只写 %令牌名%，末尾统一替换——新增令牌自动进 QSS，
  // 不会因为漏改样式而悄悄留硬编码色。⑬ 增字体/密度槽位
  // （%font_family%/%font_pt%/%radius%/%pad_sm%/%pad_md%），色表外单独替换。
  QString qss = QStringLiteral(
      "QWidget { color: %text%; font-family: \"%font_family%\"; "
      "font-size: %font_pt%pt; }\n"
      "QMainWindow, QDialog { background: %surface%; }\n"
      "QTextBrowser, QPlainTextEdit, QTextEdit { background: %chat_bg%; "
      "color: %text%; border: 1px solid %border%; border-radius: %radius%px; "
      "}\n"
      "QListWidget, QTreeWidget, QTableWidget { background: %surface_alt%; "
      "color: %text%; border: 1px solid %border%; border-radius: %radius%px; "
      "}\n"
      "QListWidget::item:selected, QTreeWidget::item:selected { "
      "background: %selection%; color: %text%; }\n"
      "QLineEdit, QSpinBox, QComboBox { background: %input_bg%; "
      "color: %text%; border: 1px solid %border%; border-radius: %radius%px; "
      "padding: %pad_sm%px %pad_md%px; }\n"
      "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { "
      "border: 1px solid %brand%; }\n"
      "QComboBox QAbstractItemView { background: %surface_raised%; "
      "color: %text%; selection-background-color: %brand%; "
      "selection-color: %on_brand%; border: 1px solid %border%; }\n"
      "QPushButton { background: %surface_raised%; color: %text%; "
      "border: 1px solid %border%; border-radius: %radius%px; "
      "padding: %pad_sm%px %pad_md%px; }\n"
      "QPushButton:hover { border: 1px solid %brand_hover%; "
      "color: %brand_hover%; }\n"
      "QPushButton:pressed { background: %brand_tint%; }\n"
      "QPushButton:disabled { background: %disabled_bg%; color: %text_muted%; "
      "border-color: %divider%; }\n"
      "QToolButton { color: %text%; }\n"
      "QToolButton:hover { color: %brand_hover%; }\n"
      "QHeaderView::section { background: %surface_alt%; color: %text_muted%; "
      "border: none; border-bottom: 1px solid %border%; "
      "padding: %pad_sm%px %pad_md%px; }\n"
      "QToolTip { background: %surface_raised%; color: %text%; "
      "border: 1px solid %border%; }\n"
      "QMenu { background: %surface_raised%; color: %text%; "
      "border: 1px solid %border%; }\n"
      "QMenu::item:selected { background: %selection%; color: %text%; }\n"
      "QGroupBox { border: 1px solid %border%; border-radius: %radius%px; "
      "margin-top: 12px; }\n"
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; "
      "padding: 0 4px; color: %text_muted%; }\n"
      "QTabBar::tab { background: %surface_alt%; color: %text_muted%; "
      "padding: %pad_sm%px %pad_md%px; border: 1px solid %border%; }\n"
      "QTabBar::tab:selected { background: %surface_raised%; color: %text%; "
      "border-bottom: 1px solid %brand%; }\n"
      "QTabWidget::pane { border: 1px solid %border%; }\n"
      "QCheckBox, QRadioButton { color: %text%; spacing: 6px; }\n"
      "QCheckBox::indicator, QRadioButton::indicator { "
      "border: 1px solid %border%; background: %input_bg%; }\n"
      "QSplitter::handle { background: %divider%; }\n"
      "QScrollBar:vertical, QScrollBar:horizontal { background: transparent; "
      "margin: 0; }\n"
      "QScrollBar::handle { background: %divider%; border-radius: 4px; "
      "min-height: 24px; min-width: 24px; }\n"
      "QScrollBar::handle:hover { background: %brand_tint%; }\n"
      "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }\n"
      "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; "
      "}\n"
      "QStatusBar { background: %surface_alt%; color: %text_muted%; }\n"
      "QProgressBar { background: %surface_alt%; color: %text%; "
      "border: 1px solid %border%; border-radius: 6px; text-align: center; }\n"
      "QProgressBar::chunk { background: %brand%; }");

  const auto map = tokens.as_map();
  for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
    qss.replace(QStringLiteral("%") + it.key() + QStringLiteral("%"),
                it.value().name());
  }
  // ⑬ 排版/密度槽位（as_map 只管颜色）
  qss.replace(QStringLiteral("%font_family%"), tokens.font_family);
  qss.replace(QStringLiteral("%font_pt%"), QString::number(tokens.font_pt));
  qss.replace(QStringLiteral("%radius%"), QString::number(tokens.radius));
  qss.replace(QStringLiteral("%pad_sm%"), QString::number(tokens.pad_sm));
  qss.replace(QStringLiteral("%pad_md%"), QString::number(tokens.pad_md));
  return qss;
}

void ThemeManager::push_to_app() {
  if (!app_) return;
  QPalette palette = app_->palette();
  palette.setColor(QPalette::Window, tokens_.surface);
  palette.setColor(QPalette::WindowText, tokens_.text);
  palette.setColor(QPalette::Base, tokens_.surface_raised);
  palette.setColor(QPalette::AlternateBase, tokens_.surface_alt);
  palette.setColor(QPalette::Text, tokens_.text);
  palette.setColor(QPalette::Button, tokens_.surface_raised);
  palette.setColor(QPalette::ButtonText, tokens_.text);
  palette.setColor(QPalette::Highlight, tokens_.brand);
  palette.setColor(QPalette::HighlightedText, tokens_.on_brand);
  palette.setColor(QPalette::ToolTipBase, tokens_.surface_raised);
  palette.setColor(QPalette::ToolTipText, tokens_.text);
  palette.setColor(QPalette::Link, tokens_.brand);
  palette.setColor(QPalette::PlaceholderText, tokens_.text_muted);
  app_->setPalette(palette);
  app_->setStyleSheet(stylesheet_for(tokens_));
  apply_frosted_effect();
  emit theme_changed(effective_theme());
}

// ⑭ 毛玻璃特效落地：仅在生效态（开关开 且 平台支持）时对顶层窗口启用
// 亚克力／Mica；其余一律不透明背景（降级路径）。Windows 腿只写代码＋编译，
// 本机（Linux）走 #else 空实现，实测降级＝不崩不花屏。
void ThemeManager::apply_frosted_effect() {
  if (!app_) return;
  const bool want = frosted_effect_active();
  const auto windows = app_->topLevelWidgets();
  for (QWidget* w : windows) {
    QWindow* handle = w->windowHandle();
    if (!handle) continue;  // 未创建原生窗口（离屏／未 show）→ 跳过，勿强建
#if defined(Q_OS_WIN)
    apply_frosted_effect_to_window_impl(handle, want);
#else
    // 非 Windows：系统不支持毛玻璃，恒保持不透明（降级为默认背景色）
    Q_UNUSED(want);
#endif
  }
}

void ThemeManager::apply(QApplication* app) {
  app_ = app;
  applied_ = true;
  if (!watching_system_) {
    watching_system_ = true;
    // 系统亮暗变化：只在跟随模式下重应用，手动选择不被系统变化覆盖
    if (auto* hints = QGuiApplication::styleHints()) {
      QObject::connect(hints, &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == QLatin1String(kFollowSystem)) reload();
      });
    }
  }
  push_to_app();
}

}  // namespace memex::client