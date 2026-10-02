#include "collab_engine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSysInfo>

#include <algorithm>

#include <memex/protocol/messages.hpp>
#include <core/local_store.hpp>

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace memex::client {

using memex::protocol::Message;
using memex::protocol::MsgType;

namespace {

// machine-id 的两个常规落点（发行版差异）；都拿不到则退化为主机名。
QString read_machine_id() {
  for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
    QFile f(QString::fromLatin1(path));
    if (f.open(QIODevice::ReadOnly)) {
      const QString id = QString::fromUtf8(f.readAll()).trimmed();
      if (!id.isEmpty()) return id;
    }
  }
  return QString{};
}

} // namespace

QString CollabEngine::device_fingerprint() {
  const QString seed = read_machine_id() + QChar('|') + device_name();
  return QString::fromLatin1(
      QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}

QString CollabEngine::device_name() { return QSysInfo::machineHostName(); }

CollabEngine::CollabEngine(QObject* parent) : QObject(parent) {
  socket_ = new QTcpSocket(this);
  connect(socket_, &QTcpSocket::connected, this, [this] { send_login_frame(); });
  connect(socket_, &QTcpSocket::readyRead, this, [this] {
    const QByteArray data = socket_->readAll();
    std::vector<std::string> payloads;
    const auto st = decoder_.feed(
        std::string_view(data.constData(), static_cast<std::size_t>(data.size())),
        payloads);
    if (st == memex::protocol::DecodeStatus::kZeroLength ||
        st == memex::protocol::DecodeStatus::kTooLarge) {
      qWarning() << "[协作] 非法帧，断开";
      socket_->abort();
      return;
    }
    for (const auto& p : payloads) handle_frame(QByteArray(p.data(), static_cast<int>(p.size())));
  });
  connect(socket_, &QTcpSocket::disconnected, this, [this] {
    const bool was_logged_in = logged_in_;
    const bool expected = kicking_ || manual_logout_;
    teardown(was_logged_in && !expected);
    if (was_logged_in && !expected) {
      emit connection_lost();
      schedule_reconnect();
    }
  });
  connect(socket_, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) {
            if (logged_in_) return;
            if (reconnecting_) {
              schedule_reconnect();
            } else {
              emit login_failed(QStringLiteral("无法连接服务器"));
            }
          });

  // 定时器连接（QTimer 对象，非指针）
  connect(&reconnect_timer_, &QTimer::timeout, this, [this] {
    reconnect_timer_.stop();
    qInfo() << "[协作] 自动重连" << host_ << ":" << port_;
    socket_->connectToHost(host_, port_);
  });
  reconnect_timer_.setSingleShot(true);

  connect(&heartbeat_timer_, &QTimer::timeout, this, [this] {
    if (heartbeat_missed_ >= heartbeat_max_missed_) {
      qWarning() << "[协作] 心跳超时，主动断开并转入重连";
      socket_->abort();
      return;
    }
    ++heartbeat_missed_;
    Message ping;
    ping.set_type(MsgType::PING);
    ping.set_from(account_.toStdString());
    ping.set_to("server");
    ping.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
    send_frame(ping);
  });

  connect(&delivery_timer_, &QTimer::timeout, this, [this] { check_delivery_timeouts(); });
}

CollabEngine::~CollabEngine() = default;

void CollabEngine::set_heartbeat(int interval_ms, int max_missed) {
  heartbeat_interval_ms_ = interval_ms;
  heartbeat_max_missed_ = max_missed;
}

void CollabEngine::attach_store(LocalStore* store) { store_ = store; }

void CollabEngine::login(const QString& host, quint16 port,
                         const QString& account, const QString& password) {
  reconnect_timer_.stop();
  teardown(false);
  manual_logout_ = false;
  reconnecting_ = false;
  reconnect_backoff_ms_ = 1000;
  account_ = account;
  password_ = password;
  host_ = host;
  port_ = port;
  qInfo() << "[协作] 连接" << host << ":" << port << "（" << account << "）";
  socket_->connectToHost(host, port);
}

void CollabEngine::logout() {
  if (!logged_in_) return;
  manual_logout_ = true;
  Message m;
  m.set_type(MsgType::LOGOUT);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  send_frame(m);
  kicking_ = true;
  logged_in_ = false;
  socket_->disconnectFromHost();
}

