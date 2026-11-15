// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：生命周期（open/close）；由 store.hpp 按位包含，无包含卫、勿单独编译。
public:
  ServerStore() = default;
  ~ServerStore();

  ServerStore(const ServerStore&) = delete;
  ServerStore& operator=(const ServerStore&) = delete;

  // 打开（不存在则建库建表）。目录须已存在（部署口径：数据目录由运维创建）。
  bool open(const std::string& path);
  void close();
  bool is_open() const { return db_ != nullptr; }
