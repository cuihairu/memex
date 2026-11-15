// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：群密码箱域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
