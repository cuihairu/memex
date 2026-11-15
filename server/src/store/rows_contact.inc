// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：常用联系人行；由 store.hpp 按位包含，无包含卫、勿单独编译。
// T4.5 常用联系人：星标置顶＋最近联系，落 SQLite 换机保留。
struct FavRow {
  std::string peer;
  bool starred{false};
  std::int64_t last_ms{0};
};
