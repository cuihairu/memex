#include "direct_engine.hpp"

#include <QDebug>

namespace memex::client {

void DirectEngine::start() {
  if (running_) return;
  running_ = true;
  qDebug() << "[直连引擎] 启动（骨架）";
}

void DirectEngine::stop() {
  if (!running_) return;
  running_ = false;
  qDebug() << "[直连引擎] 停止";
}

std::string DirectEngine::status_text() const {
  return running_ ? "直连态 · 运行中" : "直连态 · 停止";
}

} // namespace memex::client
