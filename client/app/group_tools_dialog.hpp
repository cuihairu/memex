// 二期·群工具三件窗口（投票/接龙/群任务）：原生群互动面（不走 R25
// 外部工具代理）。判权与身份约束全在服务端（file:read 群继承、发起人
// 或群主/管理员、任务完成=负责人/创建者/群主/管理员）——客户端只提交
// 与展示。独立文件面会话（与审批/日报窗口同构）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QCheckBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;

namespace memex::client {

class FilesClient;

class GroupToolsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit GroupToolsDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口；群号为三页共享）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password,
                  quint64 gid);
  bool is_connected() const;

  // —— 程序化入口（测试共用；服务端裁决一切）——
  // 投票：建（选项≥2 由行内逗号分隔给出）/投改票/关票
  bool add_poll(const QString& topic, const QStringList& options,
                qint64 deadline_ms = 0, bool anonymous = false,
                bool multi = false);
  bool vote_selected(int choice);
  // 「1,3」→位集（多选输入）；非法（非数字/越界/重复）返回 0
  int parse_choices(const QString& text) const;
  bool close_selected_poll();
  // 接龙：建/加入（upsert 自己条目）/关
  bool add_chain(const QString& title, const QString& format_hint);
  bool join_selected(const QString& content);
  bool close_selected_chain();
  // 群任务：建（assignee 可空=待认领）/认领/完成
  bool add_task(const QString& title, const QString& assignee);
  bool claim_selected();
  bool done_selected();
  // 拉三页列表
  void refresh();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  QListWidget* poll_list() const { return poll_list_; }
  QListWidget* chain_list() const { return chain_list_; }
  QListWidget* task_list() const { return task_list_; }

 private:
  void build_ui();
  void populate_polls(const QJsonArray& polls);
  void populate_chains(const QJsonArray& chains);
  void populate_tasks(const QJsonArray& tasks);
  void set_status(const QString& text, bool error = false);
  // 当前页选中行 id（未选中=-1）；widget=目标列表
  qint64 selected_id(QListWidget* list) const;
  bool require_connected();

  FilesClient* client_;
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_box_;
  QLineEdit* password_;
  QLineEdit* gid_box_;
  QLabel* status_;
  QTabWidget* tabs_;
  QListWidget* poll_list_;
  QListWidget* chain_list_;
  QListWidget* task_list_;
  QLineEdit* poll_topic_;
  QLineEdit* poll_options_;
  QCheckBox* poll_deadline_on_;   // 「设截止」勾选（不勾=不限期）
  QDateTimeEdit* poll_deadline_;
  QCheckBox* poll_anon_;          // 「匿名」勾选（台账不回 voter）
  QCheckBox* poll_multi_;         // 「多选」勾选（choice 存位集）
  QLineEdit* poll_choice_;
  QLineEdit* chain_title_;
  QLineEdit* chain_hint_;
  QLineEdit* chain_content_;
  QLineEdit* task_title_;
  QLineEdit* task_assignee_;
  QPushButton* btn_connect_;
  QPushButton* btn_poll_add_;
  QPushButton* btn_poll_vote_;
  QPushButton* btn_poll_close_;
  QPushButton* btn_chain_add_;
  QPushButton* btn_chain_join_;
  QPushButton* btn_chain_close_;
  QPushButton* btn_task_add_;
  QPushButton* btn_task_claim_;
  QPushButton* btn_task_done_;
  QPushButton* btn_refresh_;
};

} // namespace memex::client
