// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：备忘录域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— R23-3 文件助手备忘录（memos：本人文本，不入对象存储）——
  // 历史留痕不在本层：自备忘录不留修订史（修订历史属 R24-2 群备忘录）。
  struct MemoRow {
    std::int64_t id{0};
    std::string owner;
    std::string content;
    std::int64_t created_ms{0};
    std::int64_t updated_ms{0};
  };
  // 建备忘录：owner/content 非空才收（无主/空文不落库）；返回 id，拒=0
  std::int64_t create_memo(const std::string& owner,
                           const std::string& content, std::int64_t ts_ms);
  std::optional<MemoRow> memo_by_id(std::int64_t id);
  // 本人列表（updated_ms 倒序）——owner 过滤即权限（owner 之外查不到）
  std::vector<MemoRow> list_memos(const std::string& owner, int limit = 200,
                                  int offset = 0);
  // 更新/删除带 owner 二次校验（WHERE owner=）：异属主操作落不到行
  bool update_memo(std::int64_t id, const std::string& owner,
                   const std::string& content, std::int64_t ts_ms);
  bool delete_memo(std::int64_t id, const std::string& owner);

  // —— R24-2 群备忘录（群维度共享知识：标题+正文，修订历史可回滚）——
  // 与个人备忘录（memos，R23-3 文件助手）分表：空间不同权限不同，
  // 开放编辑开关决定成员能否越过管理员写（开放时编辑照旧逐笔留痕）。
  struct GroupMemo {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string title;
    std::string content;
    std::string author; // 建条目者
    std::int64_t created_ms{0};
    std::int64_t updated_ms{0};
  };
  // 修订笔（每次编辑后的全文快照；首建也落一笔，editor=作者）
  struct GroupMemoRevision {
    std::int64_t id{0};
    std::int64_t memo_id{0};
    std::string title;
    std::string content;
    std::string editor;
    std::int64_t ts_ms{0};
  };
  std::int64_t create_group_memo(std::uint64_t group_id,
                                 const std::string& title,
                                 const std::string& content,
                                 const std::string& author,
                                 std::int64_t ts_ms);
  std::optional<GroupMemo> group_memo_by_id(std::int64_t id);
  // 列表（updated_ms 倒序）；keyword 非空=标题或正文子串命中
  std::vector<GroupMemo> list_group_memos(std::uint64_t group_id,
                                          const std::string& keyword = "",
                                          int limit = 200, int offset = 0);
  // 更新：同事务落修订笔（editor=本次编辑者；回滚也走这里——回滚即一次编辑）
  bool update_group_memo(std::int64_t id, const std::string& title,
                         const std::string& content,
                         const std::string& editor, std::int64_t ts_ms);
  bool delete_group_memo(std::int64_t id, std::uint64_t group_id);
  std::vector<GroupMemoRevision> group_memo_history(std::int64_t memo_id);
  // 开放编辑开关（默认关=管理员维护；群主/管理员可设——判权在路由层）
  bool set_group_memo_open_edit(std::uint64_t group_id, bool open);
  bool group_memo_open_edit(std::uint64_t group_id);
