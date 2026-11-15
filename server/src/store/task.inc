// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：个人任务域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— R27-1 个人任务清单（tasks：谁的任务归谁的清单，分配=别人建到
  //     你清单上；完成/提醒回执归清单主人，撤回归创建人）——
  struct TaskRow {
    std::int64_t id{0};
    std::string owner;      // 清单主人（任务落在谁的清单）
    std::string creator;    // 创建人（自建=owner；他人分配=分配人）
    std::string title;
    std::string note;       // 备注（可空）
    std::int64_t due_ms{0}; // 0=未设提醒
    std::int64_t reminded_ms{0}; // 0=未提醒（客户端本地通知后回执落笔）
    bool done{false};
    std::int64_t done_ms{0};
    std::int64_t created_ms{0};
    std::string provider;   // R27-2 外部任务引用（provider id，空=本地任务）
    std::string ext_key;    // 外部键原文（「project#键」或完整链接；与
                            // provider 成对；详情 URL 由客户端 SPI 解析）
    // R27-6：开始时间（0=未设）、类型（服务端配置清单）、状态
    //（待办/进行中/完成——done 列保留写双份过渡，读取以 status 为准）
    std::int64_t start_ms{0};
    std::string type{"任务"};
    std::string status{"待办"};
    std::int64_t start_reminded_ms{0}; // 开始到点提醒回执（0=未提醒）
  };
  // R27-6 任务类型清单（服务端配置，admin 后台增删改；初始四类
  // 任务/需求/缺陷/其他，默认「任务」——桌面/手机从服务端拉取）
  struct TaskType {
    std::string name;
    bool is_default{false};
  };
  // 建任务（owner 与 creator 都须为已建账号；标题非空；provider/ext_key
  // 成对缺省=本地任务）
  std::int64_t task_create(const std::string& owner,
                           const std::string& creator,
                           const std::string& title, const std::string& note,
                           std::int64_t due_ms, std::int64_t created_ms,
                           const std::string& provider = "",
                           const std::string& ext_key = "",
                           const std::string& type = "任务",
                           std::int64_t start_ms = 0);
  // 我的清单（含别人派来的；id 倒序）
  std::vector<TaskRow> tasks_of(const std::string& owner);
  // 我派给别人的（creator=本人且 owner≠本人；id 倒序）
  std::vector<TaskRow> tasks_assigned_by(const std::string& creator);
  std::optional<TaskRow> task_by_id(std::int64_t id);
  // 完成/回退（回执带 done_ms；0/非 0 由调用方语义决定，此处按 bool；
  // R27-6 双写过渡：done=1↔status=完成、done=0↔待办）
  bool task_set_done(std::int64_t id, bool done, std::int64_t done_ms);
  // R27-6 状态切换（待办/进行中/完成；完成↔done 双写，其余 done=0；
  // 写双份过渡期口径，读取以 status 为准）
  bool task_set_status(std::int64_t id, const std::string& status,
                       std::int64_t done_ms);
  // 提醒回执（只落一次时刻；重复调用以最早为准不回退）
  bool task_mark_reminded(std::int64_t id, std::int64_t reminded_ms);
  // R27-6 开始到点提醒回执（同上口径，独立位不与 deadline 回执互抢）
  bool task_mark_start_reminded(std::int64_t id, std::int64_t reminded_ms);
  bool task_delete(std::int64_t id);
  // —— R27-6 任务类型清单（服务端配置）——
  std::vector<TaskType> task_types();
  std::string task_type_default(); // 清单默认类型（空=清单异常，调用方落回「任务」）
  bool task_type_add(const std::string& name); // 空/重名=false
  bool task_type_delete(const std::string& name); // 默认类型/有任务引用=false
  // 分配资格数据面（R27-1 权限模型「谁能分配」）：两账号是否同任一群
  // 的共同成员（现查现裁=入群即授权、退群即失）
  bool co_members(const std::string& a, const std::string& b);
