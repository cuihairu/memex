// 主题设置页（R19 · T4.9；⑬ 默认 20 套；⑭ 面板改色＋毛玻璃开关）：
// 跟随系统＋内置 22＋自定义槽（＋已注册扩展主题）列表点选，即时生效并
// 落盘。⑭ 在列表下加面板改色（8 枚中性令牌 QColorDialog 改色→自定义槽；
// 品牌／语义色不放行）与毛玻璃特效开关（默认关，平台不支持自动降级）。
#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace memex::client {

class ThemeManager;

class ThemeSettingsPage : public QWidget {
  Q_OBJECT
 public:
  // manager 为空时用应用级单例；测试可注入独立实例（隔离持久化）
  explicit ThemeSettingsPage(ThemeManager* manager = nullptr,
                             QWidget* parent = nullptr);

  // 当前生效主题名（跟随系统时已解析为 light／dark）
  QString effective_theme() const;
  // 按模式名选中（供测试与代码接线；与手动点选同路径，立即生效并落盘）
  bool select_mode(const QString& mode);
  // ⑭ 改色（与 QColorDialog 选定后「确定」同一路径；供测试程序化驱动）。
  // 品牌／语义令牌名由 ThemeManager 拒绝并返回 false
  bool apply_custom_color(const QString& token, const QColor& color);
  // ⑭ 毛玻璃开关（与复选框 toggled 同一路径；返回是否真生效——不支持
  // 平台返回 false 即降级提示）
  bool set_frosted(bool enabled);

 private:
  void sync_from_manager();

  ThemeManager* manager_;
  QListWidget* list_ = nullptr;
  QLabel* effective_label_ = nullptr;
  QWidget* swatch_ = nullptr;
  // ⑭ 面板改色（令牌名→色钮）与「恢复默认」
  QHash<QString, QPushButton*> color_buttons_;
  QPushButton* reset_colors_ = nullptr;
  // ⑭ 毛玻璃开关与降级提示
  QCheckBox* frosted_check_ = nullptr;
  QLabel* frosted_hint_ = nullptr;
};

}  // namespace memex::client
