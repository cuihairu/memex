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
  std::string signature;       // 个性签名（需求批⑪；空=未设）
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
// 字段序=既有位置初始化式 {"account","keyword",since,until,limit} 的兼容序——
// 新增字段只能尾插（peer，需求批⑧），插中间会让既有位置实参整体错位
//（test_nudge 等四处因此踩过 std::string(int) 的 null 构造崩溃）。
struct MessageSearch {
  std::string account;  // 该账号收发两侧都命中；空=全部
  std::string keyword;  // 正文包含（子串）；空=不过滤
  std::int64_t since_ms{0}; // 起始时间（含），0=不限
  std::int64_t until_ms{0}; // 截止时间（含），0=不限
  int limit{200};
  // 会话维度（需求批⑧）：对端账号（单聊双向）或群键 "group:N"；空=不限。
  // account 非空为前提（群靠成员关系联入；单聊叠加为「我与该对端」双向）。
  // 新代码请用字段名赋值，勿依赖位置。
  std::string peer;
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
  // 平台-10 直连文件旁路（蓝图§十九四问的部门/转发两问）：
  // 跨部门默认禁（白名单口径——一级部门不同即跨部门，未分配从严）；
  // 再转发默认允（内网协作常态），禁了之后 forward=1 的发送即拒。
  bool allow_cross_dept_file{false};
  bool allow_forward_file{true};
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

// 机器人（bot）台账：name 不含 "bot:" 前缀（全名= "bot:"+name 伪账号，
// 直插 group_members/offline_messages——两表无外键，零迁移）。token 明文
// 仅 add 时输出一次，库存 sha256 摘要；disabled=1 即 403 拒收发。
struct BotRow {
  std::string name;
  std::string token_hash;
  std::string created_by;
  std::int64_t created_ms{0};
  bool disabled{false};
};

// 模型端点（平台三期·模型网关）：上游 OpenAI 兼容服务登记项。注册序
// 即路由优先序；is_local=归档数据红线专用（仅本地模型——规则写死在
// 网关代码不做成配置）；api_key 库存原文（须可逆代发上游，与验自身
// token 的摘要口径不同）。
struct ModelEndpointRow {
  std::int64_t id{0};
  std::string name;    // 路由名（请求 model 字段命中即提优先）
  std::string base_url; // http://host[:port][/prefix]（仅 http，内网口径）
  std::string api_key; // 上游 Bearer（可空=上游不鉴权）
  std::string model;   // 上游模型名（空=透传请求里的 model）
  bool is_local{false};
  bool enabled{true};
  std::string created_by;
  std::int64_t created_ms{0};
};

// 模型调用审计：一次上游尝试一行（降级即多行，可对账）。只记元数据
// 不记 prompt/completion 正文（正文敏感；status 0=连接失败/超时）。
struct ModelCallRow {
  std::int64_t id{0};
  std::string caller; // bot:<name>（网关 Bearer 主体）
  std::string endpoint;
  std::string model;
  bool archive_scope{false};
  int prompt_chars{0};
  int completion_chars{0};
  int status{0};
  std::int64_t latency_ms{0};
  std::int64_t created_ms{0};
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

  // —— 在线时长（需求批⑩）：上下线事件流水 ——
  // 事件溯源式：在线区间由 online/offline 配对扫掠得出。聚合语义是
  // 「任一端在线即在线」——并发多端的并行区间取并集计一次，不叠加。
  // 登记点：register_online 记 'online'（顶替被踢的旧会话补 'offline'）、
  // unregister_online 记 'offline'（登出与意外断开的唯一真实离线路径）。
  bool add_presence_event(const std::string& account, const std::string& event,
                          std::int64_t ts_ms);

  // [from_ms, to_ms] 窗内的在线毫秒数：按时间序全量扫该账号事件做深度
  // 计数（online +1 / offline -1），depth 从 0 变正即开段、归零即闭段；
  // 窗起点前已开的段按计数延续；末笔 online 到窗尾仍未闭→计到窗尾
  // （调用方保证 ts_ms 递增即可，同毫秒按插入序）。
  std::int64_t online_ms_between(const std::string& account,
                                 std::int64_t from_ms, std::int64_t to_ms) const;

