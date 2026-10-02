#include "direct_transport.hpp"

#include <QDateTime>
#include <QDebug>

namespace memex::client {

using memex::protocol::DecodeStatus;
using memex::protocol::FrameDecoder;
using memex::protocol::Message;
using memex::protocol::MsgType;

namespace {
constexpr int kAckTimeoutMs = 3000;
constexpr int kMaxStreamFrame = 4 * 1024 * 1024; // 与 common 上限一致
} // namespace

DirectTransport::DirectTransport(QObject* parent) : QObject(parent) {
  connect(&server_, &QTcpServer::newConnection, this,
          &DirectTransport::on_new_connection);
}

bool DirectTransport::listen(quint16 preferred) {
  if (server_.isListening()) return true;
  const quint16 begin = preferred < kDirectTcpPortBegin ? kDirectTcpPortBegin : preferred;
  for (quint16 port = begin; port <= kDirectTcpPortEnd; ++port) {
    if (server_.listen(QHostAddress::AnyIPv4, port)) {
      qDebug() << "[直连接入] 监听 TCP" << port;
      return true;
    }
  }
  qWarning() << "[直连接入] 端口段" << begin << "-" << kDirectTcpPortEnd << "全部占用";
  return false;
}

void DirectTransport::stop() {
  // 先断信号再 abort：避免 stop 迭代期间回入 finish
  for (auto& [seq, p] : pending_) {
    if (p.timer) p.timer->stop();
    if (p.socket) {
      p.socket->disconnect(this);
      p.socket->abort();
    }
  }
  pending_.clear();
  // 只清理仍在帧解码阶段的入站连接；文件连接已移交文件服务，由其自管
  for (auto& [socket, decoder] : inbound_decoders_) {
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
  }
  inbound_decoders_.clear();
  server_.close();
}

void DirectTransport::on_new_connection() {
  while (server_.hasPendingConnections()) {
    QTcpSocket* socket = server_.nextPendingConnection();
    inbound_decoders_[socket] = std::make_unique<FrameDecoder>();
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
      on_inbound_ready(socket);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
      inbound_decoders_.erase(socket);
      socket->deleteLater();
    });
  }
}

void DirectTransport::on_inbound_ready(QTcpSocket* socket) {
  const auto it = inbound_decoders_.find(socket);
  if (it == inbound_decoders_.end()) return;

  const QByteArray data = socket->readAll();
  std::vector<std::string> payloads;
  const DecodeStatus st =
      it->second->feed(std::string_view(data.constData(),
                                         static_cast<std::size_t>(data.size())),
                       payloads);
  if (st == DecodeStatus::kZeroLength || st == DecodeStatus::kTooLarge) {
    qWarning() << "[直连接入] 非法帧，断开：" << memex::protocol::decode_status_name(st);
    socket->abort();
    return;
  }
  for (const auto& payload : payloads) {
    if (!inbound_decoders_.contains(socket)) return; // 已移交或清理
    handle_payload(socket, payload);
  }
}

void DirectTransport::handle_payload(QTcpSocket* socket, const std::string& payload) {
  Message msg;
  try {
    msg = Message::decode_payload(payload);
  } catch (const memex::protocol::ProtocolError& e) {
    qWarning() << "[直连接入] 载荷解析失败：" << e.what();
    return; // 畸形载荷丢弃，不断开（可能只是个别坏帧）
  }

  switch (msg.type) {
  case MsgType::kText: {
    const QString from_id = QString::fromStdString(msg.from);
    const QString to_id = QString::fromStdString(msg.to);
    const QString text =
        (msg.body.contains("text") && msg.body.at("text").is_string())
            ? QString::fromStdString(msg.body.at("text").get<std::string>())
            : QString{};
    emit text_received(from_id, to_id, static_cast<quint64>(msg.seq), msg.ts_ms, text);

    // 送达确认：kAck 携带原 seq
    Message ack;
    ack.type = MsgType::kAck;
    ack.seq = msg.seq;
    ack.from = device_id_;
    ack.to = msg.from;
    ack.ts_ms = QDateTime::currentMSecsSinceEpoch();
    ack.body = nlohmann::json::object();
    const std::string frame = ack.encode();
    socket->write(QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
    break;
  }
  case MsgType::kAck:
    // 入站连接不承载发送确认（确认走各自出站连接）
    break;
  case MsgType::kFileMeta:
    // T1.3：文件连接整体移交文件服务（此后为二进制块流，不走帧解码）
    inbound_decoders_.erase(socket);
    socket->disconnect(this);
    socket->setParent(nullptr);
    emit file_incoming(socket, msg);
    return;
  default:
    // 其余类型由后续任务接入
    break;
  }
}

void DirectTransport::send_text(const QHostAddress& target, quint16 target_port,
                                const std::string& to_id, std::uint64_t seq,
                                const std::string& text) {
  if (pending_.contains(seq)) {
    emit delivered(seq, false);
    return;
  }

  QTcpSocket* socket = new QTcpSocket(this);
  QTimer* timer = new QTimer(socket);
  timer->setSingleShot(true);
  pending_.emplace(seq, Pending{socket, timer});

  // 幂等收尾：确认一次即断开并释放，后续信号（abort 触发）全部失效
  auto done = std::make_shared<bool>(false);
  const auto finish = [this, seq, socket, timer, done](bool ok) {
    if (*done) return;
    *done = true;
    timer->stop();
    socket->disconnect(this); // 断开本对象的所有槽位，防二次触发
    socket->abort();
    socket->deleteLater();
    pending_.erase(seq);
    emit delivered(seq, ok);
  };

  connect(socket, &QTcpSocket::connected, this, [this, socket, to_id, seq, text] {
    Message msg;
    msg.type = MsgType::kText;
    msg.seq = seq;
    msg.from = device_id_;
    msg.to = to_id;
    msg.ts_ms = QDateTime::currentMSecsSinceEpoch();
    msg.body = nlohmann::json{{"text", text}};
    const std::string frame = msg.encode();
    socket->write(QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
  });

  connect(socket, &QTcpSocket::readyRead, this, [socket, seq, finish] {
    // 出站连接只期待一帧 kAck；累积缓冲容忍半包
    QByteArray buf = socket->property("inbuf").toByteArray();
    buf.append(socket->readAll());
    socket->setProperty("inbuf", buf);
    if (buf.size() < 4) return;
    const quint32 len =
        (quint32(quint8(buf[0])) << 24) | (quint32(quint8(buf[1])) << 16) |
        (quint32(quint8(buf[2])) << 8) | quint32(quint8(buf[3]));
    if (len == 0 || len > kMaxStreamFrame) {
      finish(false);
      return;
    }
    if (buf.size() < qsizetype(4 + len)) return;
    try {
      const Message ack =
          Message::decode_payload(std::string_view(buf.constData() + 4, len));
      finish(ack.type == MsgType::kAck && ack.seq == seq);
    } catch (const memex::protocol::ProtocolError&) {
      finish(false);
    }
  });

  connect(socket, &QTcpSocket::errorOccurred, this,
          [finish](QAbstractSocket::SocketError) { finish(false); });
  connect(socket, &QTcpSocket::disconnected, this, [finish] { finish(false); });
  connect(timer, &QTimer::timeout, this, [finish] { finish(false); });

  timer->start(kAckTimeoutMs);
  socket->connectToHost(target, target_port);
}

} // namespace memex::client
