#include "collab_engine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSysInfo>

#include <algorithm>

#include <nlohmann/json.hpp>

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

} // namespace memex::client