  // 服务端启动自愈：末态 depth>0（悬空 online，断电/崩溃所致）的账号
  // 统一补一笔 offline（ts=start_ms）——把崩溃期的在线段截断到重启点，
  // 防止关机期被持续计为在线。幂等：重启多次只补到当前笔数。
  void trim_dangling_online(std::int64_t ts_ms);

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
  // 送达回执幂等判据（需求批⑦）：该 msg_id 是否已记过 by_account 的
  // delivered 事件——重复 ACK 只记一次、只推一次 DELIVER_NOTICE。
  bool delivered_recorded(const std::string& msg_id,
                          const std::string& by_account);
  // 重放事件重建当前态（created 建 行、recalled 置标记、edited 替正文）
  std::vector<ArchivedMessage> rebuild_messages_from_events();

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

  // —— 平台-11 远程协助（Security Domain 模型，蓝图§二十七/§五）——
  //     协助会话生命周期五态：requested→approved→active→closed（或
  //     requested→denied）；五粒度权限位对应蓝图 view/keyboard/mouse/
  //     clipboard/file_transfer。两条红线不设开关：consent=只有受控方
  //     本人可批/可拒/可随时撤（蓝图§五 require_consent 恒真），
  //     audit=全部状态迁移与策略变更自动留痕（require_audit 恒真）。
  //     部门放行开关默认禁（白名单口径，蓝图 allowed_department）。
  //     协议面与媒体传输（P2）后续另接，本段只做模型层。
  enum : int {
    kAssistView = 1,
    kAssistKeyboard = 2,
    kAssistMouse = 4,
    kAssistClipboard = 8,
    kAssistFileTransfer = 16,
  };
  struct AssistPolicy {
    std::int64_t id{0};
    std::string department_path; // 空=全局行；未配置部门沿链上溯
    bool allow{false};
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  struct AssistSession {
    std::string id;
    std::string requester;
    std::string target;
    int requested_mask{0};
    int granted_mask{0}; // 受控方实批集 ⊆ 申请集（可缩不可扩）
    std::string status;  // requested/approved/active/closed/denied
    std::int64_t requested_ms{0};
    std::int64_t approved_ms{0};
    std::int64_t started_ms{0};
    std::int64_t ended_ms{0}; // 终态时刻（closed 与 denied 都填）
    std::string end_actor;
    std::string end_reason;
  };
  struct AssistAuditRow {
    std::int64_t id{0};
    std::string session_id; // 空=策略面动作
    std::string actor;
    std::string action; // policy/request/approve/deny/start/end
    std::string detail; // 权限集名／理由
    std::int64_t ts_ms{0};
  };
  // 部门放行开关（UPSERT 按部门路径）；部门行须挂已存在部门
  bool assist_policy_set(bool allow, const std::string& department_path,
                         const std::string& updated_by, std::int64_t ts_ms);
  std::vector<AssistPolicy> assist_policy_list();
  // 生效口径：本人部门链逐级上溯 → 全局行 → 内置默认禁止
  bool assist_policy_resolve(const std::string& account);
  // 发起：两账号存在、非同一人、双方部门均放行（从严）、mask 合法非零
  // → 返回会话 id（ra-<hex>），空串=拒
  std::string assist_request(const std::string& requester,
                             const std::string& target, int mask,
                             std::int64_t ts_ms);
  // 受控方本人批准；granted 须 ⊆ requested 且非零（consent 可缩权）
  bool assist_approve(const std::string& id, const std::string& by,
                      int granted_mask, std::int64_t ts_ms);
  // 受控方本人拒绝（requested→denied 终态）
  bool assist_deny(const std::string& id, const std::string& by,
                   std::int64_t ts_ms);
  // 启动：任一当事方，approved→active
  bool assist_start(const std::string& id, const std::string& by,
                    std::int64_t ts_ms);
  // 结束/撤权：任一当事方，active 或 approved（批了没用上）→closed；
  // 受控方在 active 期结束即撤权，即时生效。终态再动拒。
  bool assist_end(const std::string& id, const std::string& by,
                  const std::string& reason, std::int64_t ts_ms);
  std::optional<AssistSession> assist_session(const std::string& id);
  // 该账号相关（发起或受控）的会话，按发起时刻倒序；account 空=全部
  std::vector<AssistSession> assist_sessions(const std::string& account);
  // 会话事件序列（按发生序）；session_id 空=含策略面动作的近期记录
  std::vector<AssistAuditRow> assist_audits(const std::string& session_id);

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
  // 个性签名设置/清除（需求批⑪）：签名即档案面单值，upsert（空串=清除；
  // 长度门 120 字由会话层把关）；账号不存在返回 false
  bool set_signature(const std::string& account, const std::string& signature);

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
                  bool allow_cross_state, bool new_device_approval,
                  bool allow_cross_dept_file = false,
                  bool allow_forward_file = true);
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

