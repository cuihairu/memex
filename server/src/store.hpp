// 服务端本地库（SQLite）：账号表、登录记录表与离线消息队列。
// 归档消息表在 T2.3 接入同一库文件；本层只做存储，不含业务策略。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

typedef struct sqlite3 sqlite3;

namespace memex::server {

struct AccountRow {
  std::string account;
  std::string display_name;
  std::string salt_hex;
  std::string digest_hex;
  std::string role; // admin／member（权限分级，T3.1）
};

struct LoginRecord {
  std::int64_t id{0};
  std::string account;
  std::string fingerprint; // 设备指纹（SHA-256 hex）
  std::string kind;        // 设备类型（desktop／mobile）
  std::string name;        // 设备名（主机名）
  std::string source_ip;   // 来源地址
  std::string version;     // 客户端版本
  std::string result;      // ok／bad_password／no_account／device_disabled
  std::int64_t ts_ms{0};
};

// 设备台账（T3.3）：设备首次成功登录自动建档；启停对登录即时生效
//（停用＝拒绝登录，解绑＝清责任人并停用）。
struct DeviceRow {
  std::string fingerprint;
  std::string kind;
  std::string name;
  std::string owner_account; // 责任人（管理端登记；空=未登记）
  bool enabled{true};
  std::int64_t first_seen_ms{0};
  std::int64_t last_seen_ms{0};
};

// 已归档消息（T2.3／T2.5：全量落库、撤回只置标记、按账号检索面）
struct ArchivedMessage {
  std::string msg_id;
  std::string from_account;
  std::string to_account;
  int type{0};
  std::string text;
  std::int64_t ts_ms{0};
  bool recalled{false};
};

// 成员资料（T2.6 组织架构）：直属上级为独立字段——单列存储，
// 每人至多一名上级（结构性约束），链路逐级上溯由 manager_chain 完成。
struct MemberProfile {
  std::string account;
  std::string display_name;
  std::string department_path; // 全路径（"公司/研发部/客户端组"），空=未分配
  std::string title;           // 职务
  std::string manager;         // 直属上级账号（空=无）
  std::string role;            // admin／member（T3.1 角色分级）
};

// 批量导入行（CSV 解析后）与结果（错误行校验拒绝并报告行号）
struct OrgImportRow {
  int line_no{0};
  std::string account;
  std::string dept;    // 部门全路径（空=不分配）
  std::string title;   // 职务（空=无）
  std::string manager; // 直属上级账号（空=无上级）
};

struct OrgImportResult {
  int imported{0};
  std::vector<std::string> errors; // 行号 + 拒绝原因
};

// 检索条件（T3.2）：全部字段可空——空=不过滤；组合为 AND。
struct MessageSearch {
  std::string account;  // 该账号收发两侧都命中；空=全部
  std::string keyword;  // 正文包含（子串）；空=不过滤
  std::int64_t since_ms{0}; // 起始时间（含），0=不限
  std::int64_t until_ms{0}; // 截止时间（含），0=不限
  int limit{200};
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
struct PolicyRow {
  std::string department_path; // 空=全局默认
  bool allow_anonymous{true};
  bool allow_cross_state{true};
  bool new_device_approval{false};
};

// 群聊（T4.1）：群消息走既有 TEXT（to="group:<群号>"），
// 归档 to_account 即 "group:<群号>"，按成员账号检索时一并联入。
struct GroupInfo {
  std::uint64_t group_id{0};
  std::string name;
  std::string owner;
  std::string announcement;        // 空=未设
  std::vector<std::string> members; // 含群主
};

// 跨态会话日志（T4.2）：已登录端上报与未登录设备的会话——时间/双方/时长，
// 不含消息内容。ended_ms=0 表示进行中。
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
struct ReadRow {
  std::string msg_id;
  std::string reader; // 已读方账号
  std::int64_t read_ms{0}; // 服务端受理时刻
};

class ServerStore {
public:
  ServerStore() = default;
  ~ServerStore();

  ServerStore(const ServerStore&) = delete;
  ServerStore& operator=(const ServerStore&) = delete;

  // 打开（不存在则建库建表）。目录须已存在（部署口径：数据目录由运维创建）。
  bool open(const std::string& path);
  void close();
  bool is_open() const { return db_ != nullptr; }

