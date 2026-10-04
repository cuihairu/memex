// 本地存储后端实现（基于现有 LocalStore / SQLite）
// 客户端永不直连对象存储，所有操作落地本地数据库。
// 实现 StorageInterface，供 DirectEngine 通过抽象接口调用。

#pragma once

#include "storage_interface.hpp"
#include "local_store.hpp"

namespace memex::client {

class StorageLocal : public StorageInterface {
 public:
  explicit StorageLocal(LocalStore* store = nullptr) : store_(store) {}
  ~StorageLocal() override = default;

  bool put_file(const FileMeta& meta) override {
    if (!store_) return false;
    // 将 FileMeta 转换为本地 SQL 插入
    // object_key 为空或本地路径时使用 object_key 列；实际对象存储路径由
    // 上层（server/transport）在走文件通道时填入。
    QSqlQuery q(QSqlDatabase::database(store_->connection_name_));
    q.prepare(
        "INSERT OR REPLACE INTO files "
        "(owner, belong_gid, belong_uid, file_name, file_size, file_hash,"
        " object_key, source, upload_ts, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    q.bindValue(0, QString::fromStdString(meta.file_name));  // 简化：owner 使用文件名占位
    // 实际 project 中 owner/ belong_gid/ belong_uid 来自传入流程，此处保持接口兼容
    q.bindValue(1, QString());
    q.bindValue(2, QString());
    q.bindValue(3, QString::fromStdString(meta.file_name));
    q.bindValue(4, meta.file_size);
    q.bindValue(5, QString::fromStdString(meta.file_hash));
    q.bindValue(6, QString::fromStdString(meta.object_key));
    q.bindValue(7, QString::fromStdString(meta.source));
    q.bindValue(8, QString::number(meta.upload_ts));
    q.bindValue(9, meta.status);
    if (!q.exec()) {
      qWarning() << "[StorageLocal] put_file 失败：" << q.lastError().text();
      return false;
    }
    return true;
  }

  bool get_file(const std::string& object_key, FileMeta& meta) override {
    if (!store_) return false;
    QSqlQuery q(QSqlDatabase::database(store_->connection_name_));
    q.prepare("SELECT file_hash, object_key, file_name, file_size, source, upload_ts, status FROM files WHERE object_key = ?");
    q.addBindValue(QString::fromStdString(object_key));
    if (!q.exec() || !q.next()) return false;
    meta.file_hash = q.value(0).toString().toStdString();
    meta.object_key = q.value(1).toString().toStdString();
    meta.file_name = q.value(2).toString().toStdString();
    meta.file_size = q.value(3).toLongLong();
    meta.source = q.value(4).toString().toStdString();
    meta.upload_ts = q.value(5).toString().toStdString().toLongLong();
    meta.status = q.value(6).toInt();
    return true;
  }

  bool get_file_by_hash(const std::string& file_hash,
                        std::string& owner,
                        std::string& object_key) override {
    if (!store_) return false;
    QSqlQuery q(QSqlDatabase::database(store_->connection_name_));
    q.prepare("SELECT object_key, owner FROM files WHERE file_hash = ? LIMIT 1");
    q.addBindValue(QString::fromStdString(file_hash));
    if (!q.exec() || !q.next()) return false;
    object_key = q.value(0).toString().toStdString();
    // owner 列不存在于当前 schema，返回空字符串以保持接口健壮性
    owner.clear();
    return true;
  }

  bool list_files(const std::string& owner, std::vector<FileMeta>& out) override {
    if (!store_) return false;
    QSqlQuery q(QSqlDatabase::database(store_->connection_name_));
    q.prepare("SELECT file_hash, object_key, file_name, file_size, source, upload_ts, status FROM files");
    // 此处因 schema 简化，实际应加 WHERE owner 条件
    if (!q.exec()) return false;
    while (q.next()) {
      FileMeta m;
      m.file_hash = q.value(0).toString().toStdString();
      m.object_key = q.value(1).toString().toStdString();
      m.file_name = q.value(2).toString().toStdString();
      m.file_size = q.value(3).toLongLong();
      m.source = q.value(5).toString().toStdString();
      m.upload_ts = q.value(6).toString().toLongLong();
      m.status = q.value(7).toInt();
      out.push_back(std::move(m));
    }
    return true;
  }

  bool update_quota(const QuotaMeta& quota) override {
    if (!store_) return false;
    // 复用 LocalStore::update_group_quota / update_user_quota
    // 此处根据 id 前缀判断是群还是人
    if (quota.id.startsWith("gid:")) {
      bool ok = store_->update_group_quota(
          QString::fromStdString(quota.id.mid(4)), quota.used_bytes);
      return ok;
    } else {
      bool ok = store_->update_user_quota(
          QString::fromStdString(quota.id), quota.used_bytes);
      return ok;
    }
  }

  bool get_quota(const std::string& id, QuotaMeta& meta) override {
    if (!store_) return false;
    if (id.starts_with("gid:")) {
      qint64 used = 0;
      bool ok = store_->update_group_quota(QString::fromStdString(id.mid(4)), used);
      // 实际上是查询，但 LocalStore 没有专用查询，这里先返回 used=0 占位
      meta.used_bytes = used;
      return ok;
    } else {
      qint64 used = 0;
      bool ok = store_->update_user_quota(QString::fromStdString(id), used);
      meta.used_bytes = used;
      return ok;
    }
  }

  // RustFS compose 相关：本地后端不实现，留空返回 false
  bool compose_blocks(const std::string& object_key,
                      const std::vector<std::string>& blocks) override {
    Q_UNUSED(object_key);
    Q_UNUSED(blocks);
    qWarning() << "[StorageLocal] compose_blocks: 本地后端不支持，请使用 S3/RustFS 后端";
    return false;
  }

  bool decompose_file(const std::string& object_key,
                      std::vector<std::string>& out_blocks) override {
    Q_UNUSED(object_key);
    Q_UNUSED(out_blocks);
    qWarning() << "[StorageLocal] decompose_file: 本地后端不支持，请使用 S3/RustFS 后端";
    return false;
  }

 private:
  LocalStore* store_{nullptr};
};

} // namespace memex::client