  // —— 机器人（bot）台账 ——
  // 建 bot（name 不含 "bot:" 前缀，不得为空或含 ':'）；重名返回 0。
  std::int64_t bot_add(const std::string& name, const std::string& token_hash,
                       const std::string& created_by, std::int64_t created_ms);
  // 按 token 摘要取行（不存在/禁用行也返回，由调用方按 disabled 裁 403）。
  std::optional<BotRow> bot_by_token(const std::string& token_hash);
  // 全部行（CLI list）
  std::vector<BotRow> bot_list();
  // 按 name 取行（CLI join/leave 前校验）
  std::optional<BotRow> bot_by_name(const std::string& name);
  bool bot_remove(const std::string& name);
  // 禁用/启用（disabled=是否禁用；返回是否确有该行）
  bool bot_set_disabled(const std::string& name, bool disabled);
  // bot 加群/退群：直插/直删 group_members（伪账号，无外键约束）。
  // join：群或 bot 不存在返回 false；leave：本非成员返回 false。
  bool bot_join_group(const std::string& name, std::uint64_t group_id,
                      std::int64_t joined_ms);
  bool bot_leave_group(const std::string& name, std::uint64_t group_id);

  // —— 模型网关（平台三期）：端点登记＋调用审计 ——
  // 端点登记（重名返回 0）；注册序=路由优先序。
  std::int64_t model_endpoint_add(const ModelEndpointRow& ep);
  std::optional<ModelEndpointRow> model_endpoint_by_name(
      const std::string& name);
  std::vector<ModelEndpointRow> model_endpoints_list(); // 注册序（id 序）
  bool model_endpoint_remove(const std::string& name);
  bool model_endpoint_set_enabled(const std::string& name, bool enabled);
  bool model_endpoint_set_local(const std::string& name, bool is_local);
  // 调用审计：一次上游尝试一行；calls_list 按时间倒序取最近 limit 条。
  std::int64_t model_call_add(const ModelCallRow& call);
  std::vector<ModelCallRow> model_calls_list(int limit);
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

  // —— 平台-12 群能力开关（权限模型「群能力管理员配全」，蓝图§五）——
  //     词汇表固定七项，对应既有能力面（群公告/备忘录/密码箱/群工具/
  //     群服务器工具/群文件/外网上传收件箱）。口径：未配置=现行（允许），
  //     配置为禁即拒——首段收紧步进（存量部署不破），全面 deny-by-default
  //     留后续（如实注明）。配置权=群主/管理员（调用方把守），变更落
  //     查阅台账（action=group.capability，权限模型「全程留痕」）。
  static const std::vector<std::string>& group_capability_names();
  struct GroupCapability {
    std::uint64_t gid{0};
    std::string capability;
    bool enabled{true};
    std::string updated_by;
    std::int64_t updated_ms{0};
  };
  // 群须存在、能力名须在词汇表；UPSERT 一行（gid+capability 唯一）
  bool group_capability_set(std::uint64_t gid, const std::string& capability,
                            bool enabled, const std::string& by,
                            std::int64_t ts_ms);
  // 生效口径：无配置行=允许；能力名不在词汇表=拒（false）
  bool group_capability_enabled(std::uint64_t gid,
                                const std::string& capability);
  // 某群的配置行（含禁与重新允的历史现值，按能力名字典序）
  std::vector<GroupCapability> group_capabilities_list(std::uint64_t gid);

