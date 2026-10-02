#include "collab_engine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSysInfo>

#include <memex/protocol/messages.hpp>

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
  connect(socket_, &QTcpSocket::connected, this,
          [this] { send_login(password_); });
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
    teardown();
    if (was_logged_in && !kicking_) emit connection_lost();
  });
  connect(socket_, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) {
        if (!logged_in_) {
          emit login_failed(QStringLiteral("无法连接服务器"));
        } else if (!kicking_) {
          emit connection_lost();
        }
      });
}

CollabEngine::~CollabEngine() = default;

void CollabEngine::login(const QString& host, quint16 port,
                         const QString& account, const QString& password) {
  teardown();
  account_ = account;
  password_ = password;
  host_ = host;
  port_ = port;
  qInfo() << "[协作] 连接" << host << ":" << port << "（" << account << "）";
  socket_->connectToHost(host, port);
}

void CollabEngine::logout() {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::LOGOUT);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  const std::string frame = memex::protocol::encode(m);
  socket_->write(QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
  socket_->flush();
  kicking_ = true; // 主动登出的断开不算异常
  logged_in_ = false;
  socket_->disconnectFromHost();
}

void CollabEngine::send_login(const QString& password) {
  Message m;
  m.set_type(MsgType::LOGIN);
  m.set_from(device_name().toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* in = m.mutable_login();
  in->set_account(account_.toStdString());
  in->set_password(password.toStdString());
  in->set_device_fingerprint(device_fingerprint().toStdString());
  in->set_device_kind("desktop"); // 桌面端＝主设备：单点在线
  in->set_device_name(device_name().toStdString());
  in->set_client_version(MEMEX_VERSION);
  const std::string frame = memex::protocol::encode(m);
  socket_->write(QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
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
      const QString display =
          QString::fromStdString(r.display_name().empty() ? account_.toStdString()
                                                          : r.display_name());
      qInfo() << "[协作] 登录成功：" << account_ << "（" << display << "）";
      emit logged_in(account_, display);
    } else {
      qInfo() << "[协作] 登录失败：" << QString::fromStdString(r.reason());
      emit login_failed(QString::fromStdString(r.reason()));
      socket_->disconnectFromHost();
    }
    break;
  }
  case MsgType::KICK: {
    const auto& k = msg.has_kick() ? msg.kick()
                                   : memex::protocol::v1::Kick{};
    kicking_ = true; // 服务端互踢，属预期断开
    qInfo() << "[协作] 被顶替下线：" << QString::fromStdString(k.reason());
    emit kicked(QString::fromStdString(k.reason()),
                QString::fromStdString(k.replaced_by()));
    logged_in_ = false;
    socket_->disconnectFromHost();
    break;
  }
  case MsgType::PONG:
    // 心跳簿记在 T2.2（重连与离线补投）接入
    break;
  default:
    // 协作消息收发在 T2.2 接入
    break;
  }
}

void CollabEngine::teardown() {
  logged_in_ = false;
  kicking_ = false;
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
