// 主题令牌与切换（R19 · T4.9）：亮／暗双主题，跟随系统＋手动切换，选择
// 持久化。界面颜色只从这里取令牌（不再散落十六进制字面量），新增主题＝
// 新增一套令牌，不改切换逻辑。
#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QApplication;

namespace memex::client {

// 一套主题的全部颜色令牌。字段名即令牌名——QSS 生成与代码取值共用这一份
// 清单（as_map），因此加令牌只需改这一处。
struct ThemeTokens {
  QColor surface;         // 窗口底
  QColor surface_alt;     // 侧栏／列表底
  QColor surface_raised;  // 卡片／输入框底
  QColor border;          // 常规描边
  QColor divider;         // 细分隔线
  QColor text;            // 主文本
  QColor text_muted;      // 次要文本（时间、说明）
  QColor brand;           // 品牌橙（R19 主色；两主题恒同）
  QColor brand_hover;
  QColor brand_text;      // 浅底上可读的品牌色文本（按钮文字、徽标）
  QColor on_brand;        // 品牌底上的文本
  QColor brand_tint;      // 选中行底
  QColor brand_wash;      // 提示条底
  QColor brand_wash_text; // 提示条文本
  QColor success;
  QColor warning;
  QColor success_wash;    // 成功提示条底（协作态横幅）
  QColor success_text;    // 成功提示条文本
  QColor danger;
  QColor disabled_bg;     // 禁用控件底
  QColor bubble_out;
  QColor bubble_out_text;
  QColor bubble_in;
  QColor bubble_in_text;
  QColor chat_bg;
  QColor selection;
  QColor input_bg;

  // —— 排版与密度（需求批⑬：字体/密度成套）——颜色令牌之外的第二组
  // 维度，QSS 模板以 %font_family%/%font_pt%/%radius%/%pad_sm%/%pad_md%
  // 引用（as_map 只管颜色，此处单独替换）
  QString font_family = QStringLiteral("Sans Serif");
  int font_pt = 10;   // 基准字号
  int radius = 8;     // 控件圆角 px
  int pad_sm = 6;     // 控件纵向内边距 px
  int pad_md = 12;    // 控件横向内边距 px

  QHash<QString, QColor> as_map() const;
  // 全部颜色令牌有效（alpha>0）——缺色即实现失误，测试守住
  bool is_complete() const;
  // 排版与密度取值合法（字体非空、数值为正）——⑬ 批量生成面测试守住
  bool is_typography_valid() const;
};

class ThemeManager : public QObject {
  Q_OBJECT
 public:
  // 手动选择的取值：跟随系统／亮／暗，或 register_theme 注册的自定义主题名
  static constexpr const char* kFollowSystem = "system";
  static constexpr const char* kLight = "light";
  static constexpr const char* kDark = "dark";
  // 需求批⑭：面板改色的单一固定槽位名（不按内置主题逐个派生；皮肤包⑮另走体系）。
  // 选到本槽即「自定义主题」，其令牌＝当前内置基线叠加用户覆盖色。
  static constexpr const char* kCustomTheme = "自定义";
  // 比较用正确解码名：kCustomTheme 是 UTF-8 中文字面量，QLatin1String 会按
  // Latin-1 解码导致全部失配（⑬ 默认主题名曾踩同坑）——一律用本函数比较
  static QString customThemeName() { return QString::fromUtf8(kCustomTheme); }

  explicit ThemeManager(QObject* parent = nullptr);
  ~ThemeManager() override;

  // 应用级单例（main() 里 apply 一次即可）
  static ThemeManager& instance();

  // 当前手动选择（持久化在 appearance/theme_mode）
  QString mode() const { return mode_; }
  // 设置手动选择并落盘；已 apply 过则立即生效。未知主题名回落跟随系统。
  void set_mode(const QString& mode);
  // 可选项：跟随系统＋全部已注册主题（新增主题扩展位的唯一入口）
  QStringList modes() const;
  bool has_theme(const QString& name) const;
  // 注册自定义主题：只提供一套令牌，切换／持久化／QSS 全自动可用
  void register_theme(const QString& name, const ThemeTokens& tokens);

