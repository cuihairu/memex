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

// 通讯录可见性（T4.6）：一行配置管一个目标——scope="member"（key=账号）或
// "dept"（key=部门全路径）。hidden＝对查看者隐藏；restrict_scope＝该部门成员
// 只看本部门（仅 dept 行有效）；hide_fields＝敏感字段脱敏（title,manager,role
// 逗号分隔，仅 member 行有效）。白名单例外走 VisibilityAllow。
struct VisibilityRow {
  std::string scope; // member | dept
  std::string key;   // 账号 或 部门全路径
  bool hidden{false};
  bool restrict_scope{false};
  std::string hide_fields;
};

// 白名单例外：viewer 可见 target——target=成员账号，或部门全路径（整树豁免）
struct VisibilityAllow {
  std::string viewer;
  std::string target;
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
// T4.5 常用联系人：星标置顶＋最近联系，落 SQLite 换机保留。
struct FavRow {
  std::string peer;
  bool starred{false};
  std::int64_t last_ms{0};
};

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

// webhook 接入台账（T4.10）：按群／个人独立——target="group:<群号>" 或账号。
// token 明文仅 create 时输出一次，库内存 sha256 摘要；revoked=1 即吊销拒收。
struct WebhookRow {
  std::int64_t id{0};
  std::string token_hash;
  std::string target;
  std::string name; // 备注（CLI --name，可空）
  std::int64_t created_ms{0};
  bool revoked{false};
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

  // —— 平台-2 Identity 模型补全 ——
  // Credential 独立：口令凭据迁出 accounts 行（建号双写、老库 open 时
  // 幂等迁移）；token/certificate/sso/device 类型随 schema 预留，当前
  // 只有 password 参与判登。
  struct CredentialRow {
    std::int64_t id{0};
    std::string account;
    std::string type; // password／token／certificate／sso／device
    std::string salt_hex;
    std::string digest_hex;
    std::int64_t created_ms{0};
    bool disabled{false};
  };
  std::optional<CredentialRow> find_credential(const std::string& account,
                                               const std::string& type);
  // IdentityBinding：外部身份（issuer+subject 唯一）↔ 本地账号。SSO 面
  // 未接前先立模型与 CLI 维护口（identity bind/unbind/list）。
  struct IdentityBinding {
    std::int64_t id{0};
    std::string account;
    std::string issuer;
    std::string subject;
    std::int64_t created_ms{0};
  };
  // 绑定（账号须存在；issuer+subject 已绑=0）
  std::int64_t identity_bind(const std::string& account,
                             const std::string& issuer,
                             const std::string& subject, std::int64_t ts_ms);
  bool identity_unbind(std::int64_t id);
  std::optional<IdentityBinding> identity_find(const std::string& issuer,
                                               const std::string& subject);
  std::vector<IdentityBinding> identity_list(const std::string& account = "");
  // Session 持久面：file 面令牌签发落行（device_id 预留空）、登出落
  // 理由；热路径裁决仍在内存（行随 expires_ms 过去自然失效，不过期
  // 不删——历史留痕面）。
  struct SessionRecord {
    std::string token_hash;
    std::string account;
    std::string scope; // internal／uplink
    std::string device_id;
    std::int64_t created_ms{0};
    std::int64_t expires_ms{0};
    std::int64_t logged_out_ms{0}; // 0=进行中
    std::string logout_reason;     // user_logout／…
  };
  bool session_insert(const SessionRecord& rec);
  bool session_close(const std::string& token_hash, const std::string& reason,
                     std::int64_t ts_ms);
  std::vector<SessionRecord> session_list(const std::string& account,
                                          int limit = 100);

 private:
  // 凭据行插入（create_account 内部腿）
  bool credential_insert(const std::string& account, const std::string& type,
                         const std::string& salt_hex,
                         const std::string& digest_hex, std::int64_t ts_ms);

