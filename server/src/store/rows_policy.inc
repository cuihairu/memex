// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：策略行；由 store.hpp 按位包含，无包含卫、勿单独编译。
struct PolicyRow {
  std::string department_path; // 空=全局默认
  bool allow_anonymous{true};
  bool allow_cross_state{true};
  bool new_device_approval{false};
  // 平台-10 直连文件旁路（蓝图§十九四问的部门/转发两问）：
  // 跨部门默认禁（白名单口径——一级部门不同即跨部门，未分配从严）；
  // 再转发默认允（内网协作常态），禁了之后 forward=1 的发送即拒。
  bool allow_cross_dept_file{false};
  bool allow_forward_file{true};
};

// 通讯录可见性（T4.6）：一行配置管一个目标——scope="member"（key=账号）或
// "dept"（key=部门全路径）。hidden＝对查看者隐藏；restrict_scope＝该部门成员
// 只看本部门（仅 dept 行有效）；hide_fields＝敏感字段脱敏（title,manager,role
