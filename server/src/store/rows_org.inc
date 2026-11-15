// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：组织与导入行；由 store.hpp 按位包含，无包含卫、勿单独编译。
// 每人至多一名上级（结构性约束），链路逐级上溯由 manager_chain 完成。
struct MemberProfile {
  std::string account;
  std::string display_name;
  std::string department_path; // 全路径（"公司/研发部/客户端组"），空=未分配
  std::string title;           // 职务
  std::string manager;         // 直属上级账号（空=无）
  std::string role;            // admin／member（T3.1 角色分级）
  std::string signature;       // 个性签名（需求批⑪；空=未设）
};

// 批量导入行（CSV 解析后）与结果（错误行校验拒绝并报告行号）
struct OrgImportRow {
  int line_no{0};
  std::string account;
  std::string dept;    // 部门全路径（空=不分配）
  std::string title;   // 职务（空=无）
  std::string manager; // 直属上级账号（空=无上级）
};

struct OrgImportResult {
  int imported{0};
  std::vector<std::string> errors; // 行号 + 拒绝原因
};

// 检索条件（T3.2）：全部字段可空——空=不过滤；组合为 AND。
// 字段序=既有位置初始化式 {"account","keyword",since,until,limit} 的兼容序——
// 新增字段只能尾插（peer，需求批⑧），插中间会让既有位置实参整体错位