 public:

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
  // 撤回仅置标记，正文不清（留痕纪律）；平台-4 起同笔落 recalled 事件
  //（消息不存在或越权由调用方判定，此处只认存在性）。
  bool recall_message(const std::string& msg_id, const std::string& by_account,
                      std::int64_t ts_ms);
  bool is_recalled(const std::string& msg_id);
  // 某消息发送方（撤回权限判定）；不存在返回空串。
  std::string message_from(const std::string& msg_id);
  // 撤回事件独立留痕（只附加、不删改）。
  bool record_recall_event(const std::string& msg_id,
                           const std::string& by_account,
                           std::int64_t ts_ms);
  std::size_t recall_event_count(const std::string& msg_id);

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
  // 重放事件重建当前态（created 建 行、recalled 置标记、edited 替正文）
  std::vector<ArchivedMessage> rebuild_messages_from_events();

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
  //（平台-3 起沿权威表 org_reporting_lines 走，档案行有无不影响链路）
  std::vector<std::string> manager_chain(const std::string& account);

  // 批量导入：逐行校验（账号存在、非自身、不成环），坏行拒绝并报告行号，
  // 好行入库（单事务，坏行逐行回滚不影响好行）。
  OrgImportResult import_members(const std::vector<OrgImportRow>& rows);

  // —— 平台-3 Organization 与授权拆开：Account 拆三关联（一人多部门/
  //     多角色/临时代理＝带时间窗的授权）。org_memberships 与
  //     org_reporting_lines 为权威表（member_profiles 的 department_id/
  //     manager 列作单值镜像，由写入函数同步维护——单值视图消费者
  //     member_profile 不改语义，manager_chain 沿权威表走）；基础角色
  //     仍居 accounts.role，org_role_assignments 承载追加角色授权。——
  struct OrgMembership {
    std::int64_t id{0};
    std::string account;
    int department_id{0};
    std::int64_t created_ms{0};
  };
  // 加部门关联（多对多；账号/部门须存在、重复=false）
  bool membership_add(const std::string& account, int department_id,
                      std::int64_t ts_ms);
  bool membership_remove(const std::string& account, int department_id);
  std::vector<OrgMembership> memberships_of(const std::string& account);
  struct RoleAssignment {
    std::int64_t id{0};
    std::string account;
    std::string role;
    std::string scope; // 空=全局；部门全路径=域内
    std::int64_t valid_from_ms{0};  // 0=即刻
    std::int64_t valid_until_ms{0}; // 0=无限期（临时代理=带终点）
    std::string granted_by;
    std::int64_t created_ms{0};
  };
  // 授权（时间窗倒置=0 拒；账号须存在）→ id
  std::int64_t role_grant(const std::string& account, const std::string& role,
                          const std::string& scope, std::int64_t valid_from_ms,
                          std::int64_t valid_until_ms,
                          const std::string& granted_by, std::int64_t ts_ms);
  bool role_revoke(std::int64_t id);
  // 生效角色并集：基础（accounts.role）∪ 窗内授权（from<=at<until，
  // 0 端点=不开窗；含 scope 过滤空=全收）
  std::vector<std::string> effective_roles(const std::string& account,
                                           std::int64_t at_ms);
  // 授权台账全量（含过期；账号空=全部）——管理面
  std::vector<RoleAssignment> role_assignments(const std::string& account = "");
  // 直属上级（权威表 org_reporting_lines；沿用每人至多一名；同步镜像）
  bool reporting_set(const std::string& account, const std::string& manager,
                     std::int64_t ts_ms);
  bool reporting_clear(const std::string& account);

  // —— T3.4 策略开关 ——
  // 配置一行策略（部门路径空=全局；部门不存在拒绝）。
  bool set_policy(const std::string& department_path, bool allow_anonymous,
                  bool allow_cross_state, bool new_device_approval);
  // 全部已配置行（含全局行；部门行按路径字典序）
  std::vector<PolicyRow> policy_list();
  // 按账号解析生效策略：本人部门 → 逐级上级部门 → 全局 → 内置默认
  //（未配置任何行时：允许免登录、允许跨态、新设备免审批）。
  PolicyRow resolve_policy(const std::string& account);

