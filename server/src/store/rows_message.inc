// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：归档消息/检索/审计行；由 store.hpp 按位包含，无包含卫、勿单独编译。
// 已归档消息（T2.3／T2.5：全量落库、撤回只置标记、按账号检索面）
struct ArchivedMessage {
  std::string msg_id;
  std::string from_account;
  std::string to_account;
  int type{0};
  std::string text;
  std::int64_t ts_ms{0};
  bool recalled{false};
  // 模糊检索相关度（仅关键词查询时非 0；无关键词=0，不参与排序）：
  // Σ(命中次数×1000 + 首命中位置提前量封顶 999)。见 search_messages。
  std::int64_t score{0};
  // messages 自增主键（keyset 分页游标；默认 0＝附加字段，既有消费方不受影响）
  std::int64_t row_id{0};
};

// 群成员明细（群组-2 群记录导出「参与人」段）：成员＋角色＋加入时间。

//（test_nudge 等四处因此踩过 std::string(int) 的 null 构造崩溃）。
struct MessageSearch {
  std::string account;  // 该账号收发两侧都命中；空=全部
  // 正文包含（子串，模糊）；空=不过滤。支持多关键词：按空白与逗号分词，
  // 每词独立子串命中并 AND 组合（全部命中才出）。英文大小写不敏感、中文
  // 按字符直接匹配；命中行按相关度倒序、同分按时间倒序（相关度优先）。
  std::string keyword;
  std::int64_t since_ms{0}; // 起始时间（含），0=不限
  std::int64_t until_ms{0}; // 截止时间（含），0=不限
  int limit{200};
  // 会话维度（需求批⑧）：对端账号（单聊双向）或群键 "group:N"；空=不限。
  // account 非空为前提（群靠成员关系联入；单聊叠加为「我与该对端」双向）。
  // 新代码请用字段名赋值，勿依赖位置。
  std::string peer;
  // 排序方向：默认 false＝倒序（CLI/管理台/会话历史现行）；true＝升序（群组-2
  // 导出的分页序，升序键序＝对话体自然序）。
  bool asc{false};
  // keyset 游标（群组-2）：仅 asc 生效——AND id > ?（messages 自增主键
  // 单调≈时序，无重无漏）；0＝不过滤。倒序分页不游标（默认分支不动）。
  std::int64_t after_id{0};
};

// 查阅日志（T3.2）：每次检索／导出都落一条——谁、何时、用了什么条件、命中几条。
struct AuditReadRow {
  std::int64_t id{0};
  std::string op_account; // 操作者（CLI 侧取系统用户）
  std::string action;     // 检索／导出
  std::string filters;    // 过滤条件摘要（不含消息内容）
  int result_count{0};
  std::int64_t ts_ms{0};
};

// 策略开关（T3.4）：按部门配置、全局兜底。department_path 空串=全局行；
// 未配置的部门继承全局。resolve 时按成员部门逐级向上找不到再看全局。

struct ReadRow {
  std::string msg_id;
  std::string reader; // 已读方账号
  std::int64_t read_ms{0}; // 服务端受理时刻
};