  // —— R27-1 个人任务清单（tasks：谁的任务归谁的清单，分配=别人建到
  //     你清单上；完成/提醒回执归清单主人，撤回归创建人）——
  struct TaskRow {
    std::int64_t id{0};
    std::string owner;      // 清单主人（任务落在谁的清单）
    std::string creator;    // 创建人（自建=owner；他人分配=分配人）
    std::string title;
    std::string note;       // 备注（可空）
    std::int64_t due_ms{0}; // 0=未设提醒
    std::int64_t reminded_ms{0}; // 0=未提醒（客户端本地通知后回执落笔）
    bool done{false};
    std::int64_t done_ms{0};
    std::int64_t created_ms{0};
    std::string provider;   // R27-2 外部任务引用（provider id，空=本地任务）
    std::string ext_key;    // 外部键原文（「project#键」或完整链接；与
                            // provider 成对；详情 URL 由客户端 SPI 解析）
  };
  // 建任务（owner 与 creator 都须为已建账号；标题非空；provider/ext_key
  // 成对缺省=本地任务）
  std::int64_t task_create(const std::string& owner,
                           const std::string& creator,
                           const std::string& title, const std::string& note,
                           std::int64_t due_ms, std::int64_t created_ms,
                           const std::string& provider = "",
                           const std::string& ext_key = "");
  // 我的清单（含别人派来的；id 倒序）
  std::vector<TaskRow> tasks_of(const std::string& owner);
  // 我派给别人的（creator=本人且 owner≠本人；id 倒序）
  std::vector<TaskRow> tasks_assigned_by(const std::string& creator);
  std::optional<TaskRow> task_by_id(std::int64_t id);
  // 完成/回退（回执带 done_ms；0/非 0 由调用方语义决定，此处按 bool）
  bool task_set_done(std::int64_t id, bool done, std::int64_t done_ms);
  // 提醒回执（只落一次时刻；重复调用以最早为准不回退）
  bool task_mark_reminded(std::int64_t id, std::int64_t reminded_ms);
  bool task_delete(std::int64_t id);
  // 分配资格数据面（R27-1 权限模型「谁能分配」）：两账号是否同任一群
  // 的共同成员（现查现裁=入群即授权、退群即失）
  bool co_members(const std::string& a, const std::string& b);

  // —— 二期·审批（请假起步，设计稿 docs/design/审批与日报周报.md）——
  // 四态写死：pending→approved|rejected|withdrawn；只有 pending 可决；
  // 撤回=申请人专属且仅 pending；决定不删改（全程留痕）
  struct ApprovalRow {
    std::int64_t id{0};
    std::string applicant; // 申请人
    std::string type;      // 请假类型（年假/事假/病假/调休 白名单）
    std::string leave_from;
    std::string leave_to;
    std::string reason;
    std::string status; // pending|approved|rejected|withdrawn
    std::string decider;
    std::string decision_note;
    std::int64_t created_ms{0};
    std::int64_t decided_ms{0};
  };
  std::int64_t approval_create(const std::string& applicant,
                               const std::string& type,
                               const std::string& leave_from,
                               const std::string& leave_to,
                               const std::string& reason,
                               std::int64_t created_ms);
  // 我申请的（id 倒序）
  std::vector<ApprovalRow> approvals_of(const std::string& applicant);
  // 全部待决（id 倒序；可见性过滤在路由层按判权逐行裁）
  std::vector<ApprovalRow> approvals_pending();
  std::optional<ApprovalRow> approval_by_id(std::int64_t id);
  // 决定（UPDATE ... WHERE status='pending' 守卫，重复决=假）
  bool approval_decide(std::int64_t id, const std::string& decider,
                       bool approved, const std::string& note,
                       std::int64_t decided_ms);
  // 撤回（申请人专属；UPDATE ... WHERE applicant=? AND status='pending'）
  bool approval_withdraw(std::int64_t id, const std::string& applicant);

  // —— 二期·日报周报（设计稿 §二）——
  // 个人日报台账：当日重复提交=更新（UNIQUE(author,report_date) upsert，
  // 不留修订史——同 R23-3 个人备忘录口径）；周报=日报按周聚合视图，
  // 不单设表。直属上级可看下属（az report:read 在路由层裁）
  struct ReportRow {
    std::int64_t id{0};
    std::string author;
    std::string report_date; // YYYY-MM-DD（日报归属日）
    std::string content;     // 三段自由文本（做了什么/明日计划/blockers）
    std::int64_t created_ms{0};
    std::int64_t updated_ms{0};
  };
  // 写（存在即更新 content/updated_ms，created_ms 不动）→ id
  std::int64_t report_upsert(const std::string& author,
                             const std::string& report_date,
                             const std::string& content,
                             std::int64_t ts_ms);
  // 某人全部（日期倒序）——自己看自己/直属上级看下属（判权在路由层）
  std::vector<ReportRow> reports_of(const std::string& author);
  // 直接下属（org_reporting_lines 反查：manager_account=? 去重）
  std::vector<std::string> direct_reports(const std::string& manager);

