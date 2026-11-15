// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：群成员/群信息行；由 store.hpp 按位包含，无包含卫、勿单独编译。
// 离开时间现库不留痕（退群即摘行）——导出如实注记。
struct GroupMemberRow {
  std::string account;
  std::string role;      // owner／admin／member（group_role 口径）
  std::int64_t joined_ms{0};
};

// 成员资料（T2.6 组织架构）：直属上级为独立字段——单列存储，

// 归档 to_account 即 "group:<群号>"，按成员账号检索时一并联入。
struct GroupInfo {
  std::uint64_t group_id{0};
  std::string name;
  std::string owner;
  std::string announcement;        // 空=未设
  std::string topic;               // 群组-1 主题（空=未填＝展示层不显示该行）
  std::string purpose;             // 群组-1 目的（空=未填＝展示层不显示该块）
  std::int64_t created_ms{0};      // 群组-2 群记录导出「时间范围」段（建群时间）
  std::vector<std::string> members; // 含群主
};

// 跨态会话日志（T4.2）：已登录端上报与未登录设备的会话——时间/双方/时长，
// 不含消息内容。ended_ms=0 表示进行中。
