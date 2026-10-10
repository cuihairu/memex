#include "local_store.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
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
  // 平台-9：补 sync_state 列（蓝图§二十三 Sync State；空=历史行不参与恢复）
  q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN sync_state TEXT NOT NULL DEFAULT ''"));
  // 需求批⑦：补 receipt 列（我发出消息的回执态：''/delivered/read；
  // 空=未启用或非我发出——接收方向没有回执概念）
  q.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN receipt TEXT NOT NULL DEFAULT ''"));
  // msg_id 去重索引（部分索引：直连消息 msg_id 为空不参与）
  if (!q.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_messages_msg_id "
                             "ON messages(msg_id) WHERE msg_id != ''"))) {
    qWarning() << "[本地库] msg_id 索引失败：" << q.lastError().text();
    return false;
  }
  // 对端维度查询索引（会话列表与历史加载）
  if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_peer_ts "
                             "ON messages(peer, ts_ms)"))) {
    qWarning() << "[本地库] peer_ts 索引失败：" << q.lastError().text();
    return false;
  }
// —— R23-1 存储抽象层基础：文件表（元文件记录、配额、秒传键）
// 客户端仅落地元数据；对象存储交互通过 memex server 完成（直连对象存储被禁）。
// （原此处误留 return，其后建表全部成为死代码——files/配额表从未落地，
//  平台-8 device_identity 首个真实使用者踩爆，顺手修复）
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
  q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_files_owner "
                        "ON files(owner)"));
  // 平台-8 直连安全：设备身份（单行）与对端身份定针（TOFU）
  q.exec(
      "CREATE TABLE IF NOT EXISTS device_identity ("
      " id INTEGER PRIMARY KEY CHECK (id = 1),"
      " seed_hex TEXT NOT NULL,"
      " pub_hex TEXT NOT NULL,"
      " created_ms INTEGER NOT NULL DEFAULT 0)");
  return q.exec(
      "CREATE TABLE IF NOT EXISTS direct_peer_keys ("
      " device_id TEXT PRIMARY KEY,"
      " pub_hex TEXT NOT NULL,"
      " first_seen_ms INTEGER NOT NULL DEFAULT 0)");
}

bool LocalStore::append(const StoredMessage& msg, bool* inserted) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO messages"
      " (peer, from_id, to_id, seq, ts_ms, text, source, msg_id, recalled, sync_state)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
  q.bindValue(0, QString::fromStdString(msg.peer));
  q.bindValue(1, QString::fromStdString(msg.from));
  q.bindValue(2, QString::fromStdString(msg.to));
  q.bindValue(3, static_cast<qint64>(msg.seq));
  q.bindValue(4, msg.ts_ms);
  q.bindValue(5, QString::fromStdString(msg.text));
  q.bindValue(6, QString::fromStdString(msg.source));
  q.bindValue(7, QString::fromStdString(msg.msg_id));
  q.bindValue(8, msg.recalled ? 1 : 0);
  q.bindValue(9, QString::fromStdString(msg.sync_state));
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
      "SELECT id, peer, from_id, to_id, seq, ts_ms, text, source, msg_id,"
      " recalled, sync_state, receipt FROM ("
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
    m.sync_state = q.value(10).toString().toStdString();
    m.receipt = q.value(11).toString().toStdString();
    out.push_back(std::move(m));
  }
  return out;
}

