// R27-3 外部任务 provider 设置面：凭据录入（GitHub PAT／钉钉
// appKey+appSecret+unionId／飞书 app_id+app_secret）＋口令门（首建/解锁/
// 换口令）。凭据经 TaskProviderStore 加密落盘，界面只存会话内存；
// 配置变更经 store.changed() 广播（TaskDialog 拉取侧联动重建）。
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QVBoxLayout>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace memex::client {

class TaskProviderStore;

class TaskProviderSettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit TaskProviderSettingsDialog(TaskProviderStore* store,
                                      QWidget* parent = nullptr);

  // 程序化入口（测试共用；解锁按钮同径）：无落盘包=首建、有=解锁
  bool unlock_with(const QString& passphrase);
  // 程序化入口：保存当前 provider 字段（必填校验后经 store 加密落盘）
  bool save_current_provider(const QJsonObject& fields);
  // 程序化入口：解锁态换口令（新盐重包裹）
  bool change_passphrase(const QString& next);
  // 切到指定 provider 并回填其已存字段（未存=空）
  void show_provider(const QString& provider_id);

  // —— 走查/测试观察点 ——
  QString status_text() const;
  QString current_provider_id() const;
  // 当前界面字段（UI 回填核验用；未解锁=空）
  QJsonObject current_fields() const;

 private:
  void build_ui();
  void build_fields(const QString& provider_id); // 重建动态字段区并回填
  void refresh_gate();                           // 口令门按钮态
  void set_status(const QString& text, bool error = false);
  // 三个真 provider 的必填/密文形态字段（填写校验与落盘共用）
  struct FieldSpec {
    QString key;
    QString label;
    bool secret;
    bool required;
  };
  static QVector<FieldSpec> field_specs(const QString& provider_id);
  static QString provider_name(const QString& provider_id);

  TaskProviderStore* store_;
  QVBoxLayout* fields_holder_{nullptr}; // 动态字段区容器
  QLineEdit* passphrase_;
  QPushButton* btn_unlock_;
  QPushButton* btn_change_pass_;
  QComboBox* provider_;
  QLabel* provider_hint_;
  QVector<QLineEdit*> field_edits_; // 与 field_specs(current) 对齐
  QPushButton* btn_save_;
  QPushButton* btn_clear_;
  QLabel* status_;
};

} // namespace memex::client
