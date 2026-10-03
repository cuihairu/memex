// 共享内核：本地消息库（SQLite）。
// 归档铁律在本地库的体现：source 字段区分「直连（仅存本机）」与「协作（另有服务端归档）」，
// 本地删除只影响本机，不涉及服务端归档。
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

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
  // 消息标识（协作态：服务端分配；直连态为空）。按其去重：离线补投重复
  // 投递、断线补传重传同一消息都不产生第二条本地记录。
  std::string msg_id;
  // 撤回标记（仅界面展示用，原文保留在本地与服务端归档）。
  bool recalled{false};
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

  // 幂等插入（from+seq 与 msg_id 唯一约束去重）。
  // inserted 非空时回报是否真正插入（false=命中已有记录，重复消息）。
  bool append(const StoredMessage& msg, bool* inserted = nullptr);

  // 服务端起源消息（如通知，无发送方会话 seq）本地分配单调 seq——满足
  // UNIQUE(from_id, seq)；离线重投的去重仍走 msg_id 唯一索引。
  qint64 next_local_seq(const std::string& from_id);

  // 按 msg_id 置撤回标记（服务端撤回事件到达后调用）。
  bool mark_recalled(const std::string& msg_id);

  // 某对端的本地历史：取最近 limit 条，按时间正序返回。
  QList<StoredMessage> history(const QString& peer, int limit = 200) const;

  // 有历史的对端列表（按最近消息时间倒序）。source 过滤：
  // 空=全部，"collab"=协作会话（T2.4 会话列表），"direct"=直连会话。
  QStringList peers(const QString& source = {}) const;

private:
  bool ensure_schema();

  QString connection_name_;
  bool open_{false};
};

} // namespace memex::client