  // —— 品牌物料（设计稿 docs/design/品牌物料.md）：全服务器单行聚合，
  // version 每次写 +1＝客户端变更判据；PNG 字节直存（校验在路由/CLI 层）——
  struct Branding {
    std::string company_name;
    std::string accent;    // '#rrggbb'，空=未配
    std::string slogan;    // 登录页文案，空=未配
    std::vector<unsigned char> logo;   // PNG 字节，空=未配
    std::vector<unsigned char> splash; // PNG 字节，空=未配（可选件）
    std::int64_t version{0};
    std::int64_t updated_ms{0};
  };
  Branding branding_get(); // 无行=默认全空 version 0
  // 文本三件；只动非空给出的项（nullopt=不动，空串=清空）；回新 version
  std::int64_t branding_set(const std::optional<std::string>& company_name,
                            const std::optional<std::string>& accent,
                            const std::optional<std::string>& slogan,
                            std::int64_t ts_ms);
  std::int64_t branding_set_logo(const std::vector<unsigned char>& png,
                                 std::int64_t ts_ms);
  std::int64_t branding_set_splash(const std::vector<unsigned char>& png,
                                   std::int64_t ts_ms);
  std::int64_t branding_clear_logo();
  std::int64_t branding_clear_splash();

  // —— 表情包素材（需求批②：个人素材，仅本人读写——收藏随账号走）——
  // 字节以 BLOB 落库（≤1MiB/张，与品牌素材同口径；大文件仍走对象存储面）
  struct EmojiAsset { // 列表面（无字节）
    std::int64_t id{0};
    std::string name;
    std::int64_t size{0};
    std::int64_t ts_ms{0};
  };
  // 新增素材；回 id（失败 -1）
  std::int64_t emoji_add(const std::string& account, const std::string& name,
                         const std::vector<unsigned char>& bytes,
                         std::int64_t ts_ms);
  // 该账号素材（ts 倒序）
  std::vector<EmojiAsset> emoji_list(const std::string& account);
  // 取字节：仅资产属主可取（他人/不存在 false）
  bool emoji_bytes(std::int64_t id, const std::string& account,
                   std::vector<unsigned char>& out, std::string& name_out);
  // 删除：仅属主（非属主/不存在 false）
  bool emoji_delete(std::int64_t id, const std::string& account);

  // —— 用户头像（需求批⑫）：每账号四档尺寸（32/64/128/256）——
  // 独立成表不并入 member_profiles：BLOB 行不跟资料行一起被 ORG_QUERY
  // 全量扫读；版本戳=该账号各档行的 MAX(ts_ms)（无行=0，删除行即回落
  // 默认头像，缓存失效语义随行生灭，无需另立 ver 列与迁移）。
  // 上传（裁剪+四档生成在客户端完成，服务端只验 magic/上限/档位）与
  // 删除＝本人属主裁决；读面＝登录成员可读（组织内互见，同资料面口径）。
  static constexpr int kAvatarSizes[] = {32, 64, 128, 256};
  static bool avatar_size_valid(int size);
  // 写入/覆盖一档（INSERT OR REPLACE）；ts_ms 同时是该档的版本成分
  bool avatar_put(const std::string& account, int size, const std::string& mime,
                  const std::vector<unsigned char>& bytes, std::int64_t ts_ms);
  // 取字节（登录成员可读口径由路由层判权，存储层不问来者）
  bool avatar_bytes(const std::string& account, int size,
                    std::vector<unsigned char>& out, std::string& mime_out);
  // 清除全部档位（真删了行才 true——无头像可删 false，路由层回 404）
  bool avatar_clear(const std::string& account);
  // 头像版本戳：各档行 MAX(ts_ms)，无行=0（ORG_QUERY 逐成员带出）
  std::int64_t avatar_ver(const std::string& account) const;

  // —— 二期·办公室位置图（设计稿 docs/design/办公室位置图.md）——
  // 抽象平面（网格归一化坐标 0~1，不画真实底图）；工位即楼层归属
  //（本人楼层=本人占用工位所在楼层，不从部门推导）；编辑权 org-admin
  //（az office:manage 在路由层裁）
  struct SeatRow {
    std::int64_t id{0};
    std::string floor;      // 楼层号（"3F"）
    std::string label;      // 工位号（UNIQUE(floor,label)）
    double x{0};            // 归一化坐标 0~1
    double y{0};
    std::string account;    // 占用者（空=空位；一人一工位=部分唯一索引）
  };
  // 建/改位（拖拽落位即 upsert；UNIQUE(floor,label) 冲突=改坐标）→ id
  std::int64_t seat_upsert(const std::string& floor, const std::string& label,
                           double x, double y, std::int64_t ts_ms);
  bool seat_delete(std::int64_t id);
  // 绑定/解绑占用者（account 空=解绑；幽灵拒；一人一工位=换座先解绑）
  bool seat_bind(std::int64_t id, const std::string& account,
                 std::int64_t ts_ms);
  std::vector<SeatRow> seats_on_floor(const std::string& floor);
  std::optional<SeatRow> seat_of(const std::string& account);

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

