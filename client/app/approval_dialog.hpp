// 二期·审批窗口（请假起步）：发起（类型白名单 年假/事假/病假/调休）/
// 同意/拒绝/撤回。审批人=直属上级（无上级 org-admin 兜底），判权全部
// 服务端现裁——客户端只提交与展示。独立文件面会话（与 R23-3 文件助手
// 同构：协作面同源账号、文件面独立端口）。开窗期 30s 轮询 diff 通知
//（R27-1 口径）：新待决→提醒待决人、决定落定→提醒申请人，经通知中心。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QString>

#include <QtGlobal>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace memex::client {

class FilesClient;

class ApprovalDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ApprovalDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // 程序化入口（测试共用）：类型白名单外服务端拒（客户端不重复白名单）
  bool add_approval(const QString& type, const QString& from,
                    const QString& to, const QString& reason);
  // 选中待决行同意/拒绝（批注可空；仅「待我决」行可决）
  bool decide_selected(bool approved, const QString& note);
  // 撤回选中申请（申请人专属且仅 pending；服务端裁决）
  bool withdraw_selected();
  // 拉列表（我的申请＋待我决）
  void refresh();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int approval_count() const;  // 列表总条数（我的申请＋待我决）
  QListWidget* list() const { return list_; }
  // 选中行 id（未选中=-1）
  qint64 selected_id() const;
  // 选中行种类："mine"=我的申请、"pending"=待我决、未选中=空串
  QString selected_kind() const;

 private:
  void build_ui();
  void populate(const QJsonArray& mine, const QJsonArray& pending);
  void set_status(const QString& text, bool error = false);

  // 开窗期通知 diff 记忆集（R27-1 轮询口径：只提醒开窗后新出现的变化）
  QSet<QString> seen_pending_;  // 待我决 id 已见集合（新现→通知待决人）
  QMap<QString, QString> seen_mine_status_;  // 我的申请 id→状态（终态迁移→通知申请人）
  bool seeded_{false};          // 首次 populate 全量静默吸收（不轰炸存量）
  bool skip_notify_once_{false};  // 自己撤回触发的刷新跳过 diff（不提醒自己）

  FilesClient* client_;
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_box_;
  QLineEdit* password_;
  QLabel* status_;
  QListWidget* list_;
  QComboBox* type_;
  QLineEdit* from_;
  QLineEdit* to_;
  QLineEdit* reason_;
  QLineEdit* note_;
  QPushButton* btn_connect_;
  QPushButton* btn_add_;
  QPushButton* btn_approve_;
  QPushButton* btn_reject_;
  QPushButton* btn_withdraw_;
  QPushButton* btn_refresh_;
};

} // namespace memex::client