// 按日期范围的历史（需求批⑧）：与 history 同构（最近 N 条整体正序），
// 差异只在 WHERE 时间窗；时间窗走 idx_messages_peer_ts(peer, ts_ms)。
QList<StoredMessage> LocalStore::history_between(const QString& peer,
                                                 qint64 from_ms, qint64 until_ms,
                                                 int limit) const {
  QList<StoredMessage> out;
  if (!open_) return out;
  QString where = QStringLiteral("peer = ?");
  if (from_ms > 0) where += QStringLiteral(" AND ts_ms >= ?");
  if (until_ms > 0) where += QStringLiteral(" AND ts_ms <= ?");
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "SELECT id, peer, from_id, to_id, seq, ts_ms, text, source, msg_id,"
      " recalled, sync_state, receipt FROM ("
      " SELECT * FROM messages WHERE %1"
      " ORDER BY ts_ms DESC, id DESC LIMIT ?)"
      " ORDER BY ts_ms ASC, id ASC")
                .arg(where));
  q.addBindValue(peer);
  if (from_ms > 0) q.addBindValue(from_ms);
  if (until_ms > 0) q.addBindValue(until_ms);
  q.addBindValue(limit);
  if (!q.exec()) {
    qWarning() << "[本地库] 日期范围查询失败：" << q.lastError().text();
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
    m.sync_state = q.value(10).toString().toStdString();
    m.receipt = q.value(11).toString().toStdString();
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

// —— 平台-9 同步状态机 ——

bool LocalStore::set_sync_state(const std::string& from_id, std::uint64_t seq,
                                const std::string& state) {
  if (!open_ || from_id.empty() || state.empty()) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "UPDATE messages SET sync_state = ? WHERE from_id = ? AND seq = ?"));
  q.addBindValue(QString::fromStdString(state));
  q.addBindValue(QString::fromStdString(from_id));
  q.addBindValue(static_cast<qint64>(seq));
  if (!q.exec()) {
    qWarning() << "[本地库] 同步状态推移失败：" << q.lastError().text();
    return false;
  }
  return q.numRowsAffected() > 0;
}

bool LocalStore::set_sync_state_archived(const std::string& msg_id) {
  // 发送侧归档推进：只从 SERVER_ACKED 走（其余态不倒退不越级）
  if (!open_ || msg_id.empty()) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "UPDATE messages SET sync_state = 'ARCHIVED'"
      " WHERE msg_id = ? AND sync_state = 'SERVER_ACKED'"));
  q.addBindValue(QString::fromStdString(msg_id));
  if (!q.exec()) {
    qWarning() << "[本地库] 归档推进失败：" << q.lastError().text();
    return false;
  }
  return q.numRowsAffected() > 0;
}

// —— 需求批⑦ 消息回执 ——

bool LocalStore::set_msg_id(const std::string& from_id, std::uint64_t seq,
                            const std::string& msg_id) {
  // 发出消息落服务端标识（受理前即按 sha256(account:seq) 本地推定写入，
  // 服务端同式派生——两侧一致；只补空位不覆盖既有值）
  if (!open_ || from_id.empty() || msg_id.empty()) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "UPDATE messages SET msg_id = ? WHERE from_id = ? AND seq = ?"
      " AND msg_id = ''"));
  q.addBindValue(QString::fromStdString(msg_id));
  q.addBindValue(QString::fromStdString(from_id));
  q.addBindValue(static_cast<qint64>(seq));
  if (!q.exec()) {
    qWarning() << "[本地库] msg_id 回填失败：" << q.lastError().text();
    return false;
  }
  return q.numRowsAffected() > 0;
}

bool LocalStore::set_receipt(const std::string& msg_id,
                             const std::string& state) {
  // 回执态只升不降：已读是终态，乱序后到的 delivered 通知不倒退展示
  if (!open_ || msg_id.empty()) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "UPDATE messages SET receipt = ? WHERE msg_id = ? AND receipt != 'read'"));
  q.addBindValue(QString::fromStdString(state));
  q.addBindValue(QString::fromStdString(msg_id));
  if (!q.exec()) {
    qWarning() << "[本地库] 回执态写入失败：" << q.lastError().text();
    return false;
  }
  return q.numRowsAffected() > 0;
}

QList<StoredMessage> LocalStore::pending_sync(
    const std::string& from_id) const {
  QList<StoredMessage> out;
  if (!open_ || from_id.empty()) return out;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "SELECT peer, from_id, to_id, seq, ts_ms, text, source, msg_id, sync_state"
      " FROM messages WHERE from_id = ? AND sync_state IN ('PENDING','SENDING')"
      " ORDER BY seq ASC"));
  q.addBindValue(QString::fromStdString(from_id));
  if (!q.exec()) {
    qWarning() << "[本地库] 待同步查询失败：" << q.lastError().text();
    return out;
  }
  while (q.next()) {
    StoredMessage m;
    m.peer = q.value(0).toString().toStdString();
    m.from = q.value(1).toString().toStdString();
    m.to = q.value(2).toString().toStdString();
    m.seq = static_cast<std::uint64_t>(q.value(3).toLongLong());
    m.ts_ms = q.value(4).toLongLong();
    m.text = q.value(5).toString().toStdString();
    m.source = q.value(6).toString().toStdString();
    m.msg_id = q.value(7).toString().toStdString();
    m.sync_state = q.value(8).toString().toStdString();
    out.push_back(std::move(m));
  }
  return out;
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

