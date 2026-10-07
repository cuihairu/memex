// R25-2 群 CI/CD 工具窗口：流水线红绿灯列表＋触发构建（谁触发可回溯）
// ＋run 历史＋流水线管理（增删=群主/管理员，服务端裁决）。独立文件面
// 会话（与备忘录/密码箱同构）；触发走 R25-1 工具白名单闸（未开放由
// 服务端 403，状态行明示）。stub 执行器即时出终态（真 CI 接入随 R25-4）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace memex::client {

class FilesClient;

class GroupCiDialog : public QDialog {
  Q_OBJECT
 public:
  explicit GroupCiDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 切目标群（懒建复用同一窗口时换群：清列表重拉）
  void set_group(quint64 gid, const QString& group_name);

  // 拉红绿灯列表＋run 历史（历史按选中流水线过滤，无选中=全部）
  void refresh();
  // 触发选中流水线（params 取参数框现值；解析失败或未选中=false）
  bool trigger_selected();
  // 程序化流水线管理（测试共用；remove=true 删）
  bool submit_pipeline(const QString& name, const QString& description,
                       bool remove = false);
  // 开放成员触发（R25-1 白名单面：tool=ci 动作 trigger；仅群主/管理员，
  // 越权由服务端 403 状态行明示）
  bool open_trigger_whitelist();

  // —— R25-4 凭据面（工具固定 ci；值只此一次发往服务端，界面永只显示
  // 掩码状态。程序化入口不弹确认框——确认框只挂按钮路径，测试走这里）——
  bool set_credential(const QString& value);
  bool delete_credential();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int pipeline_count() const;
  QListWidget* pipeline_list() const { return pipelines_; }
  int runs_count() const;
  QListWidget* runs_list() const { return runs_; }
  QString credential_state_text() const;

 private:
  void build_ui();
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  quint64 gid_{0};
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QLineEdit* params_;
  QLineEdit* pl_name_;
  QLineEdit* pl_desc_;
  QLineEdit* cred_value_;
  QLabel* cred_state_;
  QLabel* status_;
  QListWidget* pipelines_;
  QListWidget* runs_;
  QPushButton* btn_connect_;
  QPushButton* btn_refresh_;
  QPushButton* btn_trigger_;
  QPushButton* btn_add_;
  QPushButton* btn_delete_;
  QPushButton* btn_open_;
  QPushButton* btn_cred_set_;
  QPushButton* btn_cred_del_;
};

} // namespace memex::client
