// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：审批与日报域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
