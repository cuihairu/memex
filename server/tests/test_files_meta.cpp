// R23-1 存储抽象层验收（服务端元数据面，纯库级不涉网）：
// 文件元数据表（创建/字段回读/删除）、秒传哈希去重（UNIQUE(file_hash,owner)
// 冲突返回已有行 id、异属主不命中、隔离态不参与秒传）、群/人两级配额
//（无行=0 用量 0 上限、UPSERT 累加、负上限拒绝、两级独立）、外网收件箱
// 留痕（uplink_logs 追加/过滤/倒序）、RustFS compose 生成内容契约。
#include <cstdio>
#include <iostream>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "storage.hpp"
#include "store.hpp"

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

using memex::server::RustFSCompose;
using memex::server::S3Config;
using ServerStore = memex::server::ServerStore;

ServerStore::FileMeta mk(const std::string& owner, const std::string& hash,
                         const std::string& gid = "",
                         const std::string& uid = "",
                         const std::string& name = "a.bin",
                         const std::string& key = "") {
  ServerStore::FileMeta m;
  m.owner = owner;
  m.belong_gid = gid;
  m.belong_uid = uid;
  m.file_name = name;
  m.file_size = 123;
  m.file_hash = hash;
  m.object_key = key.empty() ? "groups/g1/" + hash : key;
  m.upload_ts = 1700000000000;
  return m;
}

} // namespace

