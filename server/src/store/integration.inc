// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：webhook/bot 域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
  // —— bot 可用范围白名单（细粒度权限）——
  // 口径：名单非空才收紧，空=不设限（存量 bot 零变化）。
  // 群白名单：名单非空时 bot 群成员资格 ⊆ 名单——allow/deny 后统一
  // 扫尾，不在名单的群连带退群（bot_remove 连带退群同款）；join 名单
  // 外群由 bot_join_group 拒。部门白名单：名单非空时单聊目标账号部门
  // 路径须落在任一白名单部门子树（「/」段边界的 ancestor-or-self 前缀
  // 匹配；未分配部门从严）。入站不裁（白名单管 bot 能动谁，不管谁能找
  // bot——TEXT 直达/扇出零改动是 bot 面立身之本）。allow 的群／部门须
  // 已存在（无隐式创建）；幽灵账号交给既有 404 语义。
  // 名单写入留痕（created_ms）。bot_remove 连带清两表。
  bool bot_allow_group(const std::string& name, std::uint64_t group_id,
                       std::int64_t created_ms);  // 群不存在返 false
  bool bot_deny_group(const std::string& name, std::uint64_t group_id);  // 移出名单即连带退群（名单复空亦退）
  bool bot_allow_dept(const std::string& name,
                      const std::string& department_path,
                      std::int64_t created_ms);  // 部门路径不存在返 false
  bool bot_deny_dept(const std::string& name,
                     const std::string& department_path);
  std::vector<std::uint64_t> bot_groups_allowed(const std::string& name);
  std::vector<std::string> bot_depts_allowed(const std::string& name);
  // 判定面：名单非空且不含 → false（true=放行）。幽灵账号 bot_group_
  // allowed 不涉及；bot_account_allowed 对不存在账号返回 true（交
  // deliver_notice 出既有 404）。
  bool bot_group_allowed(const std::string& name, std::uint64_t group_id);
  bool bot_account_allowed(const std::string& name, const std::string& account);