// —— R23-3 文件助手（备忘录+文件传输，内网）客户端基础
// 统一收件箱：备忘录文本 + 文件传输记录，仅本人可见
// 外网来的文件需在内网人工转发后方可记录

struct FwHelperRecord {
  int64_t id{0};
  QString owner;
  QString memo_text;
  QString file_hash;
  qint64 file_size{0};
  QString object_key;
  int64_t upload_ts{0};
  int status{0}; // 0=normal、1=forwarded、2=expired
};

bool LocalStore::add_helper_record(const QString& owner, const QString& memo_text,
                                   const QString& file_hash, qint64 file_size,
                                   const QString& object_key) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO helper_records"
      " (owner, memo_text, file_hash, file_size, object_key, upload_ts, status)"
      " VALUES (?, ?, ?, ?, ?, ?, ?)"));
  q.bindValue(0, owner);
  q.bindValue(1, memo_text);
  q.bindValue(2, file_hash);
  q.bindValue(3, file_size);
  q.bindValue(4, object_key);
  q.bindValue(5, QDateTime::currentMSecsSinceEpoch());
  q.bindValue(6, static_cast<int>(0)); // status: 0=normal
  if (!q.exec()) {
    qWarning() << "[本地库] 写入文件助手记录失败：" << q.lastError().text();
    return false;
  }
  return true;
}

QList<QSqlRecord> LocalStore::helper_list(const QString& owner) const {
  if (!open_) return QList<QSqlRecord>();
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral("SELECT * FROM helper_records WHERE owner = ? ORDER BY upload_ts DESC"));
  q.addBindValue(owner);
  if (!q.exec()) {
    qWarning() << "[本地库] 查询文件助手记录失败：" << q.lastError().text();
    return QList<QSqlRecord>();
  }
  QList<QSqlRecord> out;
  while (q.next()) out.append(q.record());
  return out;
}

bool LocalStore::mark_helper_read(const QString& record_id) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral("UPDATE helper_records SET status = 1 WHERE id = ?"));
  q.bindValue(0, record_id);
  if (!q.exec()) {
    qWarning() << "[本地库] 标记助手记录已读失败：" << q.lastError().text();
    return false;
  }
  return q.numRowsAffected() > 0;
}

bool LocalStore::mark_helper_deleted(const QString& record_id) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral("DELETE FROM helper_records WHERE id = ?"));
  if (!q.exec()) {
    qWarning() << "[本地库] 删除助手记录失败：" << q.lastError().text();
    return false;
  }
  return q.numRowsAffected() > 0;
}

// —— R23-5 杀毒扫描钩子 + 白名单客户端基础
// 客户端本地白名单与扫描状态，服务端最终裁决（权限层）
// 外网上传默认关闭，开启时须通过配置显式开启并明示范围
// （ScanWhitelist / ScanResult 类型定义见 local_store.hpp）

// 扫描过程态（对照 result 列外的本地进行中状态）
enum class ScanStatus { Unknown = 0, Scanning, Clean, Quarantined, Error };

bool LocalStore::set_scan_whitelist(const ScanWhitelist& whitelist) {
  if (!open_) return false;
  // Store whitelist as JSON in a config table or key-value pair
  // 此处简化：直接写入系统设置备注，实际应持久化到元数据层
  QJsonObject obj;
  obj["allowed_extensions"] = QJsonArray::fromStringList(whitelist.allowed_extensions);
  obj["allowed_mime_types"] = QJsonArray::fromStringList(whitelist.allowed_mime_types);
  obj["max_file_size"] = whitelist.max_file_size;
  QJsonDocument doc(obj);
  // 写入本地配置文件或键值存储
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  settings.setValue("scan_whitelist", doc.toJson());
  return true;
}

std::unique_ptr<ScanWhitelist> LocalStore::get_scan_whitelist() {
  if (!open_) return nullptr;
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  QJsonDocument doc = settings.value("scan_whitelist").toJsonDocument();
  if (doc.isNull()) return nullptr;
  auto whitelist = std::make_unique<ScanWhitelist>();
  QJsonObject obj = doc.object();
  if (obj.contains("allowed_extensions")) {
    const auto ext = obj["allowed_extensions"].toArray();
    for (const auto& v : ext) whitelist->allowed_extensions << v.toString();
  }
  if (obj.contains("allowed_mime_types")) {
    const auto mime = obj["allowed_mime_types"].toArray();
    for (const auto& v : mime) whitelist->allowed_mime_types << v.toString();
  }
  if (obj.contains("max_file_size")) {
    whitelist->max_file_size = obj["max_file_size"].toVariant().toLongLong();
  }
  return whitelist;
}