  // —— T4.6 通讯录可见性 ——
  // 写入一行（UPSERT，三项全量覆盖）。校验拒绝（返回 false）：scope 非
  // member/dept、member 的账号不存在、dept 的部门路径不存在。
  bool set_visibility(const std::string& scope, const std::string& key,
                      bool hidden, bool restrict_scope,
                      const std::string& hide_fields);
  // 单行查询（无配置返回 nullopt）；全部已配置行（member 在前，键字典序）
  std::optional<VisibilityRow> visibility_row(const std::string& scope,
                                              const std::string& key);
  std::vector<VisibilityRow> visibility_list();
  // 白名单例外增删（目标重复添加即幂等；查询返回全部行）
  bool add_visibility_allow(const std::string& viewer,
                            const std::string& target);
  bool remove_visibility_allow(const std::string& viewer,
                               const std::string& target);
  std::vector<VisibilityAllow> visibility_allows();

  // —— T4.10 webhook 接入 ——
  // 建台账行（token 明文由调用方持有，这里只落摘要）；返回自增 id（失败 0）。
  std::int64_t webhook_create(const std::string& token_hash,
                              const std::string& target,
                              const std::string& name, std::int64_t created_ms);
  // 按 token 摘要取有效行（吊销／不存在返回 nullopt——HTTP 401 的判定依据）。
  std::optional<WebhookRow> webhook_by_token(const std::string& token_hash);
  // 全部行（CLI list；吊销行也列，状态列区分）
  std::vector<WebhookRow> webhook_list();
  // 吊销（按 id；返回是否确有该行被置位）
  bool webhook_revoke(std::int64_t id);
  // ORG_DATA 下发视图（按查看者过滤与脱敏，T4.6）：
  //  ① 被隐藏成员／隐藏部门（整树）对查看者不可见——管理员、白名单与
  //    部门内自己人豁免；② 查看者所在部门若限看本部门，则只见本部门子树；
  //  ③ 敏感字段对非管理员、非白名单查看者脱敏；④ 查看者本人始终在列。
  std::vector<MemberProfile> visible_members(const std::string& viewer);
  // 部门树下发表（与 visible_members 同规则：隐藏部门与限看范围裁剪）
  std::vector<std::pair<int, std::string>> visible_departments(
      const std::string& viewer);

  // —— T4.1 群聊 ——
  // 建群：群主（owner）自动入群；members 须全部为已建账号（含去重）。
  // 返回群号（>0）；任何成员账号不存在返回 0。
  std::uint64_t create_group(const std::string& name,
                             const std::string& owner,
                             const std::vector<std::string>& members);
  // 群详情；不存在返回 nullopt
  std::optional<GroupInfo> group_info(std::uint64_t group_id);
  // 全量群列表（运维面：组织架构树/CLI group list）
  std::vector<GroupInfo> groups_list();
  // 某账号加入的全部群
  std::vector<GroupInfo> groups_of(const std::string& account);
  // 是否群成员
  bool is_group_member(std::uint64_t group_id, const std::string& account);
  // 拉人（成员须为已建账号且不在群里）
  bool group_invite(std::uint64_t group_id, const std::string& account);
  // 退群（群主退群=解散：删成员表记录；群号与归档保留）
  bool group_leave(std::uint64_t group_id, const std::string& account);
  // 群公告（R24-1：群主/管理员可设；空串=清除）。每次成功设置都落一条
  // 编辑历史（谁/何时/改成了什么——清除也落，content 空串即「清除」一笔）。
  bool group_announce(std::uint64_t group_id, const std::string& account,
                      const std::string& announcement);
  // 群公告编辑历史（R24-1：倒序，新者在前；只附加不删改）
  struct AnnouncementRevision {
    std::int64_t id{0};
    std::string editor;
    std::string content; // 该次设置后的全文（空串=该次为清除）
    std::int64_t ts_ms{0};
  };
  std::vector<AnnouncementRevision> announcement_history(
      std::uint64_t group_id, int limit = 100);
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

  // —— T4.5 常用联系人 ——
  // 星标／取消星标（UPSERT，仅改 starred，不刷新 last_ms）
  bool fav_star(const std::string& account, const std::string& peer,
                bool starred);
  // 最近联系刷新（UPSERT，last_ms 取 max；已有 starred 不动）
  bool fav_touch(const std::string& account, const std::string& peer,
                 std::int64_t ts_ms);
  // 全量列表：星标置顶（last_ms 新者先）、未星标按 last_ms 倒序、peer 兜底
  std::vector<FavRow> fav_list(const std::string& account);

