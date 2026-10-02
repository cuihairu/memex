// 直连引擎（T0.4 骨架）：未登录零配置直连态。
// UDP 广播发现（2425）、点对点消息与文件按 todo 阶段 1 接入。
#pragma once

#include <string>

namespace memex::client {

class DirectEngine {
public:
  // 启动引擎（骨架仅记录状态，不开端口）。
  void start();
  void stop();

  bool running() const { return running_; }
  std::string status_text() const;

private:
  bool running_{false};
};

} // namespace memex::client
