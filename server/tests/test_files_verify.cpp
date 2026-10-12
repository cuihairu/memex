// files verify 一致性核对验收（评审 P0-3，todo 452）：R23 文件元数据 ↔
// 对象字节双面核对的口径腿（内存假存储，不起容器/不涉网）：
// ① 全一致（元数据行 ↔ 对象一一对上）→ 三清单全空；
// ② 缺对象（元数据在、对象被删）→ missing；
// ③ 大小不符（对象字节被换）→ mismatches（期望/实际大小齐带）；
// ④ 孤儿对象（桶里有、无元数据引用）→ orphans 只告警不判死；
// ⑤ 隔离/过期行不算孤儿（引用面含全部状态）也不受检（只核 normal）；
// ⑥ 空 object_key 行（无字节面）跳过不核。
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "files_server.hpp"
#include "store.hpp"

using memex::server::FilesVerifyMismatch;
using memex::server::FilesVerifyReport;
using memex::server::S3Storage;
using memex::server::ServerStore;
using memex::server::verify_file_objects;

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

// 内存假存储：只实现核对路径（put/head/list）；其余纯虚一律桩
class FakeS3 : public S3Storage {
public:
  std::map<std::string, std::string> objects_;

  bool create_bucket() override { return true; }
  bool put_object(const std::string& key, const std::string& body,
                  std::string* etag_out = nullptr) override {
    objects_[key] = body;
    if (etag_out != nullptr) *etag_out = "etag-" + key;
    return true;
  }
  bool get_object(const std::string& key, std::string* body_out) override {
    const auto it = objects_.find(key);
    if (it == objects_.end()) return false;
    if (body_out != nullptr) *body_out = it->second;
    return true;
  }
  bool delete_object(const std::string& key) override {
    return objects_.erase(key) > 0;
  }
  std::vector<std::string> list_objects(const std::string& prefix,
                                        int max_keys = 1000) override {
    std::vector<std::string> out;
    for (const auto& [k, v] : objects_) {
      (void)v;
      if (out.size() >= static_cast<std::size_t>(max_keys)) break;
      if (k.rfind(prefix, 0) == 0) out.push_back(k);
    }
    return out;
  }
  std::optional<std::string> create_multipart_upload(
      const std::string&) override {
    return std::nullopt;
  }
  std::optional<std::string> upload_part(const std::string&,
                                         const std::string&, int,
                                         const std::string&) override {
    return std::nullopt;
  }
  std::optional<memex::server::CompleteUploadResult>
  complete_multipart_upload(const std::string&, const std::string&,
                            const std::vector<memex::server::UploadPartResult>&)
      override {
    return std::nullopt;
  }
  bool abort_multipart_upload(const std::string&, const std::string&)
      override {
    return false;
  }
  std::string presign_put(const std::string&, int = 3600) override {
    return "";
  }
  std::string presign_get(const std::string&, int = 3600) override {
    return "";
  }
  bool head_object(const std::string& key, std::int64_t* size_out = nullptr,
                   std::string* = nullptr) override {
    const auto it = objects_.find(key);
    if (it == objects_.end()) return false;
    if (size_out != nullptr) {
      *size_out = static_cast<std::int64_t>(it->second.size());
    }
    return true;
  }
  bool delete_objects(const std::vector<std::string>& keys) override {
    for (const auto& k : keys) objects_.erase(k);
    return true;
  }
};

ServerStore::FileMeta make_meta(const std::string& owner,
                                const std::string& key,
                                std::int64_t size, int status = 0) {
  ServerStore::FileMeta m;
  m.owner = owner;
  m.belong_uid = owner;
  m.file_name = key + ".bin";
  m.file_size = size;
  m.file_hash = "hash-" + key;
  m.object_key = key;
  m.status = static_cast<ServerStore::FileStatus>(status);
  return m;
}

} // namespace

