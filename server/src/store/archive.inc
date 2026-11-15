// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：归档事件溯源与留存域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— 平台-4 归档事件溯源（Event Sourcing）：Message 原始行 immutable
  //     （正文/撤回永不改写历史），状态变迁走 message_events append-only
  //     （created/delivered/read/recalled/edited）；messages.recall/text
  //     列＝事件物化投影（由追加函数同事务同步）；当前态可由事件重放
  //     重建（rebuild_messages_from_events）。——
  struct MessageEvent {
    std::int64_t id{0};
    std::string msg_id;
    std::string event;      // created／delivered／read／recalled／edited
    std::string by_account; // 触发者（created=发送方；delivered/read=接收方）
    std::string payload;    // created=全量字段 JSON；edited=新正文；其余空
    std::int64_t ts_ms{0};
  };
  // 追加事件（append-only，无更新/删除路径）。created 物化归档行
  //（等价 store_message）；recalled 物化 recall=1；edited 物化 text。
  // 幂等：created 对已有归档行不重放（重投去重）。
  bool append_message_event(const std::string& msg_id,
                            const std::string& event,
                            const std::string& by_account,
                            const std::string& payload, std::int64_t ts_ms);
  // 某消息的事件序列（按发生序）；msg_id 空=全部事件（重投演练用）
  std::vector<MessageEvent> message_events(const std::string& msg_id);
  // 送达回执幂等判据（需求批⑦）：该 msg_id 是否已记过 by_account 的
  // delivered 事件——重复 ACK 只记一次、只推一次 DELIVER_NOTICE。
  bool delivered_recorded(const std::string& msg_id,
                          const std::string& by_account);
  // 重放事件重建当前态（created 建 行、recalled 置标记、edited 替正文）
  std::vector<ArchivedMessage> rebuild_messages_from_events();

  // —— 归档完整性账（评审四·远期加固）：message_events 事件流的哈希链
  //     清单——不引入区块链，分块封账＋链式摘要：事件落库满
  //     kArchiveChainBlockEvents 条即同事务封一块，`archive seal` 定期
  //     收尾。DB 级管理员可改库（append-only 表名≠不可篡改）——校验链
  //     断裂即可信性断言：改/删/插任一已封事件行都使逐块重放对不上账。
  static constexpr int kArchiveChainBlockEvents = 64;
  struct ArchiveChainBlock {
    std::int64_t id{0};
    std::int64_t first_event_id{0}; // 本块首事件 id（连续 id 段）
    std::int64_t last_event_id{0};
    std::int64_t event_count{0};
    std::string prev_hash;  // 前块 chain_hash（首块=64 个 '0'）
    std::string chain_hash; // 本块链式摘要（hex；改任一成员行即断）
    std::int64_t sealed_ms{0};
  };
  // 校验结论：ok=账本与事件流逐块对上；broken_at=首个断点描述（空=完好）
  struct ArchiveChainVerdict {
    bool ok{false};
    std::string broken_at;
    std::int64_t blocks{0};    // 已封块数
    std::int64_t open_tail{0}; // 未封尾事件条数（正常态，待 seal）
    std::int64_t events{0};    // 事件总数
  };
  // 封账：自上块之后的事件封成一块（≤kArchiveChainBlockEvents 条；
  // 无新事件=true 空账不算错）。事件落库满块时 server 同事务自动封，
  // 运维可另 `archive seal` 定期收尾（日报/巡检节奏）。
  bool archive_chain_seal();
  // 账本列表（id 序）
  std::vector<ArchiveChainBlock> archive_chain_list();
  // 校验：重放事件流折叠链头并与账本逐块对账（块边界连续性＋计数＋
  // 链式摘要＋prev 链接＋账本行自身完整性）；未封尾如实报告不判错。
  ArchiveChainVerdict archive_chain_verify();

  // —— 平台-5 留存策略（Retention Policy）：废除「永久留存」一刀切——
  //     生命周期由管理员配置（30/180/365/1095 天或 0=Indefinite）；
  //     删除=Retention Purge（who/when/what/why/policy/approval 台账，
  //     双人审批高风险），逐条落 purged 事件（历史无痕删除被禁止）——
  struct RetentionPolicy {
    std::int64_t id{0};
    std::string department_path; // 空=全局行；部门未配置时继承全局
    int retention_days{0};       // 0=Indefinite
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 写入一行（UPSERT，按部门路径）；days 须为 0 或 30/180/365/1095
  bool retention_set(int days, const std::string& department_path,
                     const std::string& updated_by, std::int64_t ts_ms);
  std::vector<RetentionPolicy> retention_list();
  // 生效留存期：本人部门链逐级上溯 → 全局行 → 内置默认 Indefinite
  RetentionPolicy retention_resolve(const std::string& account);
  // Retention Purge：双人审批（approved_by≠purged_by、两账号须存在）、
  // 理由必填；物理清除 before_ms 之前的归档行，逐条落 purged 事件
  //（重建可重现清除事实），批次台账＋审计留痕。返回台账 id；0=拒。
  struct RetentionPurge {
    std::int64_t id{0};
    std::string purged_by;
    std::string approved_by;
    std::string reason;
    int policy_days{0};
    int msg_count{0};
    std::int64_t before_ms{0};
    std::int64_t purged_ms{0};
  };
  std::int64_t retention_purge(std::int64_t before_ms,
                               const std::string& reason,
                               const std::string& purged_by,
                               const std::string& approved_by,
                               int policy_days, std::int64_t ts_ms);
  std::vector<RetentionPurge> retention_purges(int limit = 50);