  // —— T4.3 已读回执 ——
  // 已读上报：仅归档库存在的 msg_id 才留痕（防伪造 msg_id 灌库）；
  // 同 (msg_id, 已读方) 重复上报幂等（首条为准，返回 true）。
  bool record_read(const std::string& msg_id, const std::string& reader,
                   std::int64_t read_ms);
  // 某条消息的全部已读行（按上报序）
  std::vector<ReadRow> readers_for(const std::string& msg_id);


  // —— R23-1 文件存储元数据（服务端权限/配额判断层） ——
  // 文件来源：internal（内网上传）、uplink（外网单向上传）
  enum class FileSource : int { Internal = 0, Uplink = 1 };
  // 文件状态：normal、quarantine（隔离/杀毒待审）、expired（过期清理）
  enum class FileStatus : int { Normal = 0, Quarantine = 1, Expired = 2 };

  // 文件类目（R23-3）：Personal=常规个人/群空间；Inbox=文件助手本人收件箱
  //（手机发自己=文件传输落这里）。类目入秒传键：同属主同哈希的收件箱文件
  // 与个人空间文件是两个空间，互不秒传串用。
  enum class FileKind : int { Personal = 0, Inbox = 1 };

  struct FileMeta {
    int64_t id{0};
    std::string owner;
    std::string belong_gid;
    std::string belong_uid;
    std::string file_name;
    int64_t file_size{0};
    std::string file_hash;
    std::string object_key;
    FileSource source{FileSource::Internal};
    int64_t upload_ts{0};
    FileStatus status{FileStatus::Normal};
    bool pin{false};  // 置顶（群主/管理员管群文件、本人管个人文件）
    FileKind kind{FileKind::Personal};
  };

  struct QuotaInfo {
    std::string gid;
    std::string uid;
    int64_t used_bytes{0};
    int64_t limit_bytes{0};
  };

  int64_t create_file_meta(const FileMeta& meta);
  // 秒传按「属主+哈希+归属+类目」判（R23-2：换群/换人空间不算命中；
  // R23-3：收件箱与个人空间互不串用）。kind 缺省 Personal 兼容既有调用。
  std::optional<FileMeta> check_second_transfer(const std::string& owner,
                                                const std::string& file_hash,
                                                const std::string& belong_gid,
                                                const std::string& belong_uid,
                                                FileKind kind = FileKind::Personal);
  // kind=-1 不按类目过滤（缺省）；0/1 只列对应类目
  std::vector<FileMeta> list_files(const std::string& belong_gid,
                                   const std::string& belong_uid,
                                   int limit = 200, int offset = 0,
                                   int kind = -1);
  std::optional<FileMeta> file_by_id(int64_t file_id);
  bool delete_file_meta(int64_t file_id);
  bool set_file_pin(int64_t file_id, bool pin);
  bool set_file_status(int64_t file_id, FileStatus status);
  // 同一对象键仍被多少文件行引用（删除字节前判引用：内容寻址下多行可共用对象）
  int64_t count_file_refs(const std::string& object_key);
  std::optional<QuotaInfo> get_group_quota(const std::string& gid);
  std::optional<QuotaInfo> get_user_quota(const std::string& uid);
  bool add_group_quota_used(const std::string& gid, int64_t delta_bytes);
  bool add_user_quota_used(const std::string& uid, int64_t delta_bytes);
  // 受检扣费（原子）：limit=0 不限；used+delta 超限即拒（changes=0）。
  // 上传受理前用这个，退额（删除）用上面的 add_*（不设限）。
  bool charge_group_quota(const std::string& gid, int64_t delta_bytes);
  bool charge_user_quota(const std::string& uid, int64_t delta_bytes);
  bool set_group_quota_limit(const std::string& gid, int64_t limit_bytes);
  bool set_user_quota_limit(const std::string& uid, int64_t limit_bytes);

  struct UplinkLog {
    int64_t id{0};
    std::string uploader;
    std::string file_name;
    int64_t file_size{0};
    std::string file_hash;
    std::string object_key;
    int64_t upload_ts{0};
  };
  // 群内角色（权限模型：群主/管理员/成员）：""=非成员；owner 由 groups.owner 判
  std::string group_role(std::uint64_t group_id, const std::string& account);
  bool group_set_role(std::uint64_t group_id, const std::string& account,
                      const std::string& role); // member|admin（owner 行拒改）
  bool add_uplink_log(const UplinkLog& log);
  std::vector<UplinkLog> list_uplink_logs(const std::string& uploader,
                                          int limit = 200);
  // 外网 uplink 面的「我的上传」列表：只列 source=uplink 的收件箱文件
  //（/uplink/mine 数据源；uplink 面无任何读内网数据的端点，本查询只回
  // 本人自己的记录）。按 id 倒序（上传时间同源）。
  std::vector<FileMeta> list_uplink_files(const std::string& owner,
                                          int limit = 200, int offset = 0);

  // —— R23-3 文件助手备忘录（memos：本人文本，不入对象存储）——
  // 历史留痕不在本层：自备忘录不留修订史（修订历史属 R24-2 群备忘录）。
  struct MemoRow {
    std::int64_t id{0};
    std::string owner;
    std::string content;
    std::int64_t created_ms{0};
    std::int64_t updated_ms{0};
  };
  // 建备忘录：owner/content 非空才收（无主/空文不落库）；返回 id，拒=0
  std::int64_t create_memo(const std::string& owner,
                           const std::string& content, std::int64_t ts_ms);
  std::optional<MemoRow> memo_by_id(std::int64_t id);
  // 本人列表（updated_ms 倒序）——owner 过滤即权限（owner 之外查不到）
  std::vector<MemoRow> list_memos(const std::string& owner, int limit = 200,
                                  int offset = 0);
  // 更新/删除带 owner 二次校验（WHERE owner=）：异属主操作落不到行
  bool update_memo(std::int64_t id, const std::string& owner,
                   const std::string& content, std::int64_t ts_ms);
  bool delete_memo(std::int64_t id, const std::string& owner);

  // —— R24-2 群备忘录（群维度共享知识：标题+正文，修订历史可回滚）——
  // 与个人备忘录（memos，R23-3 文件助手）分表：空间不同权限不同，
  // 开放编辑开关决定成员能否越过管理员写（开放时编辑照旧逐笔留痕）。
  struct GroupMemo {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string title;
    std::string content;
    std::string author; // 建条目者
    std::int64_t created_ms{0};
    std::int64_t updated_ms{0};
  };
  // 修订笔（每次编辑后的全文快照；首建也落一笔，editor=作者）
  struct GroupMemoRevision {
    std::int64_t id{0};
    std::int64_t memo_id{0};
    std::string title;
    std::string content;
    std::string editor;
    std::int64_t ts_ms{0};
  };
  std::int64_t create_group_memo(std::uint64_t group_id,
                                 const std::string& title,
                                 const std::string& content,
                                 const std::string& author,
                                 std::int64_t ts_ms);
  std::optional<GroupMemo> group_memo_by_id(std::int64_t id);
  // 列表（updated_ms 倒序）；keyword 非空=标题或正文子串命中
  std::vector<GroupMemo> list_group_memos(std::uint64_t group_id,
                                          const std::string& keyword = "",
                                          int limit = 200, int offset = 0);
  // 更新：同事务落修订笔（editor=本次编辑者；回滚也走这里——回滚即一次编辑）
  bool update_group_memo(std::int64_t id, const std::string& title,
                         const std::string& content,
                         const std::string& editor, std::int64_t ts_ms);
  bool delete_group_memo(std::int64_t id, std::uint64_t group_id);
  std::vector<GroupMemoRevision> group_memo_history(std::int64_t memo_id);
  // 开放编辑开关（默认关=管理员维护；群主/管理员可设——判权在路由层）
  bool set_group_memo_open_edit(std::uint64_t group_id, bool open);
  bool group_memo_open_edit(std::uint64_t group_id);

  // —— R24-3 群密码箱（全程密文：服务端只存 b64 密文与包裹块）——
  // wingman 同款加密的存储面：客户端 PBKDF2(箱密码,salt)→KEK 解开
  // wrapped_dek 得 DEK 后本地解条目；服务端不碰明文、无解锁状态。
  // 授权名单：空=全成员可解锁（共享本意），群主可收窄；owner/admin 恒可。
  struct GroupVault {
    std::uint64_t group_id{0};
    std::string kdf_salt;    // b64（客户端派生 KEK 用）
    int kdf_iters{0};        // PBKDF2 迭代数
    std::string wrapped_dek; // b64：nonce(12)+ct(DEK 32)+tag(16) 打包
    std::int64_t created_ms{0};
    std::int64_t updated_ms{0};
  };
  // 条目：name/account_name 明文（掩码展示面），secret_* 密文（b64
  // GCM 密文打包 JSON{password,url,note}——设计「只露名称/账号」）
  struct GroupVaultEntry {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string name;
    std::string account_name;
    std::string secret_ct;
    std::string secret_nonce;
    std::string created_by;
    std::int64_t created_ms{0};
    std::int64_t updated_ms{0};
  };
  // 查看/复制留痕（谁/何时/哪条/何动作——审计进群日志）
  struct GroupVaultAudit {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::int64_t entry_id{0};
    std::string actor;
    std::string action; // reveal=查看 | copy=复制
    std::int64_t ts_ms{0};
  };
  // 建箱（已存在=false 调用方回 409）
  bool group_vault_init(std::uint64_t group_id, const std::string& kdf_salt,
                        int kdf_iters, const std::string& wrapped_dek,
                        std::int64_t ts_ms);
  // 重置/重包裹（覆盖包裹块；无箱=false 调用方回 404）
  bool group_vault_rekey(std::uint64_t group_id, const std::string& kdf_salt,
                         int kdf_iters, const std::string& wrapped_dek,
                         std::int64_t ts_ms);
  std::optional<GroupVault> group_vault_info(std::uint64_t group_id);
  std::int64_t vault_create_entry(std::uint64_t group_id,
                                  const std::string& name,
                                  const std::string& account_name,
                                  const std::string& secret_ct,
                                  const std::string& secret_nonce,
                                  const std::string& author,
                                  std::int64_t ts_ms);
  std::optional<GroupVaultEntry> vault_entry_by_id(std::int64_t id);
  bool vault_update_entry(std::int64_t id, const std::string& name,
                          const std::string& account_name,
                          const std::string& secret_ct,
                          const std::string& secret_nonce,
                          const std::string& editor, std::int64_t ts_ms);
  bool vault_delete_entry(std::int64_t id, std::uint64_t group_id);
  std::vector<GroupVaultEntry> vault_list_entries(std::uint64_t group_id);
  // 授权名单（空向量=清名单恢复全成员；事务替换）
  bool vault_set_acl(std::uint64_t group_id,
                     const std::vector<std::string>& accounts);
  std::vector<std::string> vault_acl_list(std::uint64_t group_id);
  void vault_audit_add(std::uint64_t group_id, std::int64_t entry_id,
                       const std::string& actor, const std::string& action,
                       std::int64_t ts_ms);
  std::vector<GroupVaultAudit> vault_audit_list(std::uint64_t group_id,
                                                int limit = 200);

  // —— R25-1 群工具框架（白名单动作＋入群即授权＋动作留痕＋代理骨架）——
  // 群=授权域：管理员给群配工具白名单动作；成员 call 判定在路由层叠加
  //（群成员 file:read 继承——入群即有、退群即失），无独立授权体系。
  struct GroupToolConfig {
    std::uint64_t group_id{0};
    std::string tool;
    std::string actions_json; // JSON 数组字符串（服务端不解释动作语义）
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 动作留痕：谁/何时/哪个工具/什么动作/参数/结果（铁律：点击可回溯到人）
  struct GroupToolAudit {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string tool;
    std::string action;
    std::string actor;
    std::string params_json;
    std::string result_json; // 代理调用回包（R25-1=骨架 stub 回显）
    std::int64_t ts_ms{0};
  };
  // 工具白名单动作配置（upsert；群须存在=false）
  bool tool_set_actions(std::uint64_t group_id, const std::string& tool,
                        const std::string& actions_json,
                        const std::string& updated_by, std::int64_t ts_ms);
  // 单工具配置（无行=未配置 nullopt）
  std::optional<GroupToolConfig> tool_config(std::uint64_t group_id,
                                             const std::string& tool);
  std::vector<GroupToolConfig> tool_list(std::uint64_t group_id);
  void tool_audit_add(std::uint64_t group_id, const std::string& tool,
                      const std::string& action, const std::string& actor,
                      const std::string& params_json,
                      const std::string& result_json, std::int64_t ts_ms);
  std::vector<GroupToolAudit> tool_audit_list(std::uint64_t group_id,
                                              int limit = 200);

