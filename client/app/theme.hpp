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
  QColor on_brand;        // 品牌底上的文本
  QColor brand_tint;      // 选中行底
  QColor brand_wash;      // 提示条底
  QColor brand_wash_text; // 提示条文本
  QColor success;
  QColor warning;
  QColor danger;
  QColor bubble_out;
  QColor bubble_out_text;
  QColor bubble_in;
  QColor bubble_in_text;
  QColor chat_bg;
  QColor selection;
  QColor input_bg;

  QHash<QString, QColor> as_map() const;
  // 全部令牌有效（alpha>0）——缺色即实现失误，测试守住
  bool is_complete() const;
};

class ThemeManager : public QObject {
  Q_OBJECT
 public:
  // 手动选择的取值：跟随系统／亮／暗，或 register_theme 注册的自定义主题名
  static constexpr const char* kFollowSystem = "system";
  static constexpr const char* kLight = "light";
  static constexpr const char* kDark = "dark";

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
  QString read_persisted_mode() const;
  void persist_mode(const QString& mode) const;

  QString mode_ = QString::fromUtf8(kFollowSystem);
  ThemeTokens tokens_;
  QHash<QString, ThemeTokens> custom_;
  std::function<bool()> dark_probe_;
  QApplication* app_ = nullptr;
  bool applied_ = false;
  bool watching_system_ = false;
};

}  // namespace memex::client