quint64 CollabEngine::send_text(const QString& to, const QString& text) {
  if (to.isEmpty()) return 0;
  const quint64 seq = next_seq_++;
  PendingSend p;
  p.to = to.toStdString();
  p.text = text.toStdString();
  p.seq = seq;
  p.ts_ms = QDateTime::currentMSecsSinceEpoch();

  if (logged_in_) {
    Message m;
    m.set_type(MsgType::TEXT);
    m.set_seq(seq);
    m.set_from(account_.toStdString());
    m.set_to(p.to);
    m.set_ts_ms(p.ts_ms);
    m.mutable_text()->set_text(p.text);
    send_frame(m);
    p.sent_at_ms = QDateTime::currentMSecsSinceEpoch();
    inflight_.insert(seq, p);
    if (!delivery_timer_.isActive()) delivery_timer_.start(500);
  } else if (reconnecting_) {
    // T2.5 断线中断期：本地暂存（不判失败），恢复后按原 seq 补传；
    // 服务端按 msg_id=sha256(from:seq) 去重——此前若已受理也不会重复归档。
    pending_reconnect_.push_back(p);
    qInfo() << "[协作] 断线中断期消息已暂存（待补传）：" << seq;
  } else {
    return 0; // 从未连接成功，不属于中断补传范围
  }

  if (store_) {
    memex::client::StoredMessage sm;
    sm.seq = seq;
    sm.peer = p.to;
    sm.from = account_.toStdString();
    sm.to = p.to;
    sm.ts_ms = p.ts_ms;
    sm.text = p.text;
    sm.source = "collab";
    store_->append(sm);
  }
  return seq;
}

void CollabEngine::recall_text(const QString& to, const QString& msg_id) {
  if (!logged_in_ || to.isEmpty() || msg_id.isEmpty()) return;
  Message m;
  m.set_type(MsgType::RECALL);
  m.set_from(account_.toStdString());
  m.set_to(to.toStdString());
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  m.mutable_recall()->set_msg_id(msg_id.toStdString());
  send_frame(m);
}