  // —— R25-2 CI/CD 工具（首个落在 R25-1 框架上的具体工具）——
  // 流水线定义（群内按名唯一）；红绿灯=每流水线最近一笔 run 的 status
  struct CiPipeline {
    std::uint64_t group_id{0};
    std::string name;
    std::string description;
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 触发留痕：谁触发/何时/哪条流水线/结果（谁触发可回溯=设计点名）
  struct CiRun {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string pipeline;
    std::string actor;
    std::string status; // success/failed（stub 执行器即时出结果）
    std::string params_json;
    std::string result_json;
    std::int64_t ts_ms{0};
  };
  // 流水线 upsert/删（群须存在=false；删=删到行才真）
  bool ci_pipeline_upsert(std::uint64_t group_id, const std::string& name,
                          const std::string& description,
                          const std::string& updated_by, std::int64_t ts_ms);
  bool ci_pipeline_delete(std::uint64_t group_id, const std::string& name);
  std::vector<CiPipeline> ci_pipeline_list(std::uint64_t group_id);
  // run 落账（status 即终态：stub 执行器同步完成）并回 run id
  std::int64_t ci_run_add(std::uint64_t group_id, const std::string& pipeline,
                          const std::string& actor, const std::string& status,
                          const std::string& params_json,
                          const std::string& result_json, std::int64_t ts_ms);
  // run 历史 id DESC（pipeline 空=该群全部流水线）
  std::vector<CiRun> ci_run_list(std::uint64_t group_id,
                                 const std::string& pipeline = {},
                                 int limit = 50);
  // 红绿灯面：每条流水线最近一笔 run（无 run 的流水线不出现在此）
  std::vector<CiRun> ci_status_list(std::uint64_t group_id);

  // —— R25-3 打包工具（首批动作类示例之二）——
  // 产物台账（stub 阶段=打包结果记录；真产物字节面归后续批次）
  struct PackArtifact {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string name;
    std::string version;
    std::string note;
    std::string created_by;
    std::int64_t created_ms{0};
  };
  // 产物落账（同 gid+name+version 重复=覆盖 note/created_by）；群须存在
  bool pack_artifact_upsert(std::uint64_t group_id, const std::string& name,
                            const std::string& version,
                            const std::string& note,
                            const std::string& created_by,
                            std::int64_t ts_ms);
  std::vector<PackArtifact> pack_artifact_list(std::uint64_t group_id);
  bool pack_artifact_delete(std::uint64_t group_id, const std::string& name,
                            const std::string& version);

  // —— R25-4 凭据面：工具外部凭据只存服务端（密文=cred::gcm_seal 的
  // hex 串，行内永不见明文；取回只给掩码元数据——客户端零凭据）——
  struct ToolCredentialMeta {
    std::uint64_t group_id{0};
    std::string tool;
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 上/覆盖（同 gid+tool）；群须存在=false
  bool tool_cred_set(std::uint64_t group_id, const std::string& tool,
                     const std::string& sealed_hex,
                     const std::string& updated_by, std::int64_t ts_ms);
  bool tool_cred_delete(std::uint64_t group_id, const std::string& tool);
  // 密文取回（仅供服务端代理调用内存内解密——不暴露给任何 HTTP 面）
  std::optional<std::string> tool_cred_sealed(std::uint64_t group_id,
                                              const std::string& tool);
  // 掩码清单（无密文字段）
  std::vector<ToolCredentialMeta> tool_cred_list(std::uint64_t group_id);

