// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：模型网关域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
