#include "local_store.hpp"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace memex::client {

LocalStore::~LocalStore() { close(); }

bool LocalStore::open(const QString& path) {
  if (open_) return false;
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
          " UNIQUE(from_id, seq))"));
  if (!ok) {
    qWarning() << "[本地库] 建表失败：" << q.lastError().text();
    return false;
  }
  // 对端维度查询索引（会话列表与历史加载）
  return q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_peer_ts "
                               "ON messages(peer, ts_ms)"));
}

bool LocalStore::append(const StoredMessage& msg) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO messages"
      " (peer, from_id, to_id, seq, ts_ms, text, source)"
      " VALUES (?, ?, ?, ?, ?, ?, ?)"));
  q.bindValue(0, QString::fromStdString(msg.peer));
  q.bindValue(1, QString::fromStdString(msg.from));
  q.bindValue(2, QString::fromStdString(msg.to));
  q.bindValue(3, static_cast<qint64>(msg.seq));
  q.bindValue(4, msg.ts_ms);
  q.bindValue(5, QString::fromStdString(msg.text));
  q.bindValue(6, QString::fromStdString(msg.source));
  if (!q.exec()) {
    qWarning() << "[本地库] 写入失败：" << q.lastError().text();
    return false;
  }
  return true;
}

QList<StoredMessage> LocalStore::history(const QString& peer, int limit) const {
  QList<StoredMessage> out;
  if (!open_) return out;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  // limit 取最近 N 条，整体按时间正序返回（聊天窗渲染方向）
  q.prepare(QStringLiteral(
      "SELECT id, peer, from_id, to_id, seq, ts_ms, text, source FROM ("
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
    out.push_back(std::move(m));
  }
  return out;
}

} // namespace memex::client