  // —— R26-1 服务器 agent 面：公用服务器登记（agent 凭注册令牌心跳，
  // 令牌只存 SHA-256 摘要——明文只在登记回包出现一次；指标=最近一拍，
  // 红绿灯=last_seen 新鲜度由路由层判）——
  struct GroupServerRow {
    std::uint64_t id{0};
    std::uint64_t group_id{0};
    std::string name;
    std::string host;
    std::string enrolled_by;
    std::int64_t created_ms{0};
    std::int64_t last_seen_ms{0};
    double cpu_percent{-1.0}; // -1=尚未上报过
    double mem_used_mb{0};
    double mem_total_mb{0};
    double disk_used_mb{0};
    double disk_total_mb{0};
    double load1{0};
    // R26-4 凭据掩码元数据（空/0=未配置；密文永不在此行）
    std::string cred_updated_by;
    std::int64_t cred_updated_ms{0};
  };
  // 登记（token_hash=SHA-256 hex；同 gid+name 幂等复用行并轮换令牌）；
  // 群须存在——0=群不存在/失败
  std::uint64_t server_enroll(std::uint64_t group_id, const std::string& name,
                              const std::string& host,
                              const std::string& token_hash,
                              const std::string& enrolled_by,
                              std::int64_t ts_ms);
  std::optional<GroupServerRow> server_by_token_hash(
      const std::string& token_hash);
  bool server_heartbeat(std::uint64_t id, double cpu_percent,
                        double mem_used_mb, double mem_total_mb,
                        double disk_used_mb, double disk_total_mb,
                        double load1, std::int64_t ts_ms);
  std::vector<GroupServerRow> server_list(std::uint64_t group_id);

  // —— R26-3 远程会话面：一次性短票＋接入留痕（谁/何时/连哪台/协议/
  // 是否兑现/何时结束——时长=closed_ms-opened_ms 由路由层算）——
  struct ServerSessionRow {
    std::uint64_t id{0};
    std::uint64_t group_id{0};
    std::uint64_t server_id{0};
    std::string server_name;
    std::string host;
    std::string actor;
    std::string protocol; // ssh 起步（RDP/VNC 随后）
    std::int64_t opened_ms{0};
    std::int64_t redeemed_ms{0}; // 0=短票未兑现
    std::int64_t closed_ms{0};   // 0=进行中
  };
  // 开会话（留痕起点；ticket_hash=SHA-256 hex）——server_id 须属该群，
  // 不属=0
  std::uint64_t server_session_open(std::uint64_t group_id,
                                    std::uint64_t server_id,
                                    const std::string& actor,
                                    const std::string& protocol,
                                    const std::string& ticket_hash,
                                    std::int64_t ts_ms);
  // 短票兑现（一次性）：回会话行；查无=行内已兑现=过期的按路由层校验
  std::optional<ServerSessionRow> server_session_by_ticket(
      const std::string& ticket_hash);
  bool server_session_mark_redeemed(std::uint64_t id, std::int64_t ts_ms);
  // 关会话（时长终点）：行须属该群该人且进行中
  bool server_session_close(std::uint64_t group_id, std::uint64_t id,
                            const std::string& actor, std::int64_t ts_ms);
  std::vector<ServerSessionRow> server_session_list(std::uint64_t group_id);

  // —— R26-4 服务器凭据面：目标机接入凭据只存服务端（密文=cred::gcm_seal
  // 的 hex 串；客户端零凭据——memex 只做「看+连」，真用凭据的操作归
  // croupier）——
  struct ServerCredentialMeta {
    std::uint64_t server_id{0};
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 上/覆盖（同 server_id）；服务器须属该群=false
  bool server_cred_set(std::uint64_t group_id, std::uint64_t server_id,
                       const std::string& sealed_hex,
                       const std::string& updated_by, std::int64_t ts_ms);
  // 删（服务器须属该群）；无行=false
  bool server_cred_delete(std::uint64_t group_id, std::uint64_t server_id);
  // 密文取回（仅供服务端代理调用内存内解密——不暴露给任何 HTTP 面）
  std::optional<std::string> server_cred_sealed(std::uint64_t server_id);
  // 掩码清单（按群走 JOIN scope；无密文字段）
  std::vector<ServerCredentialMeta> server_cred_list(std::uint64_t group_id);

private:
  bool ensure_schema();

  sqlite3* db_{nullptr};
};

} // namespace memex::server
