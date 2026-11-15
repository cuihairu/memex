// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：通讯录可见性域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
