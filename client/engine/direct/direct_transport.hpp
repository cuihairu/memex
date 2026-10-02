// 直连态点对点传输（T1.2 文本半边）：
// TCP 监听固定端口段 2426–2437（同机多实例自动错段），
// 复用 common 帧协议（长度前缀 + JSON），文本消息送达以 kAck 确认。
// 文件字节流（T1.3）同样经此端口段，不经服务端。
#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <map>
#include <memory>

#include <memex/protocol/messages.hpp>

namespace memex::client {

// 直连 TCP 端口段（报告固定端口段）
inline constexpr quint16 kDirectTcpPortBegin = 2426;
inline constexpr quint16 kDirectTcpPortEnd = 2437;

class DirectTransport : public QObject {
  Q_OBJECT

public:
  explicit DirectTransport(QObject* parent = nullptr);

  void set_device_id(std::string id) { device_id_ = std::move(id); }

  // 从 preferred 起扫描端口段直到绑定成功；全部占用返回 false。
  bool listen(quint16 preferred = kDirectTcpPortBegin);
  void stop();
  bool listening() const { return server_.isListening(); }
  quint16 port() const { return server_.serverPort(); }

  // 异步发送文本；送达结果经 delivered(seq, ok) 回报（超时或断连为 false）。
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
  void file_incoming(QTcpSocket* socket, const memex::protocol::Message& meta);

private:
  struct Pending {
    QTcpSocket* socket{nullptr};
    QTimer* timer{nullptr};
  };

  void on_new_connection();
  void on_inbound_ready(QTcpSocket* socket);
  void handle_payload(QTcpSocket* inbound, const std::string& payload);

  QTcpServer server_;
  std::string device_id_;
  std::map<QTcpSocket*, std::unique_ptr<memex::protocol::FrameDecoder>> inbound_decoders_;
  std::map<std::uint64_t, Pending> pending_;
};

} // namespace memex::client
