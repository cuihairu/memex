// R27-1 个人任务清单窗口：自建/完成/提醒 + 他人分配（服务端按权限模型
// 「同群/同部门才能互派」判权）。独立文件面会话（与 R23-3 文件助手同构：
// 协作面同源账号、文件面独立端口）。提醒=到期待办经通知中心强提醒，
// 回执落服务端只提醒一次；开窗期间 30s 自动轮询。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace memex::client {

class FilesClient;

class TaskDialog : public QDialog {
  Q_OBJECT
 public:
  explicit TaskDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 分配目标候选（组织架构账号；开窗前由主窗喂入）
  void set_assignees(const QStringList& accounts);

  // 程序化入口（测试共用）：标题为空不发；assignee 空=自建、
  // due_ms<=0=不设提醒
  bool add_task(const QString& title, const QString& note, qint64 due_ms,
                const QString& assignee);
  // 选中条目勾完成/回退（勾选框双击同效）
  bool toggle_selected_done();
  // 撤回选中条目（清单主人或分配人，服务端裁决）
  bool delete_selected();
  // 拉列表（我的清单＋我派出的）
  void refresh();
  // 到期待办检查：未完成、已到期、未提醒过 → 通知中心强提醒＋服务端回
  // 执（只提醒一次）。轮询定时器与测试共用同一入口。
  void check_due();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int task_count() const;      // 列表总条数（我的清单＋我派出的）
  QListWidget* list() const { return list_; }
  // 选中条目 id（未选中=-1）
  qint64 selected_id() const;

 private:
  void build_ui();
  void populate(const QJsonArray& mine, const QJsonArray& assigned);
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  QString account_;
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_box_;
  QLineEdit* password_;
  QLabel* status_;
  QListWidget* list_;
  QLineEdit* title_;
  QLineEdit* note_;
  QDateTimeEdit* due_;
  QComboBox* assignee_;
  QPushButton* btn_connect_;
  QPushButton* btn_add_;
  QPushButton* btn_toggle_;
  QPushButton* btn_delete_;
  QPushButton* btn_refresh_;
};

} // namespace memex::client