int main() {
  // —— 元数据表：创建/字段回读/空参拒绝 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    const auto id = s.create_file_meta(mk("alice", "H1", "g1", "", "report.pdf",
                                          "groups/g1/H1"));
    CHECK(id > 0);
    auto rows = s.list_files("g1", "", 200, 0);
    CHECK(rows.size() == 1);
    CHECK(rows[0].id == id);
    CHECK(rows[0].owner == "alice");
    CHECK(rows[0].belong_gid == "g1");
    CHECK(rows[0].belong_uid == "");
    CHECK(rows[0].file_name == "report.pdf");
    CHECK(rows[0].file_size == 123);
    CHECK(rows[0].file_hash == "H1");
    CHECK(rows[0].object_key == "groups/g1/H1");
    CHECK(rows[0].source == ServerStore::FileSource::Internal);
    CHECK(rows[0].status == ServerStore::FileStatus::Normal);
    CHECK(rows[0].upload_ts == 1700000000000);

    // 空属主/空文件名拒绝（留痕原则：无主字节不收）
    CHECK(s.create_file_meta(mk("", "H2")) == 0);
    CHECK(s.create_file_meta(mk("alice", "H2", "", "", "")) == 0);
    CHECK(s.list_files("", "", 200, 0).size() == 1);
  }

  // —— 秒传（哈希去重）：同属主同哈希复用，不重复落元数据 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    const auto id1 =
        s.create_file_meta(mk("alice", "H1", "g1", "", "a.bin"));
    CHECK(id1 > 0);
    // 同属主同哈希再建（换文件名/换归属）：命中 UNIQUE 约束，必须回查
    // 返回已有行 id——此前实现在冲突时返回 last_insert_rowid（连接上
    // 上一次无关插入的 rowid），本用例锁死该回归。
    CHECK(s.create_file_meta(mk("alice", "H1", "", "u9", "b.bin")) == id1);
    CHECK(s.list_files("", "", 200, 0).size() == 1);

    // 异属主同哈希：新行（秒传键含属主，跨属主不去重）
    const auto id2 = s.create_file_meta(mk("bob", "H1", "g1"));
    CHECK(id2 > 0 && id2 != id1);

    // check_second_transfer：命中回读对象键与来源；异哈希不命中
    auto hit = s.check_second_transfer("alice", "H1");
    CHECK(hit.has_value() && hit->id == id1 &&
          hit->object_key == "groups/g1/H1");
    CHECK(!s.check_second_transfer("alice", "H9").has_value());
    CHECK(!s.check_second_transfer("", "H1").has_value());
    CHECK(!s.check_second_transfer("alice", "").has_value());

    // 隔离态不参与秒传（杀毒待审的哈希不得放行复用）
    ServerStore::FileMeta q = mk("carol", "HQ", "g1");
    q.status = ServerStore::FileStatus::Quarantine;
    const auto idq = s.create_file_meta(q);
    CHECK(idq > 0);
    CHECK(!s.check_second_transfer("carol", "HQ").has_value());
  }

  // —— list_files：归属过滤（群/人/组合）、倒序、分页 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    CHECK(s.create_file_meta(mk("alice", "H1", "g1")) > 0);
    CHECK(s.create_file_meta(mk("alice", "H2", "g1", "", "b.bin")) > 0);
    CHECK(s.create_file_meta(mk("alice", "H3", "g2", "", "c.bin")) > 0);
    CHECK(s.create_file_meta(mk("bob", "H4", "", "u1", "d.bin")) > 0);

    CHECK(s.list_files("", "", 200, 0).size() == 4);          // 全量
    CHECK(s.list_files("g1", "", 200, 0).size() == 2);        // 按群
    CHECK(s.list_files("g2", "", 200, 0).size() == 1);
    CHECK(s.list_files("", "u1", 200, 0).size() == 1);        // 按人
    CHECK(s.list_files("g1", "u1", 200, 0).empty());          // 组合（交集空）
    CHECK(s.list_files("g9", "", 200, 0).empty());            // 无关群

    auto all = s.list_files("", "", 200, 0);
    CHECK(all[0].file_hash == "H4" && all[3].file_hash == "H1"); // id 倒序
    auto page = s.list_files("", "", 2, 1);                      // 分页
    CHECK(page.size() == 2 && page[0].file_hash == "H3" &&
          page[1].file_hash == "H2");
  }

  // —— 删除：物理删；删后秒传不再命中 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    const auto id = s.create_file_meta(mk("alice", "H1", "g1"));
    CHECK(s.delete_file_meta(id));
    CHECK(!s.delete_file_meta(id)); // 已删再删 = false
    CHECK(s.list_files("g1", "", 200, 0).empty());
    CHECK(!s.check_second_transfer("alice", "H1").has_value());
  }

  // —— 配额两级（群/人）：无行默认、UPSERT 累加、上限设置、负数拒绝、独立 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    // 无行 = 0 用量、0 上限（0 上限语义=不限，由调用方判）
    auto gq = s.get_group_quota("g1");
    CHECK(gq.has_value() && gq->used_bytes == 0 && gq->limit_bytes == 0);
    auto uq = s.get_user_quota("u1");
    CHECK(uq.has_value() && uq->used_bytes == 0 && uq->limit_bytes == 0);
    CHECK(!s.get_group_quota("").has_value());
    CHECK(!s.get_user_quota("").has_value());
    CHECK(!s.add_group_quota_used("", 1));
    CHECK(!s.add_user_quota_used("", 1));
    CHECK(!s.set_group_quota_limit("", 100));
    CHECK(!s.set_user_quota_limit("", 100));

    // 上限设置（UPSERT 建行）与负数拒绝
    CHECK(s.set_group_quota_limit("g1", 1000));
    CHECK(!s.set_group_quota_limit("g1", -1));
    CHECK(s.get_group_quota("g1")->limit_bytes == 1000);
    CHECK(s.set_user_quota_limit("u1", 500));
    CHECK(!s.set_user_quota_limit("u1", -5));
    CHECK(s.get_user_quota("u1")->limit_bytes == 500);

    // 用量累加：多次 add 求和；不覆盖已设上限
    CHECK(s.add_group_quota_used("g1", 300));
    CHECK(s.add_group_quota_used("g1", 200));
    auto g1 = s.get_group_quota("g1");
    CHECK(g1->used_bytes == 500 && g1->limit_bytes == 1000);
    CHECK(s.add_user_quota_used("u1", 123));
    CHECK(s.get_user_quota("u1")->used_bytes == 123);

    // 两级独立：群 g2 与人 u2 各自建行互不影响；key 隔离
    CHECK(s.add_group_quota_used("g2", 7));
    CHECK(s.get_group_quota("g2")->used_bytes == 7);
    CHECK(s.get_group_quota("g1")->used_bytes == 500);
    CHECK(s.add_user_quota_used("u2", 9));
    CHECK(s.get_user_quota("u2")->used_bytes == 9);
    CHECK(s.get_user_quota("u1")->used_bytes == 123);
  }

  // —— 外网收件箱留痕（uplink_logs）：追加/按人过滤/倒序/limit/空参拒绝 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    ServerStore::UplinkLog a;
    a.uploader = "alice";
    a.file_name = "from-outside.zip";
    a.file_size = 2048;
    a.file_hash = "HU1";
    a.object_key = "inbox/alice/HU1";
    a.upload_ts = 111;
    CHECK(s.add_uplink_log(a));
    ServerStore::UplinkLog b = a;
    b.uploader = "bob";
    b.file_hash = "HU2";
    b.upload_ts = 222;
    CHECK(s.add_uplink_log(b));
    // 空参拒绝（留痕原则：无主留痕不收）
    ServerStore::UplinkLog bad;
    bad.uploader = "";
    CHECK(!s.add_uplink_log(bad));
    bad.uploader = "alice";
    bad.file_name = "";
    CHECK(!s.add_uplink_log(bad));

    auto all = s.list_uplink_logs("", 200);
    CHECK(all.size() == 2);
    CHECK(all[0].uploader == "bob" && all[0].file_hash == "HU2" &&
          all[0].upload_ts == 222); // id 倒序
    CHECK(all[1].uploader == "alice");
    auto mine = s.list_uplink_logs("alice", 200);
    CHECK(mine.size() == 1 && mine[0].object_key == "inbox/alice/HU1");
    CHECK(s.list_uplink_logs("carol", 200).empty());
    CHECK(s.list_uplink_logs("", 1).size() == 1); // limit 生效
  }

  // —— 持久化：文件库重开 schema 幂等、数据仍在 ——
  {
    const std::string path = "/tmp/memex_test_files_meta.db";
    std::remove(path.c_str());
    {
      ServerStore s;
      CHECK(s.open(path));
      CHECK(s.create_file_meta(mk("alice", "H1", "g1")) > 0);
      CHECK(s.set_group_quota_limit("g1", 4096));
    }
    {
      ServerStore s;
      CHECK(s.open(path)); // 二次 open 走既有表（schema 幂等）
      auto rows = s.list_files("g1", "", 200, 0);
      CHECK(rows.size() == 1 && rows[0].file_hash == "H1");
      CHECK(s.check_second_transfer("alice", "H1").has_value());
      CHECK(s.get_group_quota("g1")->limit_bytes == 4096);
    }
    std::remove(path.c_str());
  }

  // —— RustFS compose 生成契约（镜像/凭据/端口/卷/健康检查）——
  {
    const std::string yaml =
        RustFSCompose::generate_compose("/var/lib/memex/rustfs", "AK", "SK",
                                        9000, 9001);
    CHECK(yaml.find("image: rustfs/rustfs:latest") != std::string::npos);
    CHECK(yaml.find("RUSTFS_ACCESS_KEY: \"AK\"") != std::string::npos);
    CHECK(yaml.find("RUSTFS_SECRET_KEY: \"SK\"") != std::string::npos);
    CHECK(yaml.find("\"9000:9000\"") != std::string::npos); // S3 API
    CHECK(yaml.find("\"9001:9001\"") != std::string::npos); // 控制台
    CHECK(yaml.find("/var/lib/memex/rustfs:/data") != std::string::npos);
    CHECK(yaml.find("healthcheck") != std::string::npos);
    // 真容器验证过的两处契约（rustfs 1.0.1）：裸 ":" 地址会 FATAL 退出、
    // admin 子命令不存在导致 healthcheck 永败——锁死回归。
    CHECK(yaml.find("RUSTFS_ADDRESS: \":9000\"") != std::string::npos);
    CHECK(yaml.find("\"rustfs\", \"info\"") != std::string::npos);
    CHECK(yaml.find("admin") == std::string::npos);

    // 落盘回读一致（自定义端口也应生效）
    const std::string p = "/tmp/memex_test_rustfs_compose.yml";
    CHECK(RustFSCompose::write_compose_file(p, "/data/rustfs", "ak2", "sk2",
                                            19000, 19001));
    std::ifstream ifs(p);
    std::stringstream ss;
    ss << ifs.rdbuf();
    CHECK(ss.str() == RustFSCompose::generate_compose("/data/rustfs", "ak2",
                                                      "sk2", 19000, 19001));
    std::remove(p.c_str());
  }

  // —— S3Config 默认值：内网 http + path-style（S3 兼容层语义）——
  {
    S3Config cfg;
    CHECK(!cfg.use_ssl);
    CHECK(cfg.path_style);
  }

  if (g_failures == 0) {
    std::cout << "test_files_meta: all checks passed\n";
    return 0;
  }
  std::cout << "test_files_meta: " << g_failures << " check(s) FAILED\n";
  return 1;
}
