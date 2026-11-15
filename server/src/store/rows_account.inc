// store.hpp 行类型片段（LAY-010 自 store.hpp 逐字搬移）：账号/登录记录/设备行；由 store.hpp 按位包含，无包含卫、勿单独编译。

struct AccountRow {
  std::string account;
  std::string display_name;
  std::string salt_hex;
  std::string digest_hex;
  std::string role; // admin／member（权限分级，T3.1）
};

struct LoginRecord {
  std::int64_t id{0};
  std::string account;
  std::string fingerprint; // 设备指纹（SHA-256 hex）
  std::string kind;        // 设备类型（desktop／mobile）
  std::string name;        // 设备名（主机名）
  std::string source_ip;   // 来源地址
  std::string version;     // 客户端版本
  std::string result;      // ok／bad_password／no_account／device_disabled
  std::int64_t ts_ms{0};
};

// 设备台账（T3.3）：设备首次成功登录自动建档；启停对登录即时生效
//（停用＝拒绝登录，解绑＝清责任人并停用）。
struct DeviceRow {
  std::string fingerprint;
  std::string kind;
  std::string name;
  std::string owner_account; // 责任人（管理端登记；空=未登记）
  bool enabled{true};
  std::int64_t first_seen_ms{0};
  std::int64_t last_seen_ms{0};
};
