// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：通讯录可见性行；由 store.hpp 按位包含，无包含卫、勿单独编译。
// 逗号分隔，仅 member 行有效）。白名单例外走 VisibilityAllow。
struct VisibilityRow {
  std::string scope; // member | dept
  std::string key;   // 账号 或 部门全路径
  bool hidden{false};
  bool restrict_scope{false};
  std::string hide_fields;
};

// 白名单例外：viewer 可见 target——target=成员账号，或部门全路径（整树豁免）
struct VisibilityAllow {
  std::string viewer;
  std::string target;
};

// 群聊（T4.1）：群消息走既有 TEXT（to="group:<群号>"），