int main() {
  ServerStore store;
  CHECK(store.open(":memory:"));
  CHECK(store.create_account("alice", "pa-1", "Alice"));
  CHECK(store.create_account("bob", "pb-1", "Bob"));
  FakeS3 s3;

  // 造底数：alice 两个 normal 对象＋bob 一个 normal 对象＋
  // alice 隔离行（status=1）＋bob 过期行（status=2）＋alice 空 key 行
  const auto m1 = make_meta("alice", "users/alice/h1", 3);
  const auto m2 = make_meta("alice", "users/alice/h2", 5);
  const auto m3 = make_meta("bob", "users/bob/h3", 7);
  const auto mq = make_meta("alice", "users/alice/hq", 9, 1); // 隔离
  const auto me = make_meta("bob", "users/bob/he", 11, 2);    // 过期
  const auto mn = make_meta("alice", "", 13);                 // 无字节面
  CHECK(store.create_file_meta(m1) > 0);
  CHECK(store.create_file_meta(m2) > 0);
  CHECK(store.create_file_meta(m3) > 0);
  CHECK(store.create_file_meta(mq) > 0);
  CHECK(store.create_file_meta(me) > 0);
  CHECK(store.create_file_meta(mn) > 0);
  CHECK(s3.put_object("users/alice/h1", std::string(3, 'a')));
  CHECK(s3.put_object("users/alice/h2", std::string(5, 'b')));
  CHECK(s3.put_object("users/bob/h3", std::string(7, 'c')));
  CHECK(s3.put_object("users/alice/hq", std::string(9, 'q')));
  CHECK(s3.put_object("users/bob/he", std::string(11, 'e')));

  { // ① 全一致：三清单全空；受检行=3（normal），对象=5（隔离/过期算引用面）
    const auto rep = verify_file_objects(store, s3);
    CHECK(rep.checked == 3);
    CHECK(rep.objects == 5);
    CHECK(rep.missing.empty());
    CHECK(rep.mismatches.empty());
    CHECK(rep.orphans.empty());
  }
  { // ② 缺对象：删 h3 的字节 → missing 且退出口径为不一致
    CHECK(s3.delete_object("users/bob/h3"));
    const auto rep = verify_file_objects(store, s3);
    CHECK(rep.missing.size() == 1);
    if (!rep.missing.empty()) CHECK(rep.missing[0] == "users/bob/h3");
    CHECK(!rep.missing.empty() || !rep.mismatches.empty()); // 判死口径
    CHECK(s3.put_object("users/bob/h3", std::string(7, 'c'))); // 还原
  }
  { // ③ 大小不符：h2 字节被换（同键异长）→ mismatches 带期望/实际
    CHECK(s3.put_object("users/alice/h2", std::string(4, 'x')));
    const auto rep = verify_file_objects(store, s3);
    CHECK(rep.mismatches.size() == 1);
    if (!rep.mismatches.empty()) {
      const auto& m = rep.mismatches[0];
      CHECK(m.key == "users/alice/h2");
      CHECK(m.expected == 5);
      CHECK(m.actual == 4);
    }
    CHECK(s3.put_object("users/alice/h2", std::string(5, 'b'))); // 还原
  }
  { // ④ 孤儿对象：桶里多一个无引用键 → orphans；不影响一致判定
    CHECK(s3.put_object("users/alice/orphan-crash-window", "z"));
    const auto rep = verify_file_objects(store, s3);
    CHECK(rep.orphans.size() == 1);
    if (!rep.orphans.empty()) {
      CHECK(rep.orphans[0] == "users/alice/orphan-crash-window");
    }
    CHECK(rep.missing.empty() && rep.mismatches.empty()); // 孤儿不判死
    CHECK(s3.delete_object("users/alice/orphan-crash-window"));
  }
  { // ⑤ 隔离/过期行：引用面（删其字节→不算孤儿也不算 missing）＋不受检
    CHECK(s3.delete_object("users/alice/hq"));
    CHECK(s3.delete_object("users/bob/he"));
    const auto rep = verify_file_objects(store, s3);
    CHECK(rep.checked == 3); // 只核 normal
    CHECK(rep.missing.empty());
    CHECK(rep.orphans.empty());
    // 全删字节后恢复，保持底数（后续腿/重跑）
    CHECK(s3.put_object("users/alice/hq", std::string(9, 'q')));
    CHECK(s3.put_object("users/bob/he", std::string(11, 'e')));
  }

  if (g_failures == 0) {
    std::cout << "files_verify tests: all passed\n";
    return 0;
  }
  std::cerr << "files_verify tests: " << g_failures << " failure(s)\n";
  return 1;
}
