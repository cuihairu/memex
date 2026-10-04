// R23-1 真容器集成验证（默认跳过，CI 不跑真容器腿）：
// 设 MEMEX_S3_E2E=1 且宿主有 docker 时全量执行——
// RustFSCompose 生成→compose up→健康检查→S3StorageImpl 全接口往返
//（桶引导/put/get/head/list/分片上传完整+中止/预签名 URL curl 实取/批量删）
// →compose down。两处 compose 契约（RUSTFS_ADDRESS 端口格式、healthcheck
// 命令）即由此用例对真容器验证后反写进实现。
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <fstream>
#include <string>
#include <vector>

#include "storage.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond    \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

// popen 读一遍命令 stdout（预签名 URL curl 实取用）
std::string run_out(const std::string& cmd) {
  std::string out;
  FILE* p = popen(cmd.c_str(), "r");
  if (p == nullptr) return out;
  char buf[512];
  while (std::size_t n = fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
  pclose(p);
  return out;
}

const char* kDir = "/tmp/memex_s3_e2e";
const int kApiPort = 19500;
const int kConsolePort = 19501;

} // namespace

int main() {
  if (std::getenv("MEMEX_S3_E2E") == nullptr) {
    std::cout << "test_s3_e2e: SKIP（未设 MEMEX_S3_E2E；真容器腿需 docker，"
                 "本机手动跑：MEMEX_S3_E2E=1 ctest -R s3_e2e --verbose）\n";
    return 0;
  }

  const std::string dir = kDir;
  const std::string compose_path = dir + "/compose.yml";
  const std::string data_dir = dir + "/data";
  const std::string endpoint =
      "http://127.0.0.1:" + std::to_string(kApiPort);

  // 清残留（上次异常退出的容器/卷），并备好 compose/数据目录
  const int down_rc =
      std::system(("docker compose -f " + compose_path + " down -v >/dev/null 2>&1").c_str());
  // 数据文件属 uid 10001，宿主普通用户删不动：尽力而为＋吞权限噪音（/tmp 易失）
  const int rm_rc = std::system(("rm -rf " + dir + " 2>/dev/null").c_str());
  (void)down_rc; (void)rm_rc; // 清残留尽力而为，成败不影响判定
  std::filesystem::create_directories(data_dir);
  // rustfs 镜像以 uid 10001(rustfs) 运行：bind mount 目须其可写（部署时
  // 对真实数据目录 chown 10001:10001；e2e 在 /tmp 临时目录直接放开权限）
  std::filesystem::permissions(data_dir, std::filesystem::perms::all);

  // —— compose 生命周期：生成→up→健康检查 ——
  CHECK(memex::server::RustFSCompose::write_compose_file(
      compose_path, data_dir, "minioadmin", "minioadmin", kApiPort,
      kConsolePort));
  CHECK(memex::server::RustFSCompose::up(compose_path));
  CHECK(memex::server::RustFSCompose::health_check(endpoint, 60));

  // —— S3StorageImpl 全接口往返（path-style + http 内网口径）——
  memex::server::S3Config cfg;
  cfg.endpoint = endpoint;
  cfg.region = "auto";
  cfg.access_key = "minioadmin";
  cfg.secret_key = "minioadmin";
  cfg.bucket = "memex-e2e";
  cfg.use_ssl = false;
  cfg.path_style = true;
  auto st = memex::server::S3Storage::create(cfg);

  CHECK(st->create_bucket());
  CHECK(st->create_bucket()); // 幂等

  const std::string key = "groups/g1/deadbeef";
  const std::string body = "memex R23-1 object roundtrip\n";
  std::string etag;
  CHECK(st->put_object(key, body, &etag));
  CHECK(!etag.empty());

  std::int64_t size = 0;
  std::string etag2;
  CHECK(st->head_object(key, &size, &etag2));
  CHECK(size == static_cast<std::int64_t>(body.size()));
  CHECK(etag2 == etag);

  std::string got;
  CHECK(st->get_object(key, &got));
  CHECK(got == body);

  // 未命中语义（False 而非异常）
  std::string miss;
  CHECK(!st->get_object("groups/g1/no-such", &miss));
  CHECK(!st->head_object("groups/g1/no-such", nullptr, nullptr));

  auto keys = st->list_objects("groups/g1/", 100);
  CHECK(keys.size() == 1 && keys[0] == key);

  // —— 预签名 URL：匿名 curl 实取（签名 v4 对兼容层真实生效）——
  const std::string url = st->presign_get(key, 300);
  CHECK(url.find("memex-e2e") != std::string::npos &&
        url.find("X-Amz-Signature") != std::string::npos);
  const std::string fetched = run_out("curl -sf '" + url + "'");
  CHECK(fetched == body);

  // 预签名 PUT：匿名 curl 实传 → head 回读大小闭环
  const std::string pkey = "inbox/u9/presigned-up";
  const std::string purl = st->presign_put(pkey, 300);
  CHECK(purl.find("X-Amz-Signature") != std::string::npos);
  const std::string put_body = "uploaded via presigned url";
  const int put_rc = std::system(
      ("curl -sf -X PUT --data-binary '" + put_body + "' '" + purl + "'")
          .c_str());
  CHECK(put_rc == 0);
  std::int64_t psize = 0;
  CHECK(st->head_object(pkey, &psize, nullptr));
  CHECK(psize == static_cast<std::int64_t>(put_body.size()));
  CHECK(st->delete_object(pkey));

  // —— 分片上传：两片完整合流（任一环失败即跳过后续，防 *空 optional）——
  const std::string mkey = "users/u1/multipart.bin";
  auto upload_id = st->create_multipart_upload(mkey);
  CHECK(upload_id.has_value());
  const std::string p1(5 * 1024 * 1024, 'a'); // 非 'a' 补足 5MiB（S3 分片下限）
  const std::string p2 = "tail-part";
  std::optional<std::string> e1, e2;
  if (upload_id.has_value()) {
    e1 = st->upload_part(mkey, *upload_id, 1, p1);
    e2 = st->upload_part(mkey, *upload_id, 2, p2);
  }
  CHECK(e1.has_value() && e2.has_value());
  if (upload_id.has_value() && e1.has_value() && e2.has_value()) {
    auto r = st->complete_multipart_upload(
        mkey, *upload_id, {{1, *e1}, {2, *e2}});
    CHECK(r.has_value());
    CHECK(r->key == mkey && r->bucket == "memex-e2e");
    std::string mgot;
    CHECK(st->get_object(mkey, &mgot));
    CHECK(mgot == p1 + p2);
  }

  // —— 分片中止：已传分片被清理 ——
  auto abort_id = st->create_multipart_upload(mkey + ".abort");
  CHECK(abort_id.has_value());
  if (abort_id.has_value()) {
    CHECK(st->upload_part(mkey + ".abort", *abort_id, 1, "x").has_value());
    CHECK(st->abort_multipart_upload(mkey + ".abort", *abort_id));
    CHECK(!st->head_object(mkey + ".abort", nullptr, nullptr));
  }

  // —— 批量删除（配额/过期清理面）——
  CHECK(st->delete_objects({key, mkey, "groups/g1/ghost"}));
  CHECK(st->list_objects("groups/", 100).empty());

  if (g_failures == 0) {
    std::cout << "test_s3_e2e: all checks passed\n";
  } else {
    std::cout << "test_s3_e2e: " << g_failures << " check(s) FAILED\n";
  }
  // 无论成败都拆容器（验证边界外的宿主不留脏状态）；
  // MEMEX_S3_E2E_KEEP=1 保留现场供排障（docker compose -f <path> down -v 手动清）
  if (std::getenv("MEMEX_S3_E2E_KEEP") == nullptr) {
    const int fin_down_rc = std::system(
        ("docker compose -f " + compose_path + " down -v >/dev/null 2>&1").c_str());
    const int fin_rm_rc = std::system(("rm -rf " + dir + " 2>/dev/null").c_str());
    (void)fin_down_rc; (void)fin_rm_rc;
  } else {
    std::cout << "KEEP 现场保留：" << compose_path << "\n";
  } // 清残留尽力而为，成败不影响判定
  return g_failures == 0 ? 0 : 1;
}
