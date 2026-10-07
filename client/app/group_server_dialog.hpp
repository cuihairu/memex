// R26-2 群服务器面板：服务器列表红绿灯（●绿=agent 在线/●红=失联或未打点）
// ＋最近一拍状态详情（CPU/内存/磁盘/负载）＋登记（群主/管理员；重登记=
// 轮换令牌须确认）。注册令牌只在登记回包出现一次，显示在只读框里喂给
// 服务器上的 memex_agent --token（客户端零凭据——面板只见掩码状态）。
// R26-3 追加 SSH 会话区：选中服务器发起→一次性短票签发即兑现（票不出框）
// →本地终端唤起尽力而为、命令框恒可复制→关闭落时长；接入留痕列表同屏。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace memex::client {

class FilesClient;

class GroupServerDialog : public QDialog {
  Q_OBJECT
 public:
  explicit GroupServerDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 切目标群（懒建复用同一窗口时换群：清列表重拉）
  void set_group(quint64 gid, const QString& group_name);

  void refresh();
  // 程序化登记（测试共用；确认框只挂按钮路径——重登记作废旧令牌）
  bool enroll_server(const QString& name, const QString& host);
  // 发起 SSH 会话（作用于服务器列表当前选中行；短票签发后即时兑现→
  // 本地唤起尽力而为，命令框恒有命令可复制）
  bool request_session();
  // 收尾本人进行中的会话（落时长留痕）
  bool close_session();
  // 存/删所选服务器凭据（仅群主/管理员；服务端加密落库——客户端零凭据，
  // 明文只此一次出门）
  bool set_server_credential(const QString& value);
  bool delete_server_credential();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int server_count() const;
  QListWidget* server_list_widget() const { return servers_; }
  QString enroll_token_text() const;
  int session_count() const;
  QListWidget* session_list_widget() const { return sessions_; }
  QString session_command_text() const;

 private:
  void build_ui();
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  quint64 gid_{0};
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QLineEdit* srv_name_;
  QLineEdit* srv_host_;
  QLineEdit* token_;
  QLabel* status_;
  QListWidget* servers_;
  QListWidget* sessions_;
  QLineEdit* cmd_;
  QLineEdit* srv_cred_;
  QPushButton* btn_connect_;
  QPushButton* btn_refresh_;
  QPushButton* btn_enroll_;
  QPushButton* btn_session_;
  QPushButton* btn_close_;
  QPushButton* btn_cred_set_;
  QPushButton* btn_cred_del_;
  qint64 open_session_id_{0}; // 本人已兑现未收尾的会话（0=无）
};

} // namespace memex::client