  // —— 需求批⑭：面板改色（中性色覆盖层，落 appearance/custom_colors）——
  // 可改色的中性令牌（品牌橙 #e16531 与 success/warning/danger 语义族锁死
  // 不放行）——设置页据此建控件、测试据此断言放行面
  static QStringList customizable_tokens();
  // 用户改色覆盖表（令牌名→色值）；空表＝无覆盖（走内置基线）
  QHash<QString, QColor> custom_overrides() const;
  // 自定义槽位的基线主题名（首个改色时锁定；「恢复默认」回落它）
  QString custom_theme_base() const { return custom_base_; }
  // 设/删单枚覆盖；品牌／语义令牌名一律拒绝（返回 false 不落盘）
  bool set_custom_override(const QString& token, const QColor& color);
  // 清空全部覆盖并落盘（「恢复默认」：回到当前内置主题基线）
  void clear_custom_overrides();
  // 覆盖层是否非空（设置页据此显示选中态与「恢复默认」可用性）
  bool has_custom_overrides() const;
  // 覆盖层应用到一套基线上（品牌／语义令牌恒不被覆盖——纵深防御）
  static ThemeTokens apply_overrides(const ThemeTokens& base,
                                     const QHash<QString, QColor>& overrides);

  // —— 需求批⑭：毛玻璃特效（全局单开关，默认关）——
  // 落 appearance/effects；不支持平台（Linux／无 DWM）自动降级为不透明背景
  bool frosted_effect_enabled() const { return frosted_enabled_; }
  // 设开关并落盘；已 apply 过则立即生效。返回是否真正启用（不支持平台返回
  // false 且界面维持不透明——调用方据此提示「当前系统不支持」）
  bool set_frosted_effect_enabled(bool enabled);
  // 当前平台是否支持毛玻璃（Windows 且运行时可调 DWM；其余恒 false）
  static bool frosted_effect_supported();
  // 测试缝：替换「平台是否支持」探测（默认走 frosted_effect_supported()）
  void set_frosted_support_probe(std::function<bool()> probe);
  // 平台生效态：开关开 且 平台支持 → true；否则 false（降级）
  bool frosted_effect_active() const;

  // 生效主题名：跟随系统时按系统亮暗解析为 light／dark
  QString effective_theme() const;
  bool system_is_dark() const;
  // 测试缝：替换系统亮暗探测（默认走 QStyleHints::colorScheme）
  void set_system_dark_probe(std::function<bool()> probe);

  const ThemeTokens& tokens() const { return tokens_; }
  static ThemeTokens tokens_for(const QString& name);
  static QStringList builtin_themes();

  // 应用到应用级（调色板＋全局 QSS）；系统亮暗变化时跟随模式下自动重应用
  void apply(QApplication* app);
  bool applied() const { return applied_; }
  // 由当前令牌生成的全局 QSS（测试与设置页预览共用）
  static QString stylesheet_for(const ThemeTokens& tokens);

 signals:
  void theme_changed(const QString& theme_name);

 private:
  QString resolve_theme() const;
  void reload();
  void push_to_app();
  // ⑭ 毛玻璃特效落地（push_to_app 内调；不支持平台走降级空实现）
  void apply_frosted_effect();
  QString read_persisted_mode() const;
  void persist_mode(const QString& mode) const;
  // ⑭ 从 QSettings 读覆盖层（appearance/custom_colors 组）；坏值丢弃
  void load_custom_overrides();
  // ⑭ 覆盖层落盘（group 重建：先清组再写，保证删覆盖即时落盘）
  void persist_custom_overrides() const;
  void persist_frosted(bool enabled) const;

  QString mode_ = QString::fromUtf8(kFollowSystem);
  ThemeTokens tokens_;
  QHash<QString, ThemeTokens> custom_;
  QHash<QString, QColor> overrides_;  // ⑭ 面板改色覆盖层（落 appearance/custom_colors）
  // ⑭ 自定义槽位的内置基线（首个改色时的当前内置主题；「恢复默认」回落它）
  QString custom_base_ = QString::fromUtf8(kLight);
  bool frosted_enabled_ = false;      // ⑭ 毛玻璃开关（落 appearance/effects）
  std::function<bool()> dark_probe_;
  std::function<bool()> frosted_probe_;  // ⑭ 测试缝：平台支持探测
  QApplication* app_ = nullptr;
  bool applied_ = false;
  bool watching_system_ = false;
};

}  // namespace memex::client