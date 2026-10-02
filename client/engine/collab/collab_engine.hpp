// 协作引擎（T0.4 骨架）：登录后与协作服务端的长连接。
// 网关连接、消息同步、离线补传按 todo 阶段 2 接入。
#pragma once

#include <string>

namespace memex::client {

class CollabEngine {
public:
  void start();
  void stop();

  bool running() const { return running_; }
  std::string status_text() const;

private:
  bool running_{false};
};

} // namespace memex::client
