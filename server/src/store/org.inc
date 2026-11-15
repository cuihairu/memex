// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：组织架构域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
