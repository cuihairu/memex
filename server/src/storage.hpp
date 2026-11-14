#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>

namespace memex::server {

// S3 兼容存储抽象层（R23-1）：
// - 统一签名/代理读写，权限在元数据层判（ServerStore）
// - 对象前缀：groups/{gid}/、users/{uid}/、inbox/{uid}/（外网上传）
// - 默认后端 RustFS（compose 一键），兼容 MinIO/云 S3（换 endpoint 即可）

struct S3Config {
  std::string endpoint;       // 如 http://127.0.0.1:9000
  std::string region;         // 如 auto
  std::string access_key;     // RustFS 默认 minioadmin
  std::string secret_key;     // RustFS 默认 minioadmin
  std::string bucket;         // 如 memex-files
  bool use_ssl{false};        // 内网 http
  bool path_style{true};      // S3 兼容层常需 path-style
};

struct UploadPartResult {
  int part_number{0};
  std::string etag;
};

struct CompleteUploadResult {
  std::string location;
  std::string bucket;
  std::string key;
  std::string etag;
};

class S3Storage {
 public:
  // 工厂：从配置文件/环境变量加载（部署口径：server 启动时读一次）
  static std::unique_ptr<S3Storage> create(const S3Config& cfg);

  virtual ~S3Storage() = default;

  // 桶引导（部署时一次；已存在时幂等成功）
  virtual bool create_bucket() = 0;

  // 简单上传（小文件 ≤5MB 建议直传；大文件走分片）
  virtual bool put_object(const std::string& key,
                          const std::string& body,
                          std::string* etag_out = nullptr) = 0;

  // 简单下载（返回字节体；大文件建议签名 URL 直下）
  virtual bool get_object(const std::string& key,
                          std::string* body_out) = 0;

  // 删除对象
  virtual bool delete_object(const std::string& key) = 0;

  // 列出前缀（用于群文件/个人文件/收件箱列表）
  virtual std::vector<std::string> list_objects(const std::string& prefix,
                                                int max_keys = 1000) = 0;

  // 全量列举带成败通道（files verify 专用）：false＝中途失败（out 只含
  // 已取得部分，调用方不得据此下「核对通过」结论）。缺省实现复用
  // list_objects 恒回 true（内存桩/兼容实现沿用旧口径，无需感知成败）。
  virtual bool list_objects_checked(const std::string& prefix, int max_keys,
                                    std::vector<std::string>& out) {
    out = list_objects(prefix, max_keys);
    return true;
  }

  // —— 分片上传（大文件/断点续传/秒传预检） ——
  // 1. 发起分片上传，返回 upload_id
  virtual std::optional<std::string> create_multipart_upload(const std::string& key) = 0;
  // 2. 上传单分片（part_number 1-based），返回 etag
  virtual std::optional<std::string> upload_part(const std::string& key,
                                                 const std::string& upload_id,
                                                 int part_number,
                                                 const std::string& body) = 0;
  // 3. 完成分片上传（需按 part_number 递增传入 etag 列表）
  virtual std::optional<CompleteUploadResult> complete_multipart_upload(
      const std::string& key, const std::string& upload_id,
      const std::vector<UploadPartResult>& parts) = 0;
  // 4. 放弃分片上传（清理已上传分片）
  virtual bool abort_multipart_upload(const std::string& key,
                                      const std::string& upload_id) = 0;

  // 预签名 URL。铁律张力警示：设计铁律=客户端永不直连对象存储（所有读写
  // 过 memex server），内网文件面（R23-2/3）不得用本组方法做直传直下；
  // 当前无消费者。若未来外网 uplink（R23-4）确需直传，须先过设计评审
  // 明示暴露范围，否则应删除（见 docs/design/文件存储与外网单向传输.md）
  virtual std::string presign_put(const std::string& key,
                                  int expires_seconds = 3600) = 0;
  virtual std::string presign_get(const std::string& key,
                                  int expires_seconds = 3600) = 0;

  // HEAD 对象（秒传判存在、取大小/etag）
  virtual bool head_object(const std::string& key,
                           int64_t* size_out = nullptr,
                           std::string* etag_out = nullptr) = 0;

  // 批量删除（配额清理/过期清理）
  virtual bool delete_objects(const std::vector<std::string>& keys) = 0;
};

// 出厂默认凭据判定（CR-4）：RustFS 默认 minioadmin/minioadmin 是公开已知
// 值，任一腿仍是它即视为默认（半个凭据泄露同样不可接受）。命中时 compose
// 生成与 CLI 须大声警告——不静默放行。
inline bool is_default_credential(const std::string& access_key,
                                  const std::string& secret_key) {
  return access_key == "minioadmin" || secret_key == "minioadmin";
}

// shell 单引号转义（CR-3）：compose 生成器把 operator 提供的 compose_path /
// endpoint 拼接进 std::system 命令串——含空格或单引号会破句/注入。单引号内
// 一切字符按字面，仅需把 ' 换成 '\''（POSIX sh 惯用）。
inline std::string shell_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '\'';
  for (const char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += '\'';
  return out;
}

// YAML 双引号标量转义（compose 生成用，评审 codex-758）：access_key/
// secret_key/data_dir 来自 CLI/env/部署输入，直接拼进 compose 文本＝恶意
// 值可注入任意 YAML 键（如凭据带换行即插入新顶级键 privileged: true）。
// 返回值自带两端双引号；`\`/`"`/换行/控制字符全部按 YAML 双引号转义集
// 转义（\\ \" \n \r \t，其余控制字符 \xNN）——单行引号标量，结构不变。
inline std::string yaml_quote(const std::string& s) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (const unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          out += "\\x";
          out += hex[c >> 4];
          out += hex[c & 0x0f];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return out;
}

// RustFS compose 编排（R23-1）：
// - 单节点 RustFS 即可（自托管、零依赖、Rust 单二进制）
// - 端口 9000（S3 API）、9001（控制台）
// - 数据卷挂载到宿主 /var/lib/rustfs
// - 默认凭据 minioadmin/minioadmin（部署时须改，CLI 提供修改口令）
struct RustFSCompose {
  static std::string generate_compose(const std::string& data_dir,
                                      const std::string& access_key = "minioadmin",
                                      const std::string& secret_key = "minioadmin",
                                      int api_port = 9000,
                                      int console_port = 9001);
  static bool write_compose_file(const std::string& path,
                                 const std::string& data_dir,
                                 const std::string& access_key,
                                 const std::string& secret_key,
                                 int api_port,
                                 int console_port);
  // 启动/停止/健康检查（供 CLI/安装脚本调用）
  static bool up(const std::string& compose_path);
  static bool down(const std::string& compose_path);
  static bool health_check(const std::string& endpoint, int timeout_seconds = 30);
};

}  // namespace memex::server