// 主题设置页（R19 · T4.9；需求批⑬扩默认 20 套）：跟随系统＋内置 22 套
// （＋已注册扩展主题）列表点选，即时生效并落盘；主窗只需挂一个入口打开
// 本页，切换逻辑全在 ThemeManager。⑬ 起 23+ 项改 QListWidget 列表。
#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QListWidget;

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

 private:
  void sync_from_manager();

  ThemeManager* manager_;
  QListWidget* list_ = nullptr;
  QLabel* effective_label_ = nullptr;
  QWidget* swatch_ = nullptr;
};

}  // namespace memex::client
