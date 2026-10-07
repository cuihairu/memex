// R25-3 打包工具＋配置导出窗口：产物台账（一键出产物=白名单闸动作）
// ＋删除（恒归群主/管理员）＋群配置快照导出（管理面；密文面永不进导出，
// 只见存在性与授权名单）。独立文件面会话（与备忘录/密码箱/CI 同构）。
#pragma once

#include <QDialog>
#include <QString>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace memex::client {

class FilesClient;

class GroupPackDialog : public QDialog {
  Q_OBJECT
 public:
  explicit GroupPackDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 切目标群（懒建复用同一窗口时换群：清列表重拉）
  void set_group(quint64 gid, const QString& group_name);

  void refresh();
  // 一键打包（程序化入口；名/版本空=false；note 可选）
  bool build_artifact(const QString& name, const QString& version,
                      const QString& note = QString());
  // 删选中产物
  bool delete_selected();
  // 开放成员打包（R25-1 白名单面：tool=pack 动作 build）
  bool open_build_whitelist();
  // 拉群配置快照（管理面；密文不进快照）
  void export_config();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int artifact_count() const;
  QListWidget* artifact_list() const { return artifacts_; }
  QString snapshot_text() const;

 private:
  void build_ui();
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  quint64 gid_{0};
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QLineEdit* name_;
  QLineEdit* version_;
  QLineEdit* note_;
  QLabel* status_;
  QListWidget* artifacts_;
  QPlainTextEdit* snapshot_;
  QPushButton* btn_connect_;
  QPushButton* btn_refresh_;
  QPushButton* btn_build_;
  QPushButton* btn_delete_;
  QPushButton* btn_open_;
  QPushButton* btn_export_;
};

} // namespace memex::client
