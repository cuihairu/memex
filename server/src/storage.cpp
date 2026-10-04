// S3 兼容存储抽象层实现（R23-1）
// 依赖：aws-sdk-cpp（vcpkg 安装：aws-sdk-cpp:s3）
// 编译：target_link_libraries(... aws-cpp-sdk-s3 aws-cpp-sdk-core)

#include "storage.hpp"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/Outcome.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/DeleteObjectsRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/UploadPartRequest.h>

#include <fstream>
#include <sstream>
#include <vector>

namespace memex::server {

class S3StorageImpl final : public S3Storage {
 public:
  explicit S3StorageImpl(const S3Config& cfg) : cfg_(cfg) {
    Aws::S3::S3ClientConfiguration client_cfg;
    client_cfg.endpointOverride = cfg.endpoint;
    client_cfg.region = cfg.region;
    client_cfg.scheme = cfg.use_ssl ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
    client_cfg.verifySSL = cfg.use_ssl;
    client_cfg.requestTimeoutMs = 30000;
    client_cfg.connectTimeoutMs = 10000;
    if (cfg.path_style) {
      client_cfg.useVirtualAddressing = false;  // path-style
    }
    client_ = std::make_unique<Aws::S3::S3Client>(
        Aws::Auth::AWSCredentials(cfg.access_key, cfg.secret_key),
        client_cfg,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        false);
  }

  bool put_object(const std::string& key,
                  const std::string& body,
                  std::string* etag_out) override {
    Aws::S3::Model::PutObjectRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);
    req.SetBody(Aws::MakeShared<Aws::StringStream>("PutObject", body));

    auto outcome = client_->PutObject(req);
    if (!outcome.IsSuccess()) {
      return false;
    }
    if (etag_out) {
      *etag_out = outcome.GetResult().GetETag();
    }
    return true;
  }

  bool get_object(const std::string& key, std::string* body_out) override {
    Aws::S3::Model::GetObjectRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);

    auto outcome = client_->GetObject(req);
    if (!outcome.IsSuccess()) {
      return false;
    }
    auto& stream = outcome.GetResult().GetBody();
    std::stringstream ss;
    ss << stream.rdbuf();
    *body_out = ss.str();
    return true;
  }

  bool delete_object(const std::string& key) override {
    Aws::S3::Model::DeleteObjectRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);
    auto outcome = client_->DeleteObject(req);
    return outcome.IsSuccess();
  }

  std::vector<std::string> list_objects(const std::string& prefix,
                                        int max_keys) override {
    std::vector<std::string> keys;
    Aws::S3::Model::ListObjectsV2Request req;
    req.SetBucket(cfg_.bucket);
    req.SetPrefix(prefix);
    req.SetMaxKeys(max_keys);

    auto outcome = client_->ListObjectsV2(req);
    if (!outcome.IsSuccess()) {
      return keys;
    }
    for (const auto& obj : outcome.GetResult().GetContents()) {
      keys.push_back(obj.GetKey());
    }
    return keys;
  }

  std::optional<std::string> create_multipart_upload(
      const std::string& key) override {
    Aws::S3::Model::CreateMultipartUploadRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);
    auto outcome = client_->CreateMultipartUpload(req);
    if (!outcome.IsSuccess()) {
      return std::nullopt;
    }
    return outcome.GetResult().GetUploadId();
  }

  std::optional<std::string> upload_part(const std::string& key,
                                         const std::string& upload_id,
                                         int part_number,
                                         const std::string& body) override {
    Aws::S3::Model::UploadPartRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);
    req.SetUploadId(upload_id);
    req.SetPartNumber(part_number);
    req.SetBody(Aws::MakeShared<Aws::StringStream>("UploadPart", body));
    auto outcome = client_->UploadPart(req);
    if (!outcome.IsSuccess()) {
      return std::nullopt;
    }
    return outcome.GetResult().GetETag();
  }

  std::optional<CompleteUploadResult> complete_multipart_upload(
      const std::string& key, const std::string& upload_id,
      const std::vector<UploadPartResult>& parts) override {
    Aws::S3::Model::CompleteMultipartUploadRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);
    req.SetUploadId(upload_id);

    Aws::Vector<Aws::S3::Model::CompletedPart> completed_parts;
    for (const auto& p : parts) {
      Aws::S3::Model::CompletedPart cp;
      cp.SetPartNumber(p.part_number);
      cp.SetETag(p.etag);
      completed_parts.push_back(std::move(cp));
    }
    Aws::S3::Model::CompletedMultipartUpload cmp;
    cmp.SetParts(std::move(completed_parts));
    req.SetMultipartUpload(std::move(cmp));

    auto outcome = client_->CompleteMultipartUpload(req);
    if (!outcome.IsSuccess()) {
      return std::nullopt;
    }
    CompleteUploadResult res;
    res.location = outcome.GetResult().GetLocation();
    res.bucket = outcome.GetResult().GetBucket();
    res.key = outcome.GetResult().GetKey();
    res.etag = outcome.GetResult().GetETag();
    return res;
  }

  bool abort_multipart_upload(const std::string& key,
                              const std::string& upload_id) override {
    Aws::S3::Model::AbortMultipartUploadRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);
    req.SetUploadId(upload_id);
    auto outcome = client_->AbortMultipartUpload(req);
    return outcome.IsSuccess();
  }

  std::string presign_put(const std::string& key,
                          int expires_seconds) override {
    // SDK 新 API：request 重载已移除，改传 bucket/key（无需构造 request）
    auto url = client_->GeneratePresignedUrl(
        cfg_.bucket, key, Aws::Http::HttpMethod::HTTP_PUT, expires_seconds);
    return url;
  }

  std::string presign_get(const std::string& key,
                          int expires_seconds) override {
    auto url = client_->GeneratePresignedUrl(
        cfg_.bucket, key, Aws::Http::HttpMethod::HTTP_GET, expires_seconds);
    return url;
  }

  bool head_object(const std::string& key,
                   int64_t* size_out,
                   std::string* etag_out) override {
    Aws::S3::Model::HeadObjectRequest req;
    req.SetBucket(cfg_.bucket);
    req.SetKey(key);
    auto outcome = client_->HeadObject(req);
    if (!outcome.IsSuccess()) {
      return false;
    }
    if (size_out) *size_out = outcome.GetResult().GetContentLength();
    if (etag_out) *etag_out = outcome.GetResult().GetETag();
    return true;
  }

  bool delete_objects(const std::vector<std::string>& keys) override {
    if (keys.empty()) return true;
    Aws::S3::Model::DeleteObjectsRequest req;
    req.SetBucket(cfg_.bucket);
    Aws::S3::Model::Delete del;
    for (const auto& k : keys) {
      Aws::S3::Model::ObjectIdentifier oid;
      oid.SetKey(k);
      del.AddObjects(std::move(oid));
    }
    req.SetDelete(std::move(del));
    auto outcome = client_->DeleteObjects(req);
    return outcome.IsSuccess();
  }

 private:
  S3Config cfg_;
  std::unique_ptr<Aws::S3::S3Client> client_;
};

