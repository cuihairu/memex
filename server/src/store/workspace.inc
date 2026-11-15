// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：邮箱与品牌域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— 邮箱·统一收件箱（设计稿 docs/design/mailbox.md：邮箱=视图+已读
  // 状态，非新通道——三类条目（warning 告警/notice 通知/announce 公告）
  // 由既有事件源各落一行，状态按账号·按条目独立；三面同源：未读数以
  // 本表为唯一来源。边界②铁律：任何邮箱操作不触归档行）——
  struct MailboxRow {
    std::int64_t id{0};
    std::string kind;    // warning|notice|announce
    std::string title;   // 条目标题
    std::string sub;     // 语境副行（原型 kw-context 位）
    std::string source;  // 来源列（admin 事件源/@我/审批结果…）
    std::string ref;     // 正文/跳转位置（group:3/approvals…）
    std::int64_t created_ms{0};
    int state{0};                // 0=未读 1=已读 2=已处理（告警专属）
    std::string handled_by;      // 告警处理人（留痕）
    std::int64_t handled_ms{0};  // 告警处理时间（留痕）
    std::int64_t confirmed_ms{0}; // 公告已读确认时间（P2 留痕）
    int confirm_total{0};        // 公告：应收确认人数
    int confirm_count{0};        // 公告：已确认人数
  };
  // 入箱（收件人逐人建未读状态行）。dedup 非空=幂等键：同键已有条目
  // 即不再入——告警类例外，已有条目须全部收件人已处理才允许再入
  //（磁盘持续超阈值等修复后复发要能再报）。收件人空=不入箱回 0。
  std::int64_t mailbox_add(const std::string& kind, const std::string& title,
                           const std::string& sub, const std::string& source,
                           const std::string& ref, const std::string& dedup,
                           const std::vector<std::string>& recipients,
                           std::int64_t ts_ms);
  // 某账号收件箱（kind 空=全部；unread_only=只看未读；q=标题/来源子串
  // 命中（不分大小写）；id 倒序，上限 200——检索台单页同口径）
  std::vector<MailboxRow> mailbox_of(const std::string& account,
                                     const std::string& kind, bool unread_only,
                                     const std::string& q);
  struct MailboxCounts {
    int all{0};
    int warning{0};
    int notice{0};
    int announce{0};
  };
  // 未读口径（rail 徽章与收件箱页签计数同源同数）
  MailboxCounts mailbox_counts(const std::string& account);
  // 标记已读（0→1；已处理位不动）；回是否真迁移（边界②：只动状态行）
  bool mailbox_mark_read(std::int64_t id, const std::string& account);
  // 全部未读→已读，回迁移条数
  int mailbox_mark_all_read(const std::string& account);
  // 告警标记已处理（仅 warning 条目；处理人+时间留痕；已读位同置）
  bool mailbox_mark_handled(std::int64_t id, const std::string& account,
                            std::int64_t ts_ms);
  // 公告确认已读（仅 announce 条目；确认时间留痕（P2 谁/何时可查）；
  // 已读位同置）
  bool mailbox_confirm(std::int64_t id, const std::string& account,
                       std::int64_t ts_ms);
  // 公告已读确认台账（P2 按条目查：全收件人＋各自确认时间，未确认=0）
  struct MailboxConfirmRow {
    std::string account;
    std::int64_t confirmed_ms{0};
  };
  std::vector<MailboxConfirmRow> mailbox_confirmations(std::int64_t id);
  // 管理员账号集（告警类默认受众：accounts.role='admin'）
  std::vector<std::string> admin_accounts();

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
