// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：消息检索与审计域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— 管理台归档检索（desktop-search）三面 ——
  // 撤回事件行：「撤回事件」单独可查面（撤回人＋时刻）。
  struct RecallEventRow {
    std::string msg_id;
    std::string by_account;
    std::int64_t ts_ms{0};
  };
  // 最近一次撤回事件（对账主证=message_events 的 recalled 事件，recall_events
  // 为对账副本——先查副本、缺行回落事件表）；无撤回返回 nullopt。
  std::optional<RecallEventRow> last_recall_event(const std::string& msg_id);
  // 命中消息的会话上下文（「查看上下文」面）：按命中行定会话键（群=to 群键；
  // 单聊=双方成对），取该行 id 前后各 span 行升序。命中行不存在返回空。
  std::vector<ArchivedMessage> context_around(const std::string& msg_id,
                                              int span);
