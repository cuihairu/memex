// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：远程协助域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
