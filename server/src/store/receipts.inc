// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：已读回执域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— T4.3 已读回执 ——
  // 已读上报：仅归档库存在的 msg_id 才留痕（防伪造 msg_id 灌库）；
  // 同 (msg_id, 已读方) 重复上报幂等（首条为准，返回 true）。
  bool record_read(const std::string& msg_id, const std::string& reader,
                   std::int64_t read_ms);
  // 某条消息的全部已读行（按上报序）
  std::vector<ReadRow> readers_for(const std::string& msg_id);
