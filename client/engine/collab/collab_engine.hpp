// 协作引擎：登录协作服务端的长连接（T2.1 登录与互踢；T2.2 心跳／重连／离线补投）。
// 「客户端提示」以 kicked 信号送达界面层（T2.4 模式切换时接入提示框）。
#pragma once

#include <QObject>
#include <QString>
#include <QTcpSocket>

#include <memex/protocol/frame.hpp>

namespace memex::client {

class CollabEngine : public QObject {
  Q_OBJECT

public:
  explicit CollabEngine(QObject* parent = nullptr);
  ~CollabEngine() override;

  // 设备指纹：machine-id 与主机名的 SHA-256（同机稳定、跨机不撞）。
  static QString device_fingerprint();
  // 设备名：主机名。
  static QString device_name();

  bool is_logged_in() const { return logged_in_; }
  QString account() const { return account_; }
  std::string status_text() const;

public slots:
  // 连接并登录（desktop 主设备）；结果异步回报：logged_in／login_failed。
  void login(const QString& host, quint16 port, const QString& account,
             const QString& password);
  // 主动登出：发 LOGOUT 后断开（T2.4 切换形态入口）。
  void logout();

signals:
  void logged_in(const QString& account, const QString& display_name);
  void login_failed(const QString& reason);
  // 单点在线被踢（第二台桌面登录）：reason 供提示文案。
  void kicked(const QString& reason, const QString& replaced_by);
  void connection_lost();

private:
  void send_login(const QString& password);
  void handle_frame(const QByteArray& payload);
  void teardown();

  QTcpSocket* socket_{nullptr};
  memex::protocol::FrameDecoder decoder_;
  QString account_;
  QString password_;
  QString host_;
  quint16 port_{0};
  bool logged_in_{false};
  bool kicking_{false}; // 互踢／登出引发的断开，不再报连接丢失
};

} // namespace memex::client
