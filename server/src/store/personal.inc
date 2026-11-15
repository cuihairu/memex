// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：个人素材与位置图域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— 表情包素材（需求批②：个人素材，仅本人读写——收藏随账号走）——
  // 字节以 BLOB 落库（≤1MiB/张，与品牌素材同口径；大文件仍走对象存储面）
  struct EmojiAsset { // 列表面（无字节）
    std::int64_t id{0};
    std::string name;
    std::int64_t size{0};
    std::int64_t ts_ms{0};
  };
  // 新增素材；回 id（失败 -1）
  std::int64_t emoji_add(const std::string& account, const std::string& name,
                         const std::vector<unsigned char>& bytes,
                         std::int64_t ts_ms);
  // 该账号素材（ts 倒序）
  std::vector<EmojiAsset> emoji_list(const std::string& account);
  // 取字节：仅资产属主可取（他人/不存在 false）
  bool emoji_bytes(std::int64_t id, const std::string& account,
                   std::vector<unsigned char>& out, std::string& name_out);
  // 删除：仅属主（非属主/不存在 false）
  bool emoji_delete(std::int64_t id, const std::string& account);

  // —— 用户头像（需求批⑫）：每账号四档尺寸（32/64/128/256）——
  // 独立成表不并入 member_profiles：BLOB 行不跟资料行一起被 ORG_QUERY
  // 全量扫读；版本戳=该账号各档行的 MAX(ts_ms)（无行=0，删除行即回落
  // 默认头像，缓存失效语义随行生灭，无需另立 ver 列与迁移）。
  // 上传（裁剪+四档生成在客户端完成，服务端只验 magic/上限/档位）与
  // 删除＝本人属主裁决；读面＝登录成员可读（组织内互见，同资料面口径）。
  static constexpr int kAvatarSizes[] = {32, 64, 128, 256};
  static bool avatar_size_valid(int size);
  // 写入/覆盖一档（INSERT OR REPLACE）；ts_ms 同时是该档的版本成分
  bool avatar_put(const std::string& account, int size, const std::string& mime,
                  const std::vector<unsigned char>& bytes, std::int64_t ts_ms);
  // 取字节（登录成员可读口径由路由层判权，存储层不问来者）
  bool avatar_bytes(const std::string& account, int size,
                    std::vector<unsigned char>& out, std::string& mime_out);
  // 清除全部档位（真删了行才 true——无头像可删 false，路由层回 404）
  bool avatar_clear(const std::string& account);
  // 头像版本戳：各档行 MAX(ts_ms)，无行=0（ORG_QUERY 逐成员带出）
  std::int64_t avatar_ver(const std::string& account) const;

  // —— 二期·办公室位置图（设计稿 docs/design/办公室位置图.md）——
  // 抽象平面（网格归一化坐标 0~1，不画真实底图）；工位即楼层归属
  //（本人楼层=本人占用工位所在楼层，不从部门推导）；编辑权 org-admin
  //（az office:manage 在路由层裁）
  struct SeatRow {
    std::int64_t id{0};
    std::string floor;      // 楼层号（"3F"）
    std::string label;      // 工位号（UNIQUE(floor,label)）
    double x{0};            // 归一化坐标 0~1
    double y{0};
    std::string account;    // 占用者（空=空位；一人一工位=部分唯一索引）
  };
  // 建/改位（拖拽落位即 upsert；UNIQUE(floor,label) 冲突=改坐标）→ id
  std::int64_t seat_upsert(const std::string& floor, const std::string& label,
                           double x, double y, std::int64_t ts_ms);
  bool seat_delete(std::int64_t id);
  // 绑定/解绑占用者（account 空=解绑；幽灵拒；一人一工位=换座先解绑）
  bool seat_bind(std::int64_t id, const std::string& account,
                 std::int64_t ts_ms);
  std::vector<SeatRow> seats_on_floor(const std::string& floor);
  std::optional<SeatRow> seat_of(const std::string& account);

  // —— T4.2 跨态会话 ——
  // 建立（start）：插入一行进行中记录（同会话重复 start 只记首条）。
  bool cross_log_start(const std::string& account,
                       const std::string& peer_device,
                       const std::string& peer_name, std::int64_t started_ms);
  // 结束（end）：按 (账号, 对端, 建立时刻) 闭环最早一条未结束记录，算时长。
  bool cross_log_end(const std::string& account,
                     const std::string& peer_device, std::int64_t started_ms,
                     std::int64_t ended_ms);
  // 全量日志（倒序；进行中 ended_ms=0、duration_ms=0）
  std::vector<CrossLogRow> cross_logs(int limit = 200);
  // 归档起点（A8）：首条归档消息之前最近一次成功登录时刻——即该账号
  // 进入协作态、归档开始的时刻；无归档返回 0；有归档但无登录记录
  //（旧库数据）退化为首条归档消息时刻。
  std::int64_t archive_start_ms(const std::string& account);

  // —— T4.5 常用联系人 ——
  // 星标／取消星标（UPSERT，仅改 starred，不刷新 last_ms）
  bool fav_star(const std::string& account, const std::string& peer,
                bool starred);
  // 最近联系刷新（UPSERT，last_ms 取 max；已有 starred 不动）
  bool fav_touch(const std::string& account, const std::string& peer,
                 std::int64_t ts_ms);
  // 全量列表：星标置顶（last_ms 新者先）、未星标按 last_ms 倒序、peer 兜底
  std::vector<FavRow> fav_list(const std::string& account);