std::unique_ptr<S3Storage> S3Storage::create(const S3Config& cfg) {
  static bool aws_init = []() {
    Aws::InitAPI({});
    return true;
  }();
  (void)aws_init;
  return std::make_unique<S3StorageImpl>(cfg);
}

// RustFS compose 生成
std::string RustFSCompose::generate_compose(const std::string& data_dir,
                                            const std::string& access_key,
                                            const std::string& secret_key,
                                            int api_port,
                                            int console_port) {
  std::ostringstream oss;
  oss << "version: '3.8'\n\n"
      << "services:\n"
      << "  rustfs:\n"
      << "    image: rustfs/rustfs:latest\n"
      << "    container_name: memex-rustfs\n"
      << "    restart: unless-stopped\n"
      << "    environment:\n"
      << "      RUSTFS_ACCESS_KEY: \"" << access_key << "\"\n"
      << "      RUSTFS_SECRET_KEY: \"" << secret_key << "\"\n"
      << "      RUSTFS_ADDRESS: \":\"  # 监听所有接口\n"
      << "    ports:\n"
      << "      - \"" << api_port << ":9000\"   # S3 API\n"
      << "      - \"" << console_port << ":9001\" # 控制台\n"
      << "    volumes:\n"
      << "      - \"" << data_dir << ":/data\"\n"
      << "    command: [\"server\", \"/data\"]\n"
      << "    healthcheck:\n"
      << "      test: [\"CMD\", \"rustfs\", \"admin\", \"info\", \"--json\"]\n"
      << "      interval: 10s\n"
      << "      timeout: 5s\n"
      << "      retries: 5\n"
      << "      start_period: 10s\n";
  return oss.str();
}

bool RustFSCompose::write_compose_file(const std::string& path,
                                       const std::string& data_dir,
                                       const std::string& access_key,
                                       const std::string& secret_key,
                                       int api_port,
                                       int console_port) {
  std::string content = generate_compose(data_dir, access_key, secret_key,
                                         api_port, console_port);
  std::ofstream ofs(path);
  if (!ofs) return false;
  ofs << content;
  return ofs.good();
}

bool RustFSCompose::up(const std::string& compose_path) {
  // 依赖宿主有 docker compose（v2 插件）
  std::string cmd = "docker compose -f " + compose_path + " up -d";
  return std::system(cmd.c_str()) == 0;
}

bool RustFSCompose::down(const std::string& compose_path) {
  std::string cmd = "docker compose -f " + compose_path + " down";
  return std::system(cmd.c_str()) == 0;
}

bool RustFSCompose::health_check(const std::string& endpoint,
                                 int timeout_seconds) {
  // 简单 HTTP GET /minio/health/live（MinIO 兼容端点）或 HEAD bucket
  // 这里用 curl 重试；生产建议用 SDK HeadBucket
  std::string cmd = "curl -sf " + endpoint + "/minio/health/live >/dev/null";
  for (int i = 0; i < timeout_seconds; ++i) {
    if (std::system(cmd.c_str()) == 0) return true;
    sleep(1);
  }
  return false;
}

}  // namespace memex::server