bool LocalStore::record_scan(const QString& file_hash, ScanResult result, qint64 file_size) {
  if (!open_) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO scan_results"
      " (file_hash, result, file_size, scan_ts)"
      " VALUES (?, ?, ?, ?)"));
  q.bindValue(0, file_hash);
  q.bindValue(1, static_cast<int>(result));
  q.bindValue(2, file_size);
  q.bindValue(3, QDateTime::currentMSecsSinceEpoch());
  if (!q.exec()) {
    qWarning() << "[本地库] 记录扫描结果失败：" << q.lastError().text();
    return false;
  }
  return true;
}

QList<QSqlRecord> LocalStore::scan_history(const QString& file_hash) const {
  if (!open_) return QList<QSqlRecord>();
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "SELECT * FROM scan_results WHERE file_hash = ? ORDER BY scan_ts DESC"));
  q.addBindValue(file_hash);
  if (!q.exec()) {
    qWarning() << "[本地库] 查询扫描历史失败：" << q.lastError().text();
    return QList<QSqlRecord>();
  }
  QList<QSqlRecord> out;
  while (q.next()) out.append(q.record());
  return out;
}

// —— 平台-8 直连安全：设备身份与对端定针 ——

bool LocalStore::read_identity(std::string* seed_hex, std::string* pub_hex) {
  if (!open_ || !seed_hex || !pub_hex) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "SELECT seed_hex, pub_hex FROM device_identity WHERE id = 1"));
  if (!q.exec() || !q.next()) return false;
  *seed_hex = q.value(0).toString().toStdString();
  *pub_hex = q.value(1).toString().toStdString();
  return seed_hex->size() == 64 && pub_hex->size() == 64;
}

bool LocalStore::save_identity(const std::string& seed_hex,
                               const std::string& pub_hex) {
  if (!open_ || seed_hex.size() != 64 || pub_hex.size() != 64) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO device_identity (id, seed_hex, pub_hex, created_ms)"
      " VALUES (1, ?, ?, ?)"));
  q.addBindValue(QString::fromStdString(seed_hex));
  q.addBindValue(QString::fromStdString(pub_hex));
  q.addBindValue(QDateTime::currentMSecsSinceEpoch());
  if (!q.exec()) {
    qWarning() << "[本地库] 设备身份落库失败：" << q.lastError().text();
    return false;
  }
  return true;
}

std::string LocalStore::peer_identity_pub(const std::string& device_id) {
  if (!open_ || device_id.empty()) return {};
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(
      QStringLiteral("SELECT pub_hex FROM direct_peer_keys WHERE device_id = ?"));
  q.addBindValue(QString::fromStdString(device_id));
  if (!q.exec() || !q.next()) return {};
  return q.value(0).toString().toStdString();
}

bool LocalStore::pin_peer_identity(const std::string& device_id,
                                   const std::string& pub_hex) {
  if (!open_ || device_id.empty() || pub_hex.size() != 64) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(QStringLiteral(
      "INSERT OR IGNORE INTO direct_peer_keys (device_id, pub_hex, first_seen_ms)"
      " VALUES (?, ?, ?)"));
  q.addBindValue(QString::fromStdString(device_id));
  q.addBindValue(QString::fromStdString(pub_hex));
  q.addBindValue(QDateTime::currentMSecsSinceEpoch());
  if (!q.exec()) {
    qWarning() << "[本地库] 对端定针失败：" << q.lastError().text();
    return false;
  }
  return true;
}

bool LocalStore::clear_peer_identity(const std::string& device_id) {
  if (!open_ || device_id.empty()) return false;
  QSqlQuery q(QSqlDatabase::database(connection_name_));
  q.prepare(
      QStringLiteral("DELETE FROM direct_peer_keys WHERE device_id = ?"));
  q.addBindValue(QString::fromStdString(device_id));
  if (!q.exec()) {
    qWarning() << "[本地库] 清除定针失败：" << q.lastError().text();
    return false;
  }
  return true;
}

} // namespace memex::client