  // 建账号：成功 true；账号已存在 false（幂等拒绝，不覆盖）。
  // role：admin／member（默认 member）。
  bool create_account(const std::string& account, const std::string& password,
                      const std::string& display_name,
                      const std::string& role = "member");

  std::optional<AccountRow> find_account(const std::string& account);

  // 全部账号（账号、展示名、角色）——管理后台成员维护面（T3.1）
  std::vector<std::pair<std::string, std::string>> account_list();

  // 登录记录：成功失败都记（result 区分）；全量可查（按账号／设备指纹前缀
  // 过滤，空=全部）。
  bool add_login_record(const LoginRecord& rec);
  std::vector<LoginRecord> login_records(const std::string& account,
                                         const std::string& fp_prefix = "",
                                         int limit = 200);

  // —— T3.3 设备台账 ——
  // 首次成功登录建档、再次登录刷新 last_seen（台账随使用自动生长）。
  bool upsert_device(const std::string& fingerprint, const std::string& kind,
                     const std::string& name, std::int64_t ts_ms);
  std::optional<DeviceRow> find_device(const std::string& fingerprint);
  std::vector<DeviceRow> device_list(); // 按最近活跃倒序
  // 指纹前缀定位（CLI 易用）：前缀≥8 位；返回 {完整指纹, 是否多义}。
  std::pair<std::string, bool> device_by_prefix(const std::string& prefix);
  // 责任人登记（账号须已存在）；启停；解绑（清责任人并停用）。
  bool set_device_owner(const std::string& fingerprint,
                        const std::string& owner_account);
  bool set_device_enabled(const std::string& fingerprint, bool enabled);
  bool unbind_device(const std::string& fingerprint);

  // 离线消息队列（投递语义：先入队，在线即投，接收方 ACK(msg_id) 后删除；
  // 未 ACK 前重复投递无害——接收端按 msg_id 去重）。归档库（T2.3）另行落表。
  bool queue_offline(const std::string& msg_id, const std::string& to_account,
                     const std::string& envelope_blob);
  std::vector<std::string> pending_offline(const std::string& account);
  // 接收方回执清队列（按接收方清：群扇出时同一条消息对每名成员各一行）
  bool ack_offline(const std::string& msg_id, const std::string& account);
  std::size_t offline_count(const std::string& account);

  // T2.3 消息归档：协作态消息全量落库（msg_id 唯一；重复消息忽略）。
  bool store_message(const std::string& msg_id, const std::string& from_account,
                     const std::string& to_account, int type,
                     const std::string& text, std::int64_t ts_ms);
  // 检索已归档消息（倒序）：账号为空=全部；非空=该账号收发的都算
  // （管理员检索面，T2.5 补传验收经 CLI 同口径查询）。
  std::vector<ArchivedMessage> messages(const std::string& account,
                                        int limit = 200);
  // T3.2 条件检索：按人（收发双侧）／时间窗（含端点）／关键词（子串），
  // 条件 AND 组合、倒序、限量。撤回消息照常命中（原文保留、带标记）。
  std::vector<ArchivedMessage> search_messages(const MessageSearch& q);

  // 查阅留痕：检索／导出动作逐次落日志（只附加，不删改）。
  bool add_audit_read(const AuditReadRow& rec);
  std::vector<AuditReadRow> audit_reads(int limit = 100);
  // 撤回仅置标记，正文不清（留痕纪律）。
  bool recall_message(const std::string& msg_id);
  bool is_recalled(const std::string& msg_id);
  // 某消息发送方（撤回权限判定）；不存在返回空串。
  std::string message_from(const std::string& msg_id);
  // 撤回事件独立留痕（只附加、不删改）。
  bool record_recall_event(const std::string& msg_id,
                           const std::string& by_account,
                           std::int64_t ts_ms);
  std::size_t recall_event_count(const std::string& msg_id);

  // —— T2.6 组织架构 ——

  // 部门：按全路径逐级创建（已存在即复用）；返回末级部门 id，失败 -1。
  int ensure_department_path(const std::string& path);
  // 全部部门（id → 全路径，按路径字典序）
  std::vector<std::pair<int, std::string>> department_list();
  // 部门 id 反查全路径；不存在返回空串
  std::string department_path(int id);

