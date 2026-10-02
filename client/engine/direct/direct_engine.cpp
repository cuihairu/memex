#include "direct_engine.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>

namespace memex::client {

DirectEngine::DirectEngine(const std::string& device_id, const QString& db_path,
                           QObject* parent)
    : QObject(parent) {
  if (!device_id.empty()) {
    device_id_ = device_id;
  } else {
    QSettings settings(QCoreApplication::organizationName(),
                       QCoreApplication::applicationName());
    auto id = settings.value(QStringLiteral("direct/device_id")).toString();
    if (id.isEmpty()) {
      id = QUuid::createUuid().toString(QUuid::WithoutBraces);
      settings.setValue(QStringLiteral("direct/device_id"), id);
    }
    device_id_ = id.toStdString();
  }
  device_name_ = QSysInfo::machineHostName().toStdString();

  db_path_ = db_path;
  if (db_path_.isEmpty()) {
    const QString data_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    db_path_ = data_dir + QStringLiteral("/memex-local.db");
  }
}

DirectEngine::~DirectEngine() { stop(); }

bool DirectEngine::start() {
  if (running_) return true;

  store_ = std::make_unique<LocalStore>();
  if (!store_->open(db_path_)) {
    qWarning() << "[直连引擎] 本地库打开失败：" << db_path_;
    store_.reset();
    return false;
  }

  transport_ = std::make_unique<DirectTransport>();
  transport_->set_device_id(device_id_);
  if (!transport_->listen()) {
    qWarning() << "[直连引擎] TCP 监听失败";
    transport_.reset();
    store_.reset();
    return false;
  }

  discovery_ = std::make_unique<DiscoveryService>(device_id_, device_name_);
  discovery_->set_tcp_port(transport_->port());
  if (!discovery_->start()) {
    discovery_.reset();
    transport_->stop();
    transport_.reset();
    store_.reset();
    return false;
  }

  connect(discovery_.get(), &DiscoveryService::peerJoined, this,
          [this](const Peer&) { emit peers_changed(); });
  connect(discovery_.get(), &DiscoveryService::peerLeft, this,
          [this](const std::string&) { emit peers_changed(); });

  connect(transport_.get(), &DirectTransport::text_received, this,
          [this](const QString& from_id, const QString& /*to_id*/, quint64 seq,
                 qint64 ts_ms, const QString& text) {
            // 本地落库：来源＝直连（仅本机，不入服务端归档）
            StoredMessage m;
            m.peer = from_id.toStdString();
            m.from = from_id.toStdString();
            m.to = device_id_;
            m.seq = seq;
            m.ts_ms = ts_ms;
            m.text = text.toStdString();
            m.source = "direct";
            store_->append(m);
            emit message_received(from_id, text, ts_ms);
          });

  connect(transport_.get(), &DirectTransport::delivered, this,
          [this](quint64 seq, bool ok) { emit text_delivered(seq, ok); });

  running_ = true;
  qDebug().noquote() << QString::fromStdString("[直连引擎] 启动：设备 " + device_id_ +
                                               "，TCP " +
                                               std::to_string(transport_->port()));
  return true;
}

void DirectEngine::stop() {
  if (!running_) return;
  running_ = false;
  if (discovery_) discovery_->stop();
  if (transport_) transport_->stop();
  if (store_) store_->close();
  discovery_.reset();
  transport_.reset();
  store_.reset();
}

bool DirectEngine::running() const { return running_; }

QList<Peer> DirectEngine::peers() const {
  return discovery_ ? discovery_->peers() : QList<Peer>{};
}

Peer DirectEngine::peer(const std::string& device_id) const {
  const auto list = peers();
  for (const Peer& p : list) {
    if (p.device_id == device_id) return p;
  }
  return {};
}

bool DirectEngine::has_peer(const std::string& device_id) const {
  return !peer(device_id).device_id.empty();
}

quint64 DirectEngine::send_text(const std::string& peer_device_id,
                                const std::string& text) {
  if (!running_) return 0;
  const Peer target = peer(peer_device_id);
  if (target.device_id.empty() || target.tcp_port == 0) {
    qWarning() << "[直连引擎] 对端不可达："
               << QString::fromStdString(peer_device_id);
    return 0;
  }

  const std::uint64_t seq = ++seq_counter_;
  const qint64 ts_ms = QDateTime::currentMSecsSinceEpoch();

  // 发送方本地落库（送达失败仍保留本机记录，与聊天界面语义一致）
  StoredMessage m;
  m.peer = peer_device_id;
  m.from = device_id_;
  m.to = peer_device_id;
  m.seq = seq;
  m.ts_ms = ts_ms;
  m.text = text;
  m.source = "direct";
  store_->append(m);

  transport_->send_text(target.address, target.tcp_port, peer_device_id, seq, text);
  return static_cast<quint64>(seq);
}

QList<StoredMessage> DirectEngine::history(const QString& peer, int limit) const {
  return store_ ? store_->history(peer, limit) : QList<StoredMessage>{};
}

std::string DirectEngine::status_text() const {
  if (!running_) return "直连态 · 停止";
  const auto n = peers().size();
  if (n == 0) return "直连态 · 运行中（未发现设备）";
  return "直连态 · " + std::to_string(n) + " 台在线";
}

} // namespace memex::client
