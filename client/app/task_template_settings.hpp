// R27-2 自定义模板设置页：增删 URL 模板 provider（{name}/{id}/{模板}，
// 模板须含 {key} 槽、{id} 避内置预设）。模板非凭据，经
// custom_templates()/save_custom_templates() 明文 QSettings 持久化；
// 保存后 TaskDialog 调 registry.reload_custom() 即重挂。org/app 可注入
//（缺省=真配置面；测试独立命名隔离——QSettings setPath 重定向不了
// 默认构造，显式传参才不污染真配置，同 TaskProviderStore 口径）。
#pragma once

#include <QDialog>

#include <QString>
#include <QVector>

#include "engine/task/task_provider.hpp"

class QLineEdit;
class QLabel;
class QListWidget;
class QPushButton;

namespace memex::client {

class TaskTemplateSettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit TaskTemplateSettingsDialog(
      QWidget* parent = nullptr,
      QString org = QStringLiteral("memex"),
      QString app = QStringLiteral("task-providers"));

  // 程序化入口（测试共用；按钮同径）：增/删自定义模板（校验同按钮）
  bool add_template(const QString& name, const QString& id,
                    const QString& url_template);
  bool remove_template(const QString& id);

  QString status_text() const;
  int template_count() const; // 走查/测试观察点：列表条数

 private:
  void build_ui();
  void reload_list();
  void set_status(const QString& text, bool error = false);

  QListWidget* list_;
  QLineEdit* name_;
  QLineEdit* id_;
  QLineEdit* template_in_;
  QPushButton* btn_add_;
  QPushButton* btn_delete_;
  QLabel* status_;
  QString org_;
  QString app_;
};

} // namespace memex::client
