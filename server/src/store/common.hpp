// store 拆分模块的内部共享小工具；对外 API 仍只在 store.hpp
#pragma once

#include <chrono>

namespace memex::server {

inline std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace memex::server
