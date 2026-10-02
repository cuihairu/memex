// 共享内核：本地消息库（SQLite）。
// 归档铁律在本地库的体现：source 字段区分「直连（仅存本机）」与「协作（另有服务端归档）」，
// 本地删除只影响本机，不涉及服务端归档。
#pragma once

#include <QList>
#include <QString>

#include <cstdint>
#include <string>

namespace memex::client {

struct StoredMessage {
  qint64 id{0};
  std::uint64_t seq{0};
  std::string peer;   // 会话对端（查询维度）
  std::string from;
  std::string to;
  qint64 ts_ms{0};
  std::string text;
  std::string source; // "direct" 或 "collab"
};

class LocalStore {
public:
  LocalStore() = default;
  ~LocalStore();

  LocalStore(const LocalStore&) = delete;
  LocalStore& operator=(const LocalStore&) = delete;

  // path 支持 ":memory:"（测试）与普通文件路径。重复 open 为幂等失败。
  bool open(const QString& path);
  void close();
  bool is_open() const { return open_; }

  // 幂等插入（from+seq 唯一约束去重）。
  bool append(const StoredMessage& msg);

  // 某对端的本地历史：取最近 limit 条，按时间正序返回。
  QList<StoredMessage> history(const QString& peer, int limit = 200) const;

private:
  bool ensure_schema();

  QString connection_name_;
  bool open_{false};
};

} // namespace memex::client
