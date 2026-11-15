// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：群互动三件（投票/接龙/群任务）；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— 二期群工具三件（原生群互动，判权在路由层循 file:read 群继承，
  // 身份约束服务端逻辑判；台账即留痕——改票/改接龙条目不留修订史）——
  // 投票（记名单选：choice=选项序号 1 起）
  struct GroupPoll {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string topic;
    std::vector<std::string> options;
    std::int64_t deadline_ms{0}; // 0=不设截止（到点惰性判定自动截止）
    bool closed{false};
    bool anonymous{false}; // 匿名：结果与台账不回带 voter 身份（库内留 account 供改票与审计）
    bool multi{false};     // 多选：choice 存位集（bit i=选 i+1 号），一人仍只一行计一票
    std::string created_by;
    std::int64_t created_ms{0};
  };
  struct GroupPollVote {
    std::string account;
    int choice{0};
    std::int64_t ts_ms{0};
  };
  // 建票（群须存在；topic 非空、选项 2~10）；非法返回 0
  std::int64_t poll_create(std::uint64_t group_id, const std::string& topic,
                           const std::vector<std::string>& options,
                           std::int64_t deadline_ms, const std::string& by,
                           std::int64_t ts_ms, bool anonymous = false,
                           bool multi = false);
  std::optional<GroupPoll> poll_by_id(std::int64_t id);
  std::vector<GroupPoll> polls_list(std::uint64_t group_id); // id DESC
  // 投/改票（upsert 覆盖）；票不存在或已关=false、选项越界=false
  bool poll_vote(std::int64_t poll_id, const std::string& account, int choice,
                 std::int64_t ts_ms);
  std::vector<GroupPollVote> poll_votes(std::int64_t poll_id); // ts ASC
  bool poll_close(std::int64_t poll_id); // 已关仍返回 true（幂等收口）
  // 接龙（一人一条，重复提交=更新自己条目）
  struct GroupChain {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string title;
    std::string format_hint;
    bool closed{false};
    std::string created_by;
    std::int64_t created_ms{0};
  };
  struct GroupChainEntry {
    std::string account;
    std::string content;
    std::int64_t ts_ms{0};
  };
  std::int64_t chain_create(std::uint64_t group_id, const std::string& title,
                            const std::string& format_hint,
                            const std::string& by, std::int64_t ts_ms);
  std::optional<GroupChain> chain_by_id(std::int64_t id);
  std::vector<GroupChain> chains_list(std::uint64_t group_id); // id DESC
  bool chain_join(std::int64_t chain_id, const std::string& account,
                  const std::string& content, std::int64_t ts_ms);
  std::vector<GroupChainEntry> chain_entries(std::int64_t chain_id); // ts ASC
  bool chain_close(std::int64_t chain_id);
  // 群任务（认领制；done 终态留痕不删行）
  struct GroupTask {
    std::int64_t id{0};
    std::uint64_t group_id{0};
    std::string title;
    std::string assignee; // 空=待认领
    std::int64_t due_ms{0};
    std::int64_t claimed_ms{0}; // 认领留痕时刻
    std::string status; // todo｜done
    std::string created_by;
    std::int64_t created_ms{0};
    std::string done_by;
    std::int64_t done_ms{0};
  };
  // 建（title 非空；assignee 非空须为群成员）——非法返回 0
  std::int64_t gtask_create(std::uint64_t group_id, const std::string& title,
                            const std::string& assignee, std::int64_t due_ms,
                            const std::string& by, std::int64_t ts_ms);
  std::vector<GroupTask> gtasks_list(std::uint64_t group_id); // id DESC
  // 认领（todo 且无人认领；已占=false）
  bool gtask_claim(std::int64_t id, const std::string& account,
                   std::int64_t ts_ms);
  // 完成（assignee/created_by/群主·管理员由路由层预判后传入记账；
  // 仅 todo 可完成）
  bool gtask_done(std::int64_t id, const std::string& done_by,
                  std::int64_t ts_ms);
