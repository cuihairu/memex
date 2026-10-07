// R26-2 群服务器面板：服务器列表红绿灯（●绿=agent 在线/●红=失联或未打点）
// ＋最近一拍状态详情（CPU/内存/磁盘/负载）＋登记（群主/管理员；重登记=
// 轮换令牌须确认）。注册令牌只在登记回包出现一次，显示在只读框里喂给
// 服务器上的 memex_agent --token（客户端零凭据——面板只见掩码状态）。
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

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int server_count() const;
  QListWidget* server_list_widget() const { return servers_; }
  QString enroll_token_text() const;

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
  QPushButton* btn_connect_;
  QPushButton* btn_refresh_;
  QPushButton* btn_enroll_;
};

} // namespace memex::client
