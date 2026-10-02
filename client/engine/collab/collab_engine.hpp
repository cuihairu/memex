// 协作引擎：登录协作服务端的长连接（T2.1 登录与互踢；T2.2 心跳／重连／离线补投／消息路由；
// T2.5 断线中断期消息本地暂存、恢复后按消息标识去重补传归档）。
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#include <memex/protocol/frame.hpp>
#include <memex/protocol/messages.hpp>

namespace memex::client {

class LocalStore;

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

  // 设置心跳参数：interval_ms（PING 间隔）、missed_max（连续丢包阈值，触发判死重连）。
  void set_heartbeat(int interval_ms, int missed_max);

  // 绑定本地库（用于协作消息落库与 msg_id 去重）。由外部传入并管理生命周期。
  void attach_store(LocalStore* store);

public slots:
  // 连接并登录（desktop 主设备）；结果异步回报：logged_in／login_failed。
  void login(const QString& host, quint16 port, const QString& account,
             const QString& password);
  // 主动登出：发 LOGOUT 后断开（T2.4 切换形态入口）。
  void logout();
  // 发送文本消息（协作态）：返回本地序列号（sent 回执以此 seq 关联）。
  quint64 send_text(const QString& to, const QString& text);
  // 撤回一条协作态消息（服务端校验权限后全网标记，原文留痕不清）。
  void recall_text(const QString& to, const QString& msg_id);
  // 请求组织架构（部门树＋成员资料，T3.1）；结果经 org_received 送达。
  void query_org();

signals:
  void logged_in(const QString& account, const QString& display_name);
  void login_failed(const QString& reason);
  // 单点在线被踢（第二台同类型设备登录）：reason 供提示文案。
  void kicked(const QString& reason, const QString& replaced_by);
  void connection_lost();
  // 重连成功（自动重登后恢复在线，离线消息补投已由服务端推送）。
  void reconnected();
  // 收到协作文本消息：from、text、ts_ms、msg_id（服务端分配，去重键）。
  void message_received(const QString& from, const QString& text, qint64 ts_ms,
                        const QString& msg_id);
  // 发送方受理回执：seq（本地 send_text 返回值）、ok（服务端已接收并入队）。
  void text_delivered(quint64 seq, bool ok);
  // 一条协作消息被撤回（本地与对端副本都置标记，原文保留）。
  void message_recalled(const QString& from, const QString& msg_id);
  // 组织架构数据（T3.1）：JSON——
  // {"departments":[{"path":"公司/研发部"}],
  //  "members":[{"account","display_name","title","department_path","manager","role"}],
  //  "policies":[{"department_path":"","allow_anonymous":true,
  //              "allow_cross_state":true,"new_device_approval":false}]}（T3.4）
  void org_received(const QString& org_json);

private:
  // 在途／待补传消息（T2.5）：断线中断期本地暂存，恢复后按原 seq 补传，
  // 服务端按 msg_id=sha256(from:seq) 去重归档——重复补传不产生重复归档。
  struct PendingSend {
    std::string to;
    std::string text;
    quint64 seq{0};
    qint64 ts_ms{0};
    qint64 sent_at_ms{0}; // 已发未回执的超时计时（0=尚未发出）
  };

  void send_login_frame();
  void handle_frame(const QByteArray& payload);
  // unexpected=true：意外断开——在途消息转待补传（不判失败）；
  // false：主动登出／被踢／重新登录——清队列并回报失败。
  void teardown(bool unexpected);
  void schedule_reconnect();
  void check_delivery_timeouts();
  void start_heartbeat();
  void stop_heartbeat();
  void send_frame(const memex::protocol::Message& msg);
  void handle_text(const memex::protocol::Message& msg);
  void handle_ack(const memex::protocol::Message& msg);
  void flush_pending_sends(); // 重连成功后补传暂存消息

  QTcpSocket* socket_{nullptr};
  memex::protocol::FrameDecoder decoder_;
  LocalStore* store_{nullptr};
  QString account_;
  QString password_;
  QString host_;
  quint16 port_{0};
  bool logged_in_{false};
  bool kicking_{false};            // 互踢／登出引发的断开，不再报连接丢失
  bool manual_logout_{false};      // 主动登出，不触发重连
  bool reconnecting_{false};       // 正在重连中
  int reconnect_backoff_ms_{1000}; // 重连退避（ms），指数级增至 30s
  QTimer reconnect_timer_;

  int heartbeat_interval_ms_{0};   // 0=未启用
  int heartbeat_max_missed_{0};
  int heartbeat_missed_{0};
  QTimer heartbeat_timer_;

  quint64 next_seq_{1};
  QHash<quint64, PendingSend> inflight_; // seq -> 在途消息（已发未回执；超时转待补传）
  QList<PendingSend> pending_reconnect_; // 断线中断期暂存，恢复后补传
  QTimer delivery_timer_;

  static constexpr qint64 kDeliveryTimeoutMs = 10000;
  static constexpr int kMaxBackoffMs = 30000;
};

} // namespace memex::client