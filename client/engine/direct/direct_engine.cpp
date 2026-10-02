#include "direct_engine.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QSettings>
#include <QSysInfo>
#include <QUuid>

namespace memex::client {

DirectEngine::DirectEngine() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  auto id = settings.value(QStringLiteral("direct/device_id")).toString();
  if (id.isEmpty()) {
    id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    settings.setValue(QStringLiteral("direct/device_id"), id);
  }
  device_id_ = id.toStdString();
  device_name_ = QSysInfo::machineHostName().toStdString();
}

DirectEngine::~DirectEngine() { stop(); }

bool DirectEngine::start() {
  if (discovery_ && discovery_->running()) return true;
  discovery_ = std::make_unique<DiscoveryService>(device_id_, device_name_);
  if (!discovery_->start()) {
    discovery_.reset();
    qWarning() << "[直连引擎] 启动失败：发现服务未绑定";
    return false;
  }
  return true;
}

void DirectEngine::stop() {
  if (discovery_) discovery_->stop();
}

bool DirectEngine::running() const { return discovery_ && discovery_->running(); }

QList<Peer> DirectEngine::peers() const { return discovery_ ? discovery_->peers() : QList<Peer>{}; }

std::string DirectEngine::status_text() const {
  if (!running()) return "直连态 · 停止";
  const auto n = peers().size();
  if (n == 0) return "直连态 · 运行中（未发现设备）";
  return "直连态 · " + std::to_string(n) + " 台在线";
}

} // namespace memex::client
