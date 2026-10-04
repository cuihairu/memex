#include "local_store.hpp"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace memex::client {

LocalStore::~LocalStore() { close(); }

bool LocalStore::open(const QString& path) {
  if (open_) return false;
  // 首跑时应用数据目录可能不存在（SQLite 不会自动建目录）
  if (!path.startsWith(QChar(':'))) {
    QDir().mkpath(QFileInfo(path).absolutePath());
  }
  connection_name_ = QStringLiteral("memex-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name_);
    db.setDatabaseName(path);
    if (!db.open()) {
      qWarning() << "[本地库] 打开失败：" << db.lastError().text();
      QSqlDatabase::removeDatabase(connection_name_);
      connection_name_.clear();
      return false;
    }
  }
  open_ = true;
  if (!ensure_schema()) {
    close();
    return false;
  }
  return true;
}

void LocalStore::close() {
  if (!open_) return;
  {
    QSqlDatabase db = QSqlDatabase::database(connection_name_, false);
    if (db.isOpen()) db.close();
  }
  QSqlDatabase::removeDatabase(connection_name_);
  connection_name_.clear();
  open_ = false;
}

bool LocalStore::ensure_schema() {
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  const bool ok =
      q.exec(QStringLiteral(
          "CREATE TABLE IF NOT EXISTS messages ("
          " id INTEGER PRIMARY KEY AUTOINCREMENT,"
          " peer TEXT NOT NULL,"
          " from_id TEXT NOT NULL,"
          " to_id TEXT NOT NULL,"
          " seq INTEGER NOT NULL,"
          " ts_ms INTEGER NOT NULL,"
          " text TEXT NOT NULL,"
          " source TEXT NOT NULL,"
          " msg_id TEXT NOT NULL DEFAULT '',"
          " recalled INTEGER NOT NULL DEFAULT 0,"
          " UNIQUE(from_id, seq))"));
  if (!ok) {
    qWarning() << "[本地库] 建表失败：" << q.lastError().text();
    return false;
  }
  // 旧库迁移：补 msg_id 列（已存在则忽略失败）
  q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN msg_id TEXT NOT NULL DEFAULT ''"));
  // 旧库迁移：补 recalled 列
  q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN recalled INTEGER NOT NULL DEFAULT 0"));
  // msg_id 去重索引（部分索引：直连消息 msg_id 为空不参与）
  if (!q.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_messages_msg_id "
                             "ON messages(msg_id) WHERE msg_id != ''"))) {
    qWarning() << "[本地库] msg_id 索引失败：" << q.lastError().text();
    return false;
  }
  // 对端维度查询索引（会话列表与历史加载）
  return q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_peer_ts "
                               "ON messages(peer, ts_ms)"));
// —— R23-1 存储抽象层基础：文件表（元文件记录、配额、秒传键）
// 客户端仅落地元数据；对象存储交互通过 memex server 完成（直连对象存储被禁）。
  q.exec(
      "CREATE TABLE IF NOT EXISTS files ("
      " id INTEGER PRIMARY KEY AUTOINCREMENT,"
      " owner TEXT NOT NULL,"
      " belong_gid TEXT NOT NULL DEFAULT '',"
      " belong_uid TEXT NOT NULL DEFAULT '',"
      " file_name TEXT NOT NULL,"
      " file_size INTEGER NOT NULL DEFAULT 0,"
      " file_hash TEXT NOT NULL DEFAULT '',"
      " object_key TEXT NOT NULL DEFAULT '',"
      " source TEXT NOT NULL DEFAULT '',"
      " upload_ts INTEGER NOT NULL DEFAULT 0,"
      " status INTEGER NOT NULL DEFAULT 0,"
      " UNIQUE(file_hash, owner))");
  // 群配额表：每个群的使用字节数
  q.exec(
      "CREATE TABLE IF NOT EXISTS group_quota ("
      " gid TEXT PRIMARY KEY,"
      " used_bytes INTEGER NOT NULL DEFAULT 0)");
  // 用户配额表：每个人的使用字节数
  q.exec(
      "CREATE TABLE IF NOT EXISTS user_quota ("
      " uid TEXT PRIMARY KEY,"
      " used_bytes INTEGER NOT NULL DEFAULT 0)");
  // 对文件表查询索引（按归属与哈希检索）
  return q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_owner "
                               "ON files(owner)"));
}