void CollabEngine::send_frame(const Message& msg) {
  const std::string frame = memex::protocol::encode(msg);
  socket_->write(QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
}

void CollabEngine::send_login_frame() {
  Message m;
  m.set_type(MsgType::LOGIN);
  m.set_from(device_name().toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* in = m.mutable_login();
  in->set_account(account_.toStdString());
  in->set_password(password_.toStdString());
  in->set_device_fingerprint(device_fingerprint().toStdString());
  in->set_device_kind("desktop");
  in->set_device_name(device_name().toStdString());
  in->set_client_version(MEMEX_VERSION);
  send_frame(m);
}

void CollabEngine::handle_frame(const QByteArray& payload) {
  Message msg;
  try {
    msg = memex::protocol::decode_payload(
        std::string_view(payload.constData(), static_cast<std::size_t>(payload.size())));
  } catch (const memex::protocol::ProtocolError& e) {
    qWarning() << "[协作] 载荷解析失败：" << e.what();
    return;
  }

  switch (msg.type()) {
  case MsgType::LOGIN_RESULT: {
    if (!msg.has_login_result()) return;
    const auto& r = msg.login_result();
    if (r.ok()) {
      logged_in_ = true;
      reconnect_timer_.stop();
      reconnect_backoff_ms_ = 1000;
      start_heartbeat();
      const QString display =
          QString::fromStdString(r.display_name().empty() ? account_.toStdString()
                                                          : r.display_name());
      qInfo() << "[协作] 登录成功：" << account_ << "（" << display << "）";
      if (reconnecting_) {
        reconnecting_ = false;
        emit reconnected();
        // T2.5：重连成功即补传中断期暂存消息（服务端按 msg_id 去重归档）
        flush_pending_sends();
      } else {
        emit logged_in(account_, display);
      }
    } else {
      reconnecting_ = false;
      reconnect_timer_.stop();
      qInfo() << "[协作] 登录失败：" << QString::fromStdString(r.reason());
      emit login_failed(QString::fromStdString(r.reason()));
      socket_->disconnectFromHost();
    }
    break;
  }
  case MsgType::KICK: {
    const auto& k = msg.has_kick() ? msg.kick()
                                   : memex::protocol::v1::Kick{};
    kicking_ = true;
    reconnect_timer_.stop();
    reconnecting_ = false;
    qInfo() << "[协作] 被顶替下线：" << QString::fromStdString(k.reason());
    emit kicked(QString::fromStdString(k.reason()),
                QString::fromStdString(k.replaced_by()));
    logged_in_ = false;
    socket_->disconnectFromHost();
    break;
  }
  case MsgType::PONG:
    heartbeat_missed_ = 0;
    break;
  case MsgType::TEXT:
    if (msg.has_text()) handle_text(msg);
    break;
  case MsgType::ACK:
    handle_ack(msg);
    break;
  case MsgType::RECALL: {
    const std::string target = msg.has_recall() ? msg.recall().msg_id() : std::string{};
    if (!target.empty() && store_) {
      store_->mark_recalled(target);
    }
    emit message_recalled(QString::fromStdString(msg.from()),
                           QString::fromStdString(target));
    break;
  }
  default:
    break;
  }
}

void CollabEngine::handle_text(const Message& msg) {
  const qint64 ts =
      msg.ts_ms() > 0 ? msg.ts_ms() : QDateTime::currentMSecsSinceEpoch();
  bool inserted = true;
  if (store_) {
    memex::client::StoredMessage sm;
    sm.seq = msg.seq();
    sm.peer = msg.from();
    sm.from = msg.from();
    sm.to = msg.to();
    sm.ts_ms = ts;
    sm.text = msg.has_text() ? msg.text().text() : std::string{};
    sm.source = "collab";
    sm.msg_id = msg.msg_id();
    store_->append(sm, &inserted);
  }
  if (!msg.msg_id().empty()) {
    Message ack;
    ack.set_type(MsgType::ACK);
    ack.set_from(account_.toStdString());
    ack.set_to("server");
    ack.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
    ack.mutable_ack()->set_msg_id(msg.msg_id());
    send_frame(ack);
  }
  if (inserted) {
    emit message_received(QString::fromStdString(msg.from()),
                          QString::fromStdString(msg.has_text() ? msg.text().text()
                                                                : std::string{}),
                          ts, QString::fromStdString(msg.msg_id()));
  } else {
    qInfo() << "[协作] 重复补投，按 msg_id 去重：" << QString::fromStdString(msg.msg_id());
  }
}

void CollabEngine::handle_ack(const Message& msg) {
  if (msg.seq() != 0 && inflight_.contains(msg.seq())) {
    const quint64 seq = msg.seq();
    inflight_.remove(seq);
    emit text_delivered(seq, true);
  }
}

void CollabEngine::start_heartbeat() {
  heartbeat_missed_ = 0;
  // 未配置心跳参数（interval=0）则不启用——QTimer::start(0) 会退化为
  // 连发，误触超时重连。
  if (heartbeat_interval_ms_ > 0) {
    heartbeat_timer_.start(heartbeat_interval_ms_);
  }
}

void CollabEngine::stop_heartbeat() {
  heartbeat_timer_.stop();
}

void CollabEngine::schedule_reconnect() {
  if (manual_logout_ || host_.isEmpty() || account_.isEmpty()) return;
  reconnecting_ = true;
  qInfo() << "[协作] " << reconnect_backoff_ms_ << "ms 后重连";
  reconnect_timer_.start(reconnect_backoff_ms_);
  reconnect_backoff_ms_ = std::min(reconnect_backoff_ms_ * 2, CollabEngine::kMaxBackoffMs);
}

void CollabEngine::check_delivery_timeouts() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (auto it = inflight_.begin(); it != inflight_.end();) {
    if (it.value().sent_at_ms > 0 &&
        now - it.value().sent_at_ms > CollabEngine::kDeliveryTimeoutMs) {
      // 超时未回执：不判失败，转待补传（服务端可能已受理——补传按
      // msg_id 去重，不会重复归档；真正送达以回执为准）
      PendingSend p = it.value();
      p.sent_at_ms = 0;
      pending_reconnect_.push_back(p);
      it = inflight_.erase(it);
    } else {
      ++it;
    }
  }
  if (inflight_.isEmpty()) delivery_timer_.stop();
}

// 重连成功后补传：暂存消息按原 seq 重发，服务端 sha256(from:seq) 幂等归档
void CollabEngine::flush_pending_sends() {
  if (pending_reconnect_.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const PendingSend& p : pending_reconnect_) {
    Message m;
    m.set_type(MsgType::TEXT);
    m.set_seq(p.seq);
    m.set_from(account_.toStdString());
    m.set_to(p.to);
    m.set_ts_ms(p.ts_ms);
    m.mutable_text()->set_text(p.text);
    send_frame(m);
    PendingSend inflight = p;
    inflight.sent_at_ms = now;
    inflight_.insert(p.seq, inflight);
    qInfo() << "[协作] 补传中断期消息（seq" << p.seq << "）";
  }
  pending_reconnect_.clear();
  if (!delivery_timer_.isActive()) delivery_timer_.start(500);
}

void CollabEngine::teardown(bool unexpected) {
  logged_in_ = false;
  kicking_ = false;
  stop_heartbeat();
  if (unexpected) {
    // 意外断开：在途消息转待补传（可能已达服务端——重发按 msg_id 幂等）
    for (auto it = inflight_.constBegin(); it != inflight_.constEnd(); ++it) {
      PendingSend p = it.value();
      p.sent_at_ms = 0;
      pending_reconnect_.push_back(p);
    }
  } else {
    // 主动登出／被踢／重新登录：清队列并回报失败，不再补传
    for (auto it = inflight_.constBegin(); it != inflight_.constEnd(); ++it) {
      emit text_delivered(it.key(), false);
    }
    pending_reconnect_.clear();
  }
  inflight_.clear();
  delivery_timer_.stop();
  decoder_.reset();
  if (socket_->state() != QAbstractSocket::UnconnectedState) {
    socket_->abort();
  }
}

std::string CollabEngine::status_text() const {
  return logged_in_ ? "协作态 · 已登录（" + account_.toStdString() + "）"
                    : "协作态 · 未登录";
}

} // namespace memex::client