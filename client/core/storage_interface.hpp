// 存储后端抽象层（S3 兼容）
// 客户端永不直连对象存储，所有元数据与权限均在本地 SQLite（LocalStore）层完成。
// 具体后端（S3、RustFS、本地）由实现类决定，接口统一。
// 纪元 R23-1：T4.7 系统集成与文件传输基础的存储抽象化起点。

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace memex::client {

// --- 文件元数据 ---
struct FileMeta {
  std::string file_hash;    // SHA-256，用于去重（秒传键）
  std::string object_key;   // 对象存储路径/键（S3 key 或 RustFS 路径）
  std::string file_name;
  qint64 file_size{0};
  std::string source;       // "direct" | "collab"
  std::string upload_ts;    // ISO 8601 或 Unix ms 字符串
  int status{0};            // 0=normal, 1=uploading, 2=completed, 3=error
};

// --- 配额元数据 ---
struct QuotaMeta {
  std::string id;           // gid 或 uid
  qint64 used_bytes{0};
  qint64 total_bytes{0};    // 可选配额上限，0=无限
};

// --- 接口 ---
class StorageInterface {
 public:
  virtual ~StorageInterface() = default;

  // 文件增删改查
  virtual bool put_file(const FileMeta& meta) = 0;
  // object_key 对应的文件元数据（若不存在返回 false）
  virtual bool get_file(const std::string& object_key, FileMeta& meta) = 0;
  // 根据哈希查找元数据（用于秒传去重）
  virtual bool get_file_by_hash(const std::string& file_hash,
                                std::string& owner,
                                std::string& object_key) = 0;
  // 列出归属者的文件列表
  virtual bool list_files(const std::string& owner,
                          std::vector<FileMeta>& out) = 0;
  // 更新配额
  virtual bool update_quota(const QuotaMeta& quota) = 0;
  // 查询配额
  virtual bool get_quota(const std::string& id, QuotaMeta& meta) = 0;

  // RustFS compose 相关（若后端为 RustFS 时有效）
  // 提交编码后的文件块到 compose
  virtual bool compose_blocks(const std::string& object_key,
                              const std::vector<std::string>& blocks) = 0;
  // 从 compose 中获取完整文件
  virtual bool decompose_file(const std::string& object_key,
                              std::vector<std::string>& out_blocks) = 0;
};

} // namespace memex::client