#include "direct_transport.hpp"

#include <QDateTime>
#include <QDebug>

namespace memex::client {

using memex::protocol::Message;
using memex::protocol::MsgType;

namespace {
constexpr int kAckTimeoutMs = 3000; // 含握手（握手为 µs 级，不起独立定时）
} // namespace

DirectTransport::DirectTransport(QObject* parent) : QObject(parent) {
  connect(&server_, &QTcpServer::newConnection, this,
          &DirectTransport::on_new_connection);
}

void DirectTransport::set_secure(const DeviceIdentity* self,
                                 LocalStore* store) {
  self_ = self;
  store_ = store;
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
  // 只清理仍在入站信道阶段的连接；文件连接已移交文件服务，由其自管
  for (auto& [socket, ch] : inbound_channels_) {
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
  }
  inbound_channels_.clear();
  server_.close();
}

void DirectTransport::on_new_connection() {
  while (server_.hasPendingConnections()) {
    QTcpSocket* socket = server_.nextPendingConnection();
    // 平台-8：每入站连接一个安全信道（响应方）；未注入身份即信道失效，
    // 连接收到首字节即断（fail-closed，无明文回退）。
    inbound_channels_[socket] = std::make_shared<SecureChannel>(
        SecureChannel::Role::kResponder, self_, store_, device_id_,
        std::string{});
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
      on_inbound_ready(socket);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
      inbound_channels_.erase(socket);
      socket->deleteLater();
    });
  }
}

void DirectTransport::on_inbound_ready(QTcpSocket* socket) {
  const auto it = inbound_channels_.find(socket);
  if (it == inbound_channels_.end()) return;
  const std::shared_ptr<SecureChannel> ch = it->second;

  const QByteArray data = socket->readAll();
  const SecureChannel::Fed fed = ch->feed(
      std::string_view(data.constData(),
                       static_cast<std::size_t>(data.size())));
  if (fed.failed) {
    qWarning() << "[直连接入] 安全信道失效，断开：" << fed.reason;
    inbound_channels_.erase(socket);
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    return;
  }
  if (!fed.reply.empty()) {
    socket->write(QByteArray(fed.reply.data(),
                             static_cast<qsizetype>(fed.reply.size())));
  }
  for (const auto& payload : fed.payloads) {
    if (inbound_channels_.find(socket) == inbound_channels_.end()) return; // 已移交或清理
    handle_payload(socket, payload, ch);
  }
}

void DirectTransport::handle_payload(QTcpSocket* socket,
                                     const std::string& payload,
                                     const std::shared_ptr<SecureChannel>& ch) {
  Message msg;
  try {
    msg = memex::protocol::decode_payload(payload);
  } catch (const memex::protocol::ProtocolError& e) {
    qWarning() << "[直连接入] 载荷解析失败：" << e.what();
    return; // 畸形载荷丢弃，不断开（AEAD 已认证来源，仅个别坏帧）
  }

  // 身份绑定：应用层 from/to 必须与已认证信道一致（防已认证对端冒名）
  const std::string authed_peer = ch->peer_id();
  if (msg.from() != authed_peer || msg.to() != device_id_) {
    qWarning() << "[直连接入] 应用层身份与信道不符，丢弃："
               << QString::fromStdString(msg.from());
    return;
  }

  switch (msg.type()) {
  case MsgType::TEXT: {
    const QString from_id = QString::fromStdString(msg.from());
    const QString to_id = QString::fromStdString(msg.to());
    const QString text = msg.has_text()
                             ? QString::fromStdString(msg.text().text())
                             : QString{};
    emit text_received(from_id, to_id, static_cast<quint64>(msg.seq()),
                       msg.ts_ms(), text);

    // 送达确认：ACK 携带原 seq，经信道密文回写
    Message ack;
    ack.set_type(MsgType::ACK);
    ack.set_seq(msg.seq());
    ack.set_from(device_id_);
    ack.set_to(msg.from());
    ack.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
    const std::string wire =
        ch->protect(memex::protocol::encode_payload(ack));
    if (wire.empty()) {
      qWarning() << "[直连接入] 回执封装失败（信道已失效）";
      return;
    }
    socket->write(QByteArray(wire.data(), static_cast<qsizetype>(wire.size())));
    break;
  }
  case MsgType::ACK:
    // 入站连接不承载发送确认（确认走各自出站连接）
    break;
  case MsgType::FILE_META:
    // T1.3：文件连接整体移交文件服务（此后数据面经同一信道解密）
    inbound_channels_.erase(socket);
    socket->disconnect(this);
    socket->setParent(nullptr);
    emit file_incoming(socket, msg, ch);
    return;
  default:
    // 其余类型由后续任务接入
    break;
  }
}

void DirectTransport::send_text(const QHostAddress& target, quint16 target_port,
                                const std::string& to_id, std::uint64_t seq,
                                const std::string& text) {
  if (pending_.find(seq) != pending_.end()) {
    emit delivered(seq, false);
    return;
  }
  if (!self_ || !store_) {
    // 平台-8：安全材料未注入即拒发（不退回明文）
    qWarning() << "[直连发出] 安全材料未就绪，拒发 seq=" << seq;
    emit delivered(seq, false);
    return;
  }

  QTcpSocket* socket = new QTcpSocket(this);
  QTimer* timer = new QTimer(socket);
  timer->setSingleShot(true);
  // 每连接一个信道（发起方）：先握手后发信，握手失败即送达失败
  auto ch = std::make_shared<SecureChannel>(SecureChannel::Role::kInitiator,
                                            self_, store_, device_id_, to_id);
  pending_.emplace(seq, Pending{socket, timer, std::move(ch), false});

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

  connect(socket, &QTcpSocket::connected, this, [this, seq, finish] {
    const auto it = pending_.find(seq);
    if (it == pending_.end()) return;
    const std::string hello = it->second.ch->start();
    if (hello.empty()) {
      finish(false); // 握手启动失败（身份/随机数未就绪）
      return;
    }
    QTcpSocket* s = it->second.socket;
    s->write(QByteArray(hello.data(), static_cast<qsizetype>(hello.size())));
  });

  connect(socket, &QTcpSocket::readyRead, this,
          [this, seq, to_id, text, finish] {
    const auto it = pending_.find(seq);
    if (it == pending_.end()) return;
    Pending& p = it->second;

    const QByteArray data = p.socket->readAll();
    const SecureChannel::Fed fed = p.ch->feed(
        std::string_view(data.constData(),
                         static_cast<std::size_t>(data.size())));
    if (fed.failed) {
      finish(false); // 握手失败或密文认证失败——连接即送达失败
      return;
    }
    if (fed.established && !p.text_sent) {
      Message msg;
      msg.set_type(MsgType::TEXT);
      msg.set_seq(seq);
      msg.set_from(device_id_);
      msg.set_to(to_id);
      msg.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
      msg.mutable_text()->set_text(text);
      const std::string wire =
          p.ch->protect(memex::protocol::encode_payload(msg));
      if (wire.empty()) {
        finish(false);
        return;
      }
      p.socket->write(
          QByteArray(wire.data(), static_cast<qsizetype>(wire.size())));
      p.text_sent = true;
    }
    for (const auto& payload : fed.payloads) {
      if (!p.text_sent) {
        finish(false); // 未发先收，协议违例
        return;
      }
      Message m;
      try {
        m = memex::protocol::decode_payload(payload);
      } catch (const memex::protocol::ProtocolError&) {
        finish(false);
        return;
      }
      finish(m.type() == MsgType::ACK && m.seq() == seq);
      return; // 出站连接只期待一帧 ACK
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
