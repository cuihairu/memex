#include "discovery.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QDebug>

namespace memex::client {

DiscoveryService::DiscoveryService(std::string device_id, std::string device_name,
                                   QObject* parent)
    : QObject(parent), device_id_(std::move(device_id)),
      device_name_(std::move(device_name)) {
  announce_timer_.setTimerType(Qt::PreciseTimer);
  sweep_timer_.setTimerType(Qt::CoarseTimer);
  connect(&announce_timer_, &QTimer::timeout, this, &DiscoveryService::announce);
  connect(&sweep_timer_, &QTimer::timeout, this, &DiscoveryService::sweep);
}

DiscoveryService::~DiscoveryService() { stop(); }

bool DiscoveryService::start(const DiscoveryOptions& opts) {
  if (running_) return true;
  opts_ = opts;

  // ShareAddress：同机多实例共享 2425（验收要求同机双实例互现）
  const auto bind_mode = QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint;
  if (!socket_.bind(QHostAddress::AnyIPv4, opts_.port, bind_mode)) {
    qWarning() << "[直连发现] 绑定 UDP" << opts_.port << "失败：" << socket_.errorString();
    return false;
  }
  connect(&socket_, &QUdpSocket::readyRead, this, &DiscoveryService::on_ready_read,
          Qt::UniqueConnection);

  running_ = true;
  announce(); // 立即宣告一轮
  announce_timer_.start(opts_.announce_interval_ms);
  sweep_timer_.start(opts_.sweep_interval_ms);
  qDebug() << "[直连发现] 启动，UDP" << opts_.port << "device=" << device_id_.c_str();
  return true;
}

void DiscoveryService::stop() {
  if (!running_) return;
  announce_timer_.stop();
  sweep_timer_.stop();
  socket_.close();
  peers_.clear();
  running_ = false;
  qDebug() << "[直连发现] 停止";
}

void DiscoveryService::set_tcp_port(quint16 port) { tcp_port_ = port; }

void DiscoveryService::set_account(const std::string& account) {
  if (account_ == account) return;
  account_ = account;
  // 登出／登录立即补一轮宣告——对端据此判定跨态（T4.2）
  if (running_) announce();
}

QList<Peer> DiscoveryService::peers() const {
  QList<Peer> out;
  out.reserve(static_cast<int>(peers_.size()));
  for (const auto& [_, p] : peers_) out.push_back(p);
  return out;
}

void DiscoveryService::announce() {
  if (!running_) return;

  QJsonObject obj;
  obj.insert(QStringLiteral("magic"), QLatin1String(kDiscoveryMagic));
  obj.insert(QStringLiteral("v"), kDiscoveryVersion);
  obj.insert(QStringLiteral("device_id"), QString::fromStdString(device_id_));
  obj.insert(QStringLiteral("name"), QString::fromStdString(device_name_));
  obj.insert(QStringLiteral("tcp_port"), static_cast<int>(tcp_port_));
  if (!account_.empty()) {
    // T4.2：登录端携带账号标识——仅作对端显示与跨态判定，不参与路由
    obj.insert(QStringLiteral("account"), QString::fromStdString(account_));
  }
  obj.insert(QStringLiteral("ts"),
             QDateTime::currentMSecsSinceEpoch());

  const QByteArray datagram = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  socket_.writeDatagram(datagram, QHostAddress::Broadcast, opts_.port);
}

void DiscoveryService::on_ready_read() {
  while (socket_.hasPendingDatagrams()) {
    const QNetworkDatagram dgram = socket_.receiveDatagram();
    const QByteArray data = dgram.data();

    // 畸形防御：长度与 JSON 校验失败直接丢弃
    if (data.isEmpty() || data.size() > 2048) continue;
    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) continue;

    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("magic")).toString() !=
        QLatin1String(kDiscoveryMagic)) {
      continue;
    }
    if (obj.value(QStringLiteral("v")).toInt() != kDiscoveryVersion) continue;

    const std::string device_id =
        obj.value(QStringLiteral("device_id")).toString().toStdString();
    if (device_id.empty() || device_id == device_id_) continue; // 忽略自己

    Peer peer;
    peer.device_id = device_id;
    peer.name = obj.value(QStringLiteral("name")).toString().toStdString();
    peer.tcp_port = static_cast<quint16>(
        qBound(0, obj.value(QStringLiteral("tcp_port")).toInt(), 65535));
    peer.address = dgram.senderAddress();
    peer.last_seen_ms =
        static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    peer.account = obj.value(QStringLiteral("account")).toString().toStdString();

    const auto it = peers_.find(device_id);
    if (it == peers_.end()) {
      peers_.emplace(device_id, peer);
      emit peerJoined(peer);
    } else {
      const bool account_changed = it->second.account != peer.account;
      it->second = peer;
      if (account_changed) emit peerUpdated(peer); // 登录态变化（T4.2 跨态判定）
    }
  }
}

void DiscoveryService::sweep() {
  if (!running_) return;
  const auto now = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
  const auto timeout = static_cast<quint64>(opts_.peer_timeout_ms);
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (now - it->second.last_seen_ms > timeout) {
      const std::string id = it->first;
      it = peers_.erase(it);
      emit peerLeft(id);
    } else {
      ++it;
    }
  }
}

} // namespace memex::client
