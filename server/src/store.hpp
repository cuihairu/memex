// 服务端本地库（SQLite）：账号表与登录记录表。
// 归档消息表在 T2.3 接入同一库文件；本层只做存储，不含业务策略。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

typedef struct sqlite3 sqlite3;

namespace memex::server {

struct AccountRow {
  std::string account;
  std::string display_name;
  std::string salt_hex;
  std::string digest_hex;
};

struct LoginRecord {
  std::int64_t id{0};
  std::string account;
  std::string fingerprint; // 设备指纹（SHA-256 hex）
  std::string kind;        // 设备类型（desktop／mobile）
  std::string name;        // 设备名（主机名）
  std::string source_ip;   // 来源地址
  std::string version;     // 客户端版本
  std::string result;      // ok／bad_password／no_account
  std::int64_t ts_ms{0};
};

class ServerStore {
public:
  ServerStore() = default;
  ~ServerStore();

  ServerStore(const ServerStore&) = delete;
  ServerStore& operator=(const ServerStore&) = delete;

  // 打开（不存在则建库建表）。目录须已存在（部署口径：数据目录由运维创建）。
  bool open(const std::string& path);
  void close();
  bool is_open() const { return db_ != nullptr; }

  // 建账号：成功 true；账号已存在 false（幂等拒绝，不覆盖）。
  bool create_account(const std::string& account, const std::string& password,
                      const std::string& display_name);

  std::optional<AccountRow> find_account(const std::string& account);

  // 登录记录：成功失败都记（result 区分）；全量可查（按账号过滤，空=全部）。
  bool add_login_record(const LoginRecord& rec);
  std::vector<LoginRecord> login_records(const std::string& account,
                                         int limit = 200);

private:
  bool ensure_schema();

  sqlite3* db_{nullptr};
};

} // namespace memex::server