  // 成员资料建档／更新。校验拒绝（返回 false）：账号不存在、
  // manager==account（自为上级）、manager 会与现有链路构成环。
  bool set_member_profile(const std::string& account, int department_id,
                          const std::string& title,
                          const std::string& manager);
  // 成员详情（含部门全路径与直属上级）；未建档返回 nullopt
  std::optional<MemberProfile> member_profile(const std::string& account);
  // 全部成员（账号全量 LEFT JOIN 资料；未建档成员部门/职务为空）——
  // 组织架构下发表（T3.1 ORG_DATA 数据源）
  std::vector<MemberProfile> member_list();
  // 直属上级链路逐级上溯（不含本人，最近上级在前）；含环防御，遇环即止
  std::vector<std::string> manager_chain(const std::string& account);

  // 批量导入：逐行校验（账号存在、非自身、不成环），坏行拒绝并报告行号，
  // 好行入库（单事务，坏行逐行回滚不影响好行）。
  OrgImportResult import_members(const std::vector<OrgImportRow>& rows);

  // —— T3.4 策略开关 ——
  // 配置一行策略（部门路径空=全局；部门不存在拒绝）。
  bool set_policy(const std::string& department_path, bool allow_anonymous,
                  bool allow_cross_state, bool new_device_approval);
  // 全部已配置行（含全局行；部门行按路径字典序）
  std::vector<PolicyRow> policy_list();
  // 按账号解析生效策略：本人部门 → 逐级上级部门 → 全局 → 内置默认
  //（未配置任何行时：允许免登录、允许跨态、新设备免审批）。
  PolicyRow resolve_policy(const std::string& account);

  // —— T4.1 群聊 ——
  // 建群：群主（owner）自动入群；members 须全部为已建账号（含去重）。
  // 返回群号（>0）；任何成员账号不存在返回 0。
  std::uint64_t create_group(const std::string& name,
                             const std::string& owner,
                             const std::vector<std::string>& members);
  // 群详情；不存在返回 nullopt
  std::optional<GroupInfo> group_info(std::uint64_t group_id);
  // 某账号加入的全部群
  std::vector<GroupInfo> groups_of(const std::string& account);
  // 是否群成员
  bool is_group_member(std::uint64_t group_id, const std::string& account);
  // 拉人（成员须为已建账号且不在群里）
  bool group_invite(std::uint64_t group_id, const std::string& account);
  // 退群（群主退群=解散：删成员表记录；群号与归档保留）
  bool group_leave(std::uint64_t group_id, const std::string& account);
  // 群公告（仅群主可设；空串=清除）
  bool group_announce(std::uint64_t group_id, const std::string& owner,
                      const std::string& announcement);
  // 群成员账号列表（群不存在返回空）
  std::vector<std::string> group_members(std::uint64_t group_id);

  // —— T4.2 跨态会话 ——
  // 建立（start）：插入一行进行中记录（同会话重复 start 只记首条）。
  bool cross_log_start(const std::string& account,
                       const std::string& peer_device,
                       const std::string& peer_name, std::int64_t started_ms);
  // 结束（end）：按 (账号, 对端, 建立时刻) 闭环最早一条未结束记录，算时长。
  bool cross_log_end(const std::string& account,
                     const std::string& peer_device, std::int64_t started_ms,
                     std::int64_t ended_ms);
  // 全量日志（倒序；进行中 ended_ms=0、duration_ms=0）
  std::vector<CrossLogRow> cross_logs(int limit = 200);
  // 归档起点（A8）：首条归档消息之前最近一次成功登录时刻——即该账号
  // 进入协作态、归档开始的时刻；无归档返回 0；有归档但无登录记录
  //（旧库数据）退化为首条归档消息时刻。
  std::int64_t archive_start_ms(const std::string& account);

  // —— T4.3 已读回执 ——
  // 已读上报：仅归档库存在的 msg_id 才留痕（防伪造 msg_id 灌库）；
  // 同 (msg_id, 已读方) 重复上报幂等（首条为准，返回 true）。
  bool record_read(const std::string& msg_id, const std::string& reader,
                   std::int64_t read_ms);
  // 某条消息的全部已读行（按上报序）
  std::vector<ReadRow> readers_for(const std::string& msg_id);

private:
  bool ensure_schema();

  sqlite3* db_{nullptr};
};

} // namespace memex::server
