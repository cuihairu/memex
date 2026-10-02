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
  std::string result;      // ok／bad_password／no_account
  std::int64_t ts_ms{0};
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

  // 登录记录：成功失败都记（result 区分）；全量可查（按账号过滤，空=全部）。
  bool add_login_record(const LoginRecord& rec);
  std::vector<LoginRecord> login_records(const std::string& account,
                                         int limit = 200);

  // 离线消息队列（投递语义：先入队，在线即投，接收方 ACK(msg_id) 后删除；
  // 未 ACK 前重复投递无害——接收端按 msg_id 去重）。归档库（T2.3）另行落表。
  bool queue_offline(const std::string& msg_id, const std::string& to_account,
                     const std::string& envelope_blob);
  std::vector<std::string> pending_offline(const std::string& account);
  bool ack_offline(const std::string& msg_id);
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

private:
  bool ensure_schema();

  sqlite3* db_{nullptr};
};

} // namespace memex::server
