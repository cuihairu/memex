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

} // namespace memex::client
