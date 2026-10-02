// 直连引擎（T1.1）：未登录零配置直连态。
// 已实现 UDP 广播发现与在线表；点对点消息与文件按 todo T1.2／T1.3 接入。
#pragma once

#include <QList>
#include <QString>

#include <memory>
#include <string>

#include "discovery.hpp"

namespace memex::client {

class DirectEngine {
public:
  DirectEngine();
  ~DirectEngine();

  // 启动直连引擎：UDP 发现 + 后续直连接入（骨架阶段仅发现）。
  bool start();
  void stop();

  bool running() const;
  QList<Peer> peers() const;
  std::string status_text() const;

  // 本机设备标识：首次生成后持久化（QSettings）
  std::string device_id() const { return device_id_; }

private:
  std::string device_id_;
  std::string device_name_;
  std::unique_ptr<DiscoveryService> discovery_;
};

} // namespace memex::client
