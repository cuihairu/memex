// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：跨态日志行；由 store.hpp 按位包含，无包含卫、勿单独编译。

struct CrossLogRow {
  std::int64_t id{0};
  std::string account;     // 已登录端账号（上报方）
  std::string peer_device; // 对端设备标识
  std::string peer_name;   // 对端设备名
  std::int64_t started_ms{0};
  std::int64_t ended_ms{0};
  std::int64_t duration_ms{0}; // 结束后 = ended - started；进行中为 0
};

// 已读回执（T4.3）：接收方上报已读，服务端留痕（msg_id, 已读方）。
// 发送方在线即推送 READ_NOTICE；离线则仅留痕（上线后查库可见）。
