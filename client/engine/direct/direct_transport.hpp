// 直连态点对点传输（T1.2 文本半边）：
// TCP 监听固定端口段 2426–2437（同机多实例自动错段），
// 复用 common 帧协议（长度前缀 + Envelope），文本消息送达以 kAck 确认。
// 平台-8 起连接级安全信道（SecureChannel）：每条连接先做两帧握手
// （Ed25519 身份 + X25519 会话密钥 + TOFU 定针），握手后线路无明文，
// 应用载荷经 AEAD 封装；握手不成即连接失效（fail-closed，无明文回退）。
// 文件字节流（T1.3）同样经此端口段，不经服务端。
#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <map>
#include <memory>
#include <string>

#include <memex/protocol/messages.hpp>

#include "secure_channel.hpp"

namespace memex::client {

class LocalStore;

// 直连 TCP 端口段（报告固定端口段）
inline constexpr quint16 kDirectTcpPortBegin = 2426;
inline constexpr quint16 kDirectTcpPortEnd = 2437;

class DirectTransport : public QObject {
  Q_OBJECT

public:
  explicit DirectTransport(QObject* parent = nullptr);

  void set_device_id(std::string id) { device_id_ = std::move(id); }
  // 平台-8：安全信道材料（身份与本地库）。未设置时拒绝一切连接
  // （fail-closed，不会退回明文）。
  void set_secure(const DeviceIdentity* self, LocalStore* store);

  // 从 preferred 起扫描端口段直到绑定成功；全部占用返回 false。
  bool listen(quint16 preferred = kDirectTcpPortBegin);
  void stop();
  bool listening() const { return server_.isListening(); }
  quint16 port() const { return server_.serverPort(); }

  // 异步发送文本（先握手后发信，握手失败即 delivered(seq,false)）；
  // 送达结果经 delivered(seq, ok) 回报（超时或断连为 false）。
  void send_text(const QHostAddress& target, quint16 target_port,
                 const std::string& to_id, std::uint64_t seq,
                 const std::string& text);

signals:
  // 收到对端文本（尚未回执前先缓存，回执由本类发出）
  void text_received(const QString& from_id, const QString& to_id,
                     quint64 seq, qint64 ts_ms, const QString& text);
  void delivered(quint64 seq, bool ok);
  // 文件连接移交（T1.3）：kFileMeta 已解出，socket 所有权随之移交
  // （父对象置空、本类槽位断开），此后字节流由文件服务按块处理。
  // ch＝该连接的安全信道（已握手建立），数据面续用其密钥解封。
  void file_incoming(QTcpSocket* socket, const memex::protocol::Message& meta,
                     std::shared_ptr<SecureChannel> ch);

private:
  struct Pending {
    QTcpSocket* socket{nullptr};
    QTimer* timer{nullptr};
    std::shared_ptr<SecureChannel> ch; // 每连接一信道（发起方）
    bool text_sent{false};             // 握手已立、文本已写出
  };

  void on_new_connection();
  void on_inbound_ready(QTcpSocket* socket);
  void handle_payload(QTcpSocket* inbound, const std::string& payload,
                      const std::shared_ptr<SecureChannel>& ch);

  QTcpServer server_;
  std::string device_id_;
  const DeviceIdentity* self_{nullptr};
  LocalStore* store_{nullptr};
  std::map<QTcpSocket*, std::shared_ptr<SecureChannel>> inbound_channels_;
  std::map<std::uint64_t, Pending> pending_;
};

} // namespace memex::client
