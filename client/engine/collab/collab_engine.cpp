#include "collab_engine.hpp"

#include <QDebug>

namespace memex::client {

void CollabEngine::start() {
  if (running_) return;
  running_ = true;
  qDebug() << "[协作引擎] 启动（骨架）";
}

void CollabEngine::stop() {
  if (!running_) return;
  running_ = false;
  qDebug() << "[协作引擎] 停止";
}

std::string CollabEngine::status_text() const {
  return running_ ? "协作态 · 运行中" : "协作态 · 未登录";
}

} // namespace memex::client
