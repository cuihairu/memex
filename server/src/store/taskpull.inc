// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：任务源拉取域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— R27-5 服务端任务源集成：拉取配置（owner+provider 唯一；enabled
  // 默认 0=关，数据外发默认关硬口径）、源凭据（只存密文，明文不落库不进
  // 日志）、全局开关（server_flags kv；缺行=关）——
  struct TaskPullConfig {
    std::string owner;      // 配置人＝任务落点（拉到的任务进他的清单）
    std::string provider;   // 任务源 id（现只接 github；其余 400 未接）
    std::string repo;       // 源内范围（GitHub=owner/repo）
    bool enabled{false};
    int interval_s{300};    // 拉取间隔（0=每次 tick 都拉，测试用）
    std::int64_t last_pull_ms{0};
    std::int64_t updated_ms{0};
  };
  bool task_pull_config_set(const TaskPullConfig& c);
  std::vector<TaskPullConfig> task_pull_configs(); // 全量（拉取器遍历用）
  bool task_pull_config_delete(const std::string& owner,
                               const std::string& provider);
  bool task_pull_config_mark_pulled(const std::string& owner,
                                    const std::string& provider,
                                    std::int64_t ts_ms);
  bool task_source_cred_set(const std::string& owner,
                            const std::string& provider,
                            const std::string& sealed_hex, std::int64_t ts_ms);
  bool task_source_cred_delete(const std::string& owner,
                               const std::string& provider);
  // 密文取回（仅供拉取器内存内解密——不暴露给任何 HTTP 面）
  std::optional<std::string> task_source_cred_sealed(const std::string& owner,
                                                     const std::string& provider);
  std::optional<std::string> server_flag_get(const std::string& key);
  bool server_flag_set(const std::string& key, const std::string& value,
                       std::int64_t ts_ms);
  // 外部任务落表（provider/ext_key 定位 upsert：无则插、有则标题/备注
  // 变了才更新——done/reminded 等本侧进度不动；返回是否新增/有变化）
  struct ExternalTaskUpsert {
    std::int64_t id{0};
    bool created{false};
    bool changed{false};
  };
  ExternalTaskUpsert task_upsert_external(const std::string& owner,
                                          const std::string& title,
                                          const std::string& note,
                                          const std::string& provider,
                                          const std::string& ext_key,
                                          std::int64_t created_ms);
