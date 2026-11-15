// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：账号/登录/设备域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
  // 入队钩子（bot 长轮询唤醒源）：queue_offline 真落新行后同步回调
  // to_account。现行全部入队点（session TEXT 扇出／webhook 投递面）与
  // 回调同在 CollabServer 共享单线程 io_context 上，无并发；回调内不得
  // 再做本库操作。owner 指针守卫：多 WebhookServer 交叠生命周期时，
  // 旧者析构不清掉新者刚注册的钩子。
  void set_enqueue_hook(void* owner,
                        std::function<void(const std::string&)> hook);
  void clear_enqueue_hook(void* owner);
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
  // 按 msg_id 查归档全行（BUG-007 撞 id 内容比对用）；不存在返回 nullopt。
  std::optional<ArchivedMessage> message_by_id(const std::string& msg_id);
  // 撤回事件独立留痕（只附加、不删改）。
  bool record_recall_event(const std::string& msg_id,
                           const std::string& by_account,
                           std::int64_t ts_ms);
  std::size_t recall_event_count(const std::string& msg_id);
