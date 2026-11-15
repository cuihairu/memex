// store.hpp 类体片段（LAY-010 自 store.hpp 逐字搬移）：文件存储元数据域；由 store.hpp 按位包含，无包含卫、勿单独编译。
  // —— R23-1 文件存储元数据（服务端权限/配额判断层） ——
  // 文件来源：internal（内网上传）、uplink（外网单向上传）
  enum class FileSource : int { Internal = 0, Uplink = 1 };
  // 文件状态：normal、quarantine（隔离/杀毒待审）、expired（过期清理）
  enum class FileStatus : int { Normal = 0, Quarantine = 1, Expired = 2 };

  // 文件类目（R23-3）：Personal=常规个人/群空间；Inbox=文件助手本人收件箱
  //（手机发自己=文件传输落这里）。类目入秒传键：同属主同哈希的收件箱文件
  // 与个人空间文件是两个空间，互不秒传串用。
  enum class FileKind : int { Personal = 0, Inbox = 1 };

  struct FileMeta {
    int64_t id{0};
    std::string owner;
    std::string belong_gid;
    std::string belong_uid;
    std::string file_name;
    int64_t file_size{0};
    std::string file_hash;
    std::string object_key;
    FileSource source{FileSource::Internal};
    int64_t upload_ts{0};
    FileStatus status{FileStatus::Normal};
    bool pin{false};  // 置顶（群主/管理员管群文件、本人管个人文件）
    FileKind kind{FileKind::Personal};
  };

  struct QuotaInfo {
    std::string gid;
    std::string uid;
    int64_t used_bytes{0};
    int64_t limit_bytes{0};
  };

  int64_t create_file_meta(const FileMeta& meta);
  // 秒传按「属主+哈希+归属+类目」判（R23-2：换群/换人空间不算命中；
  // R23-3：收件箱与个人空间互不串用）。kind 缺省 Personal 兼容既有调用。
  std::optional<FileMeta> check_second_transfer(const std::string& owner,
                                                const std::string& file_hash,
                                                const std::string& belong_gid,
                                                const std::string& belong_uid,
                                                FileKind kind = FileKind::Personal);
  // kind=-1 不按类目过滤（缺省）；0/1 只列对应类目
  std::vector<FileMeta> list_files(const std::string& belong_gid,
                                   const std::string& belong_uid,
                                   int limit = 200, int offset = 0,
                                   int kind = -1);
  std::optional<FileMeta> file_by_id(int64_t file_id);
  bool delete_file_meta(int64_t file_id);
  bool set_file_pin(int64_t file_id, bool pin);
  bool set_file_status(int64_t file_id, FileStatus status);
  // 同一对象键仍被多少文件行引用（删除字节前判引用：内容寻址下多行可共用对象）
  int64_t count_file_refs(const std::string& object_key);
  std::optional<QuotaInfo> get_group_quota(const std::string& gid);
  std::optional<QuotaInfo> get_user_quota(const std::string& uid);
  bool add_group_quota_used(const std::string& gid, int64_t delta_bytes);
  bool add_user_quota_used(const std::string& uid, int64_t delta_bytes);
  // 受检扣费（原子）：limit=0 不限；used+delta 超限即拒（changes=0）。
  // 上传受理前用这个，退额（删除）用上面的 add_*（不设限）。
  bool charge_group_quota(const std::string& gid, int64_t delta_bytes);
  bool charge_user_quota(const std::string& uid, int64_t delta_bytes);
  bool set_group_quota_limit(const std::string& gid, int64_t limit_bytes);
  bool set_user_quota_limit(const std::string& uid, int64_t limit_bytes);

  struct UplinkLog {
    int64_t id{0};
    std::string uploader;
    std::string file_name;
    int64_t file_size{0};
    std::string file_hash;
    std::string object_key;
    int64_t upload_ts{0};
  };
  // 群内角色（权限模型：群主/管理员/成员）：""=非成员；owner 由 groups.owner 判
  std::string group_role(std::uint64_t group_id, const std::string& account);
  bool group_set_role(std::uint64_t group_id, const std::string& account,
                      const std::string& role); // member|admin（owner 行拒改）
  bool add_uplink_log(const UplinkLog& log);
  std::vector<UplinkLog> list_uplink_logs(const std::string& uploader,
                                          int limit = 200);
  // 外网 uplink 面的「我的上传」列表：只列 source=uplink 的收件箱文件
  //（/uplink/mine 数据源；uplink 面无任何读内网数据的端点，本查询只回
  // 本人自己的记录）。按 id 倒序（上传时间同源）。
  std::vector<FileMeta> list_uplink_files(const std::string& owner,
                                          int limit = 200, int offset = 0);