  // —— 二期群工具三件（原生群互动，判权在路由层循 file:read 群继承，
  // 身份约束服务端逻辑判；台账即留痕——改票/改接龙条目不留修订史）——
  // 投票（记名单选：choice=选项序号 1 起）
  struct GroupPoll {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string topic;
    std::vector<std::string> options;
    std::int64_t deadline_ms{0}; // 0=不设截止（到点惰性判定自动截止）
    bool closed{false};
    bool anonymous{false}; // 匿名：结果与台账不回带 voter 身份（库内留 account 供改票与审计）
    bool multi{false};     // 多选：choice 存位集（bit i=选 i+1 号），一人仍只一行计一票
    std::string created_by;
    std::int64_t created_ms{0};
  };
  struct GroupPollVote {
    std::string account;
    int choice{0};
    std::int64_t ts_ms{0};
  };
  // 建票（群须存在；topic 非空、选项 2~10）；非法返回 0
  std::int64_t poll_create(std::uint64_t group_id, const std::string& topic,
                           const std::vector<std::string>& options,
                           std::int64_t deadline_ms, const std::string& by,
                           std::int64_t ts_ms, bool anonymous = false,
                           bool multi = false);
  std::optional<GroupPoll> poll_by_id(std::int64_t id);
  std::vector<GroupPoll> polls_list(std::uint64_t group_id); // id DESC
  // 投/改票（upsert 覆盖）；票不存在或已关=false、选项越界=false
  bool poll_vote(std::int64_t poll_id, const std::string& account, int choice,
                 std::int64_t ts_ms);
  std::vector<GroupPollVote> poll_votes(std::int64_t poll_id); // ts ASC
  bool poll_close(std::int64_t poll_id); // 已关仍返回 true（幂等收口）
  // 接龙（一人一条，重复提交=更新自己条目）
  struct GroupChain {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string title;
    std::string format_hint;
    bool closed{false};
    std::string created_by;
    std::int64_t created_ms{0};
  };
  struct GroupChainEntry {
    std::string account;
    std::string content;
    std::int64_t ts_ms{0};
  };
  std::int64_t chain_create(std::uint64_t group_id, const std::string& title,
                            const std::string& format_hint,
                            const std::string& by, std::int64_t ts_ms);
  std::optional<GroupChain> chain_by_id(std::int64_t id);
  std::vector<GroupChain> chains_list(std::uint64_t group_id); // id DESC
  bool chain_join(std::int64_t chain_id, const std::string& account,
                  const std::string& content, std::int64_t ts_ms);
  std::vector<GroupChainEntry> chain_entries(std::int64_t chain_id); // ts ASC
  bool chain_close(std::int64_t chain_id);
  // 群任务（认领制；done 终态留痕不删行）
  struct GroupTask {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string title;
    std::string assignee; // 空=待认领
    std::int64_t due_ms{0};
    std::int64_t claimed_ms{0}; // 认领留痕时刻
    std::string status; // todo｜done
    std::string created_by;
    std::int64_t created_ms{0};
    std::string done_by;
    std::int64_t done_ms{0};
  };
  // 建（title 非空；assignee 非空须为群成员）——非法返回 0
  std::int64_t gtask_create(std::uint64_t group_id, const std::string& title,
                            const std::string& assignee, std::int64_t due_ms,
                            const std::string& by, std::int64_t ts_ms);
  std::vector<GroupTask> gtasks_list(std::uint64_t group_id); // id DESC
  // 认领（todo 且无人认领；已占=false）
  bool gtask_claim(std::int64_t id, const std::string& account,
                   std::int64_t ts_ms);
  // 完成（assignee/created_by/群主·管理员由路由层预判后传入记账；
  // 仅 todo 可完成）
  bool gtask_done(std::int64_t id, const std::string& done_by,
                  std::int64_t ts_ms);

private:
  bool ensure_schema();

  sqlite3* db_{nullptr};
};

} // namespace memex::server
