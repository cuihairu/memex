// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：策略开关域；由 store.hpp 按位包含，无包含卫、勿单独编译。
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