bool LocalStore::append(const StoredMessage& msg, bool* inserted) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO messages"
      " (peer, from_id, to_id, seq, ts_ms, text, source, msg_id, recalled)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
  q.bindValue(0, QString::fromStdString(msg.peer));
  q.bindValue(1, QString::fromStdString(msg.from));
  q.bindValue(2, QString::fromStdString(msg.to));
  q.bindValue(3, static_cast<qint64>(msg.seq));
  q.bindValue(4, msg.ts_ms);
  q.bindValue(5, QString::fromStdString(msg.text));
  q.bindValue(6, QString::fromStdString(msg.source));
  q.bindValue(7, QString::fromStdString(msg.msg_id));
  q.bindValue(8, msg.recalled ? 1 : 0);
  if (!q.exec()) {
    qWarning() << "[本地库] 写入失败：" << q.lastError().text();
    if (inserted) *inserted = false;
    return false;
  }
  if (inserted) *inserted = q.numRowsAffected() > 0; // IGNORE 命中即 0
  return true;
}

qint64 LocalStore::next_local_seq(const std::string& from_id) {
  if (!open_) return 0;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "SELECT COALESCE(MAX(seq), 0) + 1 FROM messages WHERE from_id = ?"));
  q.addBindValue(QString::fromStdString(from_id));
  if (!q.exec() || !q.next()) {
    qWarning() << "[本地库] 分配 seq 失败：" << q.lastError().text();
    return 0;
  }
  return q.value(0).toLongLong();
}

QList<StoredMessage> LocalStore::history(const QString& peer, int limit) const {
  QList<StoredMessage> out;
  if (!open_) return out;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  // limit 取最近 N 条，整体按时间正序返回（聊天窗渲染方向）
  q.prepare(QStringLiteral(
      "SELECT id, peer, from_id, to_id, seq, ts_ms, text, source, msg_id, recalled FROM ("
      " SELECT * FROM messages WHERE peer = ?"
      " ORDER BY ts_ms DESC, id DESC LIMIT ?)"
      " ORDER BY ts_ms ASC, id ASC"));
  q.addBindValue(peer);
  q.addBindValue(limit);
  if (!q.exec()) {
    qWarning() << "[本地库] 查询失败：" << q.lastError().text();
    return out;
  }
  while (q.next()) {
    StoredMessage m;
    m.id = q.value(0).toLongLong();
    m.peer = q.value(1).toString().toStdString();
    m.from = q.value(2).toString().toStdString();
    m.to = q.value(3).toString().toStdString();
    m.seq = static_cast<std::uint64_t>(q.value(4).toLongLong());
    m.ts_ms = q.value(5).toLongLong();
    m.text = q.value(6).toString().toStdString();
    m.source = q.value(7).toString().toStdString();
    m.msg_id = q.value(8).toString().toStdString();
    m.recalled = q.value(9).toInt() != 0;
    out.push_back(std::move(m));
  }
  return out;
}

QStringList LocalStore::peers(const QString& source) const {
  QStringList out;
  if (!open_) return out;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  // 每对端取最近一条的时间排序（会话列表口径）
  if (source.isEmpty()) {
    q.prepare(QStringLiteral(
        "SELECT peer FROM messages GROUP BY peer"
        " ORDER BY MAX(ts_ms) DESC, peer ASC"));
  } else {
    q.prepare(QStringLiteral(
        "SELECT peer FROM messages WHERE source = ?"
        " GROUP BY peer ORDER BY MAX(ts_ms) DESC, peer ASC"));
    q.addBindValue(source);
  }
  if (!q.exec()) {
    qWarning() << "[本地库] 对端列表查询失败：" << q.lastError().text();
    return out;
  }
  while (q.next()) out.push_back(q.value(0).toString());
  return out;
}

bool LocalStore::mark_recalled(const std::string& msg_id) {
  if (!open_ || msg_id.empty()) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "UPDATE messages SET recalled = 1 WHERE msg_id = ?"));
  q.addBindValue(QString::fromStdString(msg_id));
  if (!q.exec()) {
    qWarning() << "[本地库] 撤回标记失败：" << q.lastError().text();
    return false;
  }
  return q.numRowsAffected() > 0;
}

// —— R23-2 群文件 + 个人文件（内网全功能）客户端基础
// 文件增删改查与配额操作，均落地本地 SQLite（元数据层）；
// 对象存储交互通过 memex server 完成，客户端仅管理本地记录。

bool LocalStore::add_file(const QString& owner, const QString& belong_gid,
                          const QString& belong_uid, const QString& file_name,
                          qint64 file_size, const QString& file_hash,
                          const QString& object_key, const QString& source) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO files"
      " (owner, belong_gid, belong_uid, file_name, file_size, file_hash,"
      " object_key, source, upload_ts, status)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
  q.bindValue(0, owner);
  q.bindValue(1, belong_gid);
  q.bindValue(2, belong_uid);
  q.bindValue(3, file_name);
  q.bindValue(4, file_size);
  q.bindValue(5, file_hash);
  q.bindValue(6, object_key);
  q.bindValue(7, source);
  q.bindValue(8, QDateTime::currentMSecsSinceEpoch());
  q.bindValue(9, static_cast<int>(0)); // status: 0=normal
  if (!q.exec()) {
    qWarning() << "[本地库] 写入文件记录失败：" << q.lastError().text();
    return false;
  }
  return true;
}

QList<QSqlRecord> LocalStore::file_list(const QString& owner,
                                        const QString& belong_gid) const {
  if (!open_) return QList<QSqlRecord>();
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  if (!belong_gid.isEmpty()) {
    q.prepare(QStringLiteral(
        "SELECT * FROM files WHERE owner = ? AND belong_gid = ?"));
    q.addBindValue(owner);
    q.addBindValue(belong_gid);
  } else {
    q.prepare(QStringLiteral("SELECT * FROM files WHERE owner = ?"));
    q.addBindValue(owner);
  }
  if (!q.exec()) {
    qWarning() << "[本地库] 查询文件列表失败：" << q.lastError().text();
    return QList<QSqlRecord>();
  }
  QList<QSqlRecord> out;
  while (q.next()) out.append(q.record());
  return out;
}

bool LocalStore::update_group_quota(const QString& gid, qint64 used_bytes) {
  if (!open_) return false;
  // 先取旧值
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral("SELECT used_bytes FROM group_quota WHERE gid = ?"));
  q.addBindValue(gid);
  if (!q.exec() || !q.next()) {
    // 无记录则新增
    QSqlQuery ins(QSqlDatabase::database(connection_name_));
    ins.prepare(QStringLiteral(
        "INSERT INTO group_quota (gid, used_bytes) VALUES (?, ?)"));
    ins.bindValue(0, gid);
    ins.bindValue(1, used_bytes);
    if (!ins.exec()) {
      qWarning() << "[本地库] 写入群配额失败：" << q.lastError().text();
      return false;
    }
    return true;
  }
  q.prepare(QStringLiteral(
      "UPDATE group_quota SET used_bytes = ? WHERE gid = ?"));
  q.addBindValue(used_bytes);
  q.addBindValue(gid);
  if (!q.exec()) {
    qWarning() << "[本地库] 更新群配额失败：" << q.lastError().text();
    return false;
  }
  return true;
}

bool LocalStore::update_user_quota(const QString& uid, qint64 used_bytes) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral("SELECT used_bytes FROM user_quota WHERE uid = ?"));
  q.addBindValue(uid);
  if (!q.exec() || !q.next()) {
    QSqlQuery ins(QSqlDatabase::database(connection_name_));
    ins.prepare(QStringLiteral(
        "INSERT INTO user_quota (uid, used_bytes) VALUES (?, ?)"));
    ins.bindValue(0, uid);
    ins.bindValue(1, used_bytes);
    if (!ins.exec()) {
      qWarning() << "[本地库] 写入用户配额失败：" << q.lastError().text();
      return false;
    }
    return true;
  }
  q.prepare(QStringLiteral(
      "UPDATE user_quota SET used_bytes = ? WHERE uid = ?"));
  q.addBindValue(used_bytes);
  q.addBindValue(uid);
  if (!q.exec()) {
    qWarning() << "[本地库] 更新用户配额失败：" << q.lastError().text();
    return false;
  }
  return true;
}

} // namespace memex::client
