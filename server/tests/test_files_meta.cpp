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

#include <sqlite3.h>

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

  // —— 秒传（哈希去重）：同属主+同哈希+同归属复用；换目标＝不同文件 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    const auto id1 =
        s.create_file_meta(mk("alice", "H1", "g1", "", "a.bin"));
    CHECK(id1 > 0);
    // 同属主同哈希同目标再建（换文件名）：命中 UNIQUE 约束，必须回查
    // 返回已有行 id——此前实现在冲突时返回 last_insert_rowid（连接上
    // 上一次无关插入的 rowid），本用例锁死该回归。
    CHECK(s.create_file_meta(mk("alice", "H1", "g1", "", "b.bin")) == id1);
    CHECK(s.list_files("", "", 200, 0).size() == 1);

    // 同属主同哈希换目标（第二群/个人空间）：新行（R23-2 语义，归属入键）
    CHECK(s.create_file_meta(mk("alice", "H1", "g2", "", "a.bin")) > 0);
    CHECK(s.create_file_meta(mk("alice", "H1", "", "u1", "a.bin")) > 0);
    CHECK(s.list_files("", "", 200, 0).size() == 3);

    // 异属主同哈希：新行（秒传键含属主，跨属主不去重）
    const auto id4 = s.create_file_meta(mk("bob", "H1", "g1"));
    CHECK(id4 > 0 && id4 != id1);

    // check_second_transfer：命中限定在归属内；回读对象键
    auto hit = s.check_second_transfer("alice", "H1", "g1", "");
    CHECK(hit.has_value() && hit->id == id1 &&
          hit->object_key == "groups/g1/H1");
    CHECK(!s.check_second_transfer("alice", "H1", "g9", "").has_value());
    CHECK(!s.check_second_transfer("alice", "H1", "", "u9").has_value());
    CHECK(!s.check_second_transfer("alice", "H9", "g1", "").has_value());
    CHECK(!s.check_second_transfer("", "H1", "g1", "").has_value());
    CHECK(!s.check_second_transfer("alice", "", "g1", "").has_value());

    // 隔离态不参与秒传（杀毒待审的哈希不得放行复用）
    ServerStore::FileMeta q = mk("carol", "HQ", "g1");
    q.status = ServerStore::FileStatus::Quarantine;
    const auto idq = s.create_file_meta(q);
    CHECK(idq > 0);
    CHECK(!s.check_second_transfer("carol", "HQ", "g1", "").has_value());
  }

  // —— file_by_id / 置顶 / 状态迁移 / 对象引用计数 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    const auto ida = s.create_file_meta(mk("alice", "H1", "g1"));
    CHECK(ida > 0);
    // 内容寻址：同群两属主同哈希共用一个对象键（对象层字节只落一份）
    const auto idb = s.create_file_meta(mk("bob", "H1", "g1", "", "x.bin",
                                          "groups/g1/H1"));
    CHECK(idb > 0 && idb != ida);

    auto byid = s.file_by_id(ida);
    CHECK(byid.has_value() && byid->owner == "alice" &&
          byid->file_hash == "H1" && !byid->pin);
    CHECK(!s.file_by_id(99999).has_value());

    // 置顶：set 后 list 首行是它；取消回原序
    auto rows = s.list_files("g1", "", 200, 0);
    CHECK(rows.size() == 2 && rows[0].id == idb); // id 倒序：b 后建在前
    CHECK(s.set_file_pin(ida, true));
    rows = s.list_files("g1", "", 200, 0);
    CHECK(rows.size() == 2 && rows[0].id == ida && rows[0].pin); // 置顶优先
    CHECK(!s.set_file_pin(99999, true));
    CHECK(s.set_file_pin(ida, false));
    CHECK(s.list_files("g1", "", 200, 0)[0].id == idb);

    // 状态迁移（R23-5 杀毒钩子落点）：normal→quarantine 回读一致
    CHECK(s.set_file_status(ida, ServerStore::FileStatus::Quarantine));
    CHECK(s.file_by_id(ida)->status == ServerStore::FileStatus::Quarantine);
    CHECK(!s.set_file_status(99999, ServerStore::FileStatus::Normal));

    // 引用计数：两行共用对象=2；删一行仍 1；全删 0
    CHECK(s.count_file_refs("groups/g1/H1") == 2);
    CHECK(s.delete_file_meta(idb));
    CHECK(s.count_file_refs("groups/g1/H1") == 1);
    CHECK(s.delete_file_meta(ida));
    CHECK(s.count_file_refs("groups/g1/H1") == 0);
    CHECK(s.count_file_refs("") == 0);
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
    CHECK(!s.check_second_transfer("alice", "H1", "g1", "").has_value());
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

    // 受检扣费（原子，R23-2 上传受理用）：limit 内成功、超限整单拒、
    // 0=不限、负 delta 拒；扣费后 add（退额）不受限
    CHECK(s.charge_group_quota("g1", 400));   // 500+400=900 ≤ 1000
    CHECK(!s.charge_group_quota("g1", 200));  // 900+200 > 1000 → 拒
    CHECK(s.get_group_quota("g1")->used_bytes == 900);
    CHECK(s.charge_group_quota("g3", 10));    // 无行新建（limit=0 不限）
    CHECK(s.get_group_quota("g3")->used_bytes == 10);
    CHECK(!s.charge_group_quota("g3", -1));   // 负 delta 拒
    CHECK(s.charge_user_quota("u9", 5));
    CHECK(!s.charge_user_quota("", 5));
    CHECK(s.add_group_quota_used("g1", 200)); // 退额通道不设限
    CHECK(s.get_group_quota("g1")->used_bytes == 1100);
    // 上限改小后：已超用量拒新增扣费
    CHECK(s.set_group_quota_limit("g1", 500));
    CHECK(!s.charge_group_quota("g1", 1));
    CHECK(s.get_group_quota("g1")->used_bytes == 1100); // 原量不动
  }

  // —— 群内角色（权限模型）：默认 member、owner 行拒改、admin 可任免 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    CHECK(s.create_account("owner1", "pw", "o1"));
    CHECK(s.create_account("admin1", "pw", "a1"));
    CHECK(s.create_account("member1", "pw", "m1"));
    const auto gid = s.create_group(
        "dev", "owner1", {"owner1", "admin1", "member1"});
    CHECK(gid > 0);
    // 缺省角色=member；owner 行查到 member（owner 身份由 groups.owner 判）
    CHECK(s.group_role(gid, "member1") == "member");
    CHECK(s.group_role(gid, "owner1") == "member");
    CHECK(s.group_role(gid, "stranger") == "");  // 非成员
    CHECK(s.group_role(gid, "") == "");
    // 任免 admin；owner 行拒改；非法角色拒
    CHECK(s.group_set_role(gid, "admin1", "admin"));
    CHECK(s.group_role(gid, "admin1") == "admin");
    CHECK(s.group_set_role(gid, "admin1", "member"));
    CHECK(s.group_role(gid, "admin1") == "member");
    CHECK(!s.group_set_role(gid, "owner1", "admin")); // owner 行拒改
    CHECK(!s.group_set_role(gid, "member1", "boss")); // 非法角色
    CHECK(!s.group_set_role(gid, "stranger", "admin")); // 非成员行不动
    CHECK(!s.group_set_role(gid, "", "admin"));
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
      CHECK(s.check_second_transfer("alice", "H1", "g1", "").has_value());
      CHECK(s.get_group_quota("g1")->limit_bytes == 4096);
    }
    std::remove(path.c_str());
  }

  // —— R23-3 kind 空间隔离：收件箱与个人空间互不秒传串用、列表按类目过滤 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    const auto idp =
        s.create_file_meta(mk("alice", "H1", "", "u1", "a.bin", "users/u1/H1"));
    CHECK(idp > 0);
    ServerStore::FileMeta mi =
        mk("alice", "H1", "", "u1", "a.bin", "users/u1/H1");
    mi.kind = ServerStore::FileKind::Inbox;
    const auto idi = s.create_file_meta(mi);
    CHECK(idi > 0 && idi != idp); // 同属主同哈希同归属、类目不同 = 两行

    // 秒传按类目判：各自命中自己的行，跨类目不串用
    auto hit = s.check_second_transfer("alice", "H1", "", "u1");
    CHECK(hit.has_value() && hit->id == idp &&
          hit->kind == ServerStore::FileKind::Personal);
    hit = s.check_second_transfer("alice", "H1", "", "u1",
                                  ServerStore::FileKind::Inbox);
    CHECK(hit.has_value() && hit->id == idi &&
          hit->kind == ServerStore::FileKind::Inbox);

    // 同类目重放建行：冲突回查带 kind，回本类目行 id
    CHECK(s.create_file_meta(mi) == idi);

    // 列表类目过滤：缺省全量、0=个人、1=收件箱
    CHECK(s.list_files("", "u1", 200, 0).size() == 2);
    CHECK(s.list_files("", "u1", 200, 0, 0).size() == 1);
    CHECK(s.list_files("", "u1", 200, 0, 0)[0].id == idp);
    CHECK(s.list_files("", "u1", 200, 0, 1).size() == 1);
    CHECK(s.list_files("", "u1", 200, 0, 1)[0].id == idi);
    // file_by_id 回读 kind
    CHECK(s.file_by_id(idi)->kind == ServerStore::FileKind::Inbox);
    CHECK(s.file_by_id(idp)->kind == ServerStore::FileKind::Personal);
  }

  // —— R23-3 备忘录：建/查/改/删、owner 隔离、空参拒绝、时间倒序 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    // 空参拒绝（留痕原则：无主/空文不落库）
    CHECK(s.create_memo("", "x", 1) == 0);
    CHECK(s.create_memo("alice", "", 1) == 0);
    const auto m1 = s.create_memo("alice", "first", 100);
    const auto m2 = s.create_memo("alice", "second", 200);
    CHECK(m1 > 0 && m2 > 0 && m1 != m2);
    CHECK(s.create_memo("bob", "bob note", 300) > 0);

    auto row = s.memo_by_id(m1);
    CHECK(row.has_value() && row->owner == "alice" &&
          row->content == "first" && row->created_ms == 100 &&
          row->updated_ms == 100);
    CHECK(!s.memo_by_id(99999).has_value());

    // 列表 updated_ms 倒序；owner 过滤即权限（他人查不到）
    auto notes = s.list_memos("alice");
    CHECK(notes.size() == 2 && notes[0].id == m2 && notes[1].id == m1);
    CHECK(s.list_memos("bob").size() == 1);
    CHECK(s.list_memos("carol").empty());
    CHECK(s.list_memos("").empty());
    // 分页
    CHECK(s.list_memos("alice", 1, 0).size() == 1 &&
          s.list_memos("alice", 1, 0)[0].id == m2);
    CHECK(s.list_memos("alice", 200, 1).size() == 1);

    // 更新：内容与 updated_ms 落地、created_ms 不动；异属主改不动
    CHECK(s.update_memo(m1, "alice", "first v2", 300));
    row = s.memo_by_id(m1);
    CHECK(row->content == "first v2" && row->updated_ms == 300 &&
          row->created_ms == 100);
    CHECK(!s.update_memo(m1, "bob", "hijack", 160));
    CHECK(s.memo_by_id(m1)->content == "first v2");
    CHECK(!s.update_memo(99999, "alice", "ghost", 1));
    CHECK(!s.update_memo(m1, "", "x", 1));
    CHECK(!s.update_memo(m1, "alice", "", 1));

    // 更新时间序生效：m1 冲到最前
    notes = s.list_memos("alice");
    CHECK(notes[0].id == m1);

    // 删除：异属主删不动；本人删后查无、再删 false
    CHECK(!s.delete_memo(m2, "bob"));
    CHECK(s.delete_memo(m2, "alice"));
    CHECK(!s.delete_memo(m2, "alice"));
    CHECK(s.list_memos("alice").size() == 1);
    CHECK(!s.delete_memo(99999, "alice"));
  }

  // —— R23-3 备忘录持久化：重开数据仍在 ——
  {
    const std::string path = "/tmp/memex_test_memos.db";
    std::remove(path.c_str());
    {
      ServerStore s;
      CHECK(s.open(path));
      CHECK(s.create_memo("alice", "keep", 111) > 0);
    }
    {
      ServerStore s;
      CHECK(s.open(path));
      auto rows = s.list_memos("alice");
      CHECK(rows.size() == 1 && rows[0].content == "keep" &&
            rows[0].created_ms == 111 && rows[0].updated_ms == 111);
    }
    std::remove(path.c_str());
  }

  // —— R23-3 迁移：R23-2 形态旧库（files 无 kind 列）→ open 迁移保数据 ——
  {
    const std::string path = "/tmp/memex_test_files_migrate_r233.db";
    std::remove(path.c_str());
    {
      sqlite3* db = nullptr;
      CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
      char* err = nullptr;
      // 照抄 R23-2 迁移落地的新形 DDL（探针按前缀认它是 R23-2 已迁移形，
      // R23-3 探针才判 legacy）；插两行：群文件一行、置顶个人文件一行
      const bool ok = sqlite3_exec(
          db,
          "CREATE TABLE files ("
          "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
          "  owner TEXT NOT NULL,"
          "  belong_gid TEXT NOT NULL DEFAULT '',"
          "  belong_uid TEXT NOT NULL DEFAULT '',"
          "  file_name TEXT NOT NULL,"
          "  file_size INTEGER NOT NULL DEFAULT 0,"
          "  file_hash TEXT NOT NULL DEFAULT '',"
          "  object_key TEXT NOT NULL DEFAULT '',"
          "  source INTEGER NOT NULL DEFAULT 0,"
          "  upload_ts INTEGER NOT NULL DEFAULT 0,"
          "  status INTEGER NOT NULL DEFAULT 0,"
          "  pin INTEGER NOT NULL DEFAULT 0,"
          "  UNIQUE(file_hash, owner, belong_gid, belong_uid));"
          "INSERT INTO files(owner, belong_gid, belong_uid, file_name,"
          " file_size, file_hash, object_key, upload_ts)"
          " VALUES('alice', 'g1', '', 'old.bin', 7, 'HOLD',"
          " 'groups/g1/HOLD', 42);"
          "INSERT INTO files(owner, belong_gid, belong_uid, file_name,"
          " file_size, file_hash, object_key, upload_ts, pin)"
          " VALUES('alice', '', 'u1', 'mine.bin', 9, 'HP', 'users/u1/HP',"
          " 43, 1);",
          nullptr, nullptr, &err) == SQLITE_OK;
      if (err) sqlite3_free(err);
      CHECK(ok);
      sqlite3_close(db);
    }
    {
      ServerStore s;
      CHECK(s.open(path)); // open 内跑迁移：加 kind 列、键扩含 kind
      // 存量行保留、kind=0（personal）、pin 保留
      auto rows = s.list_files("g1", "", 200, 0);
      CHECK(rows.size() == 1 && rows[0].file_hash == "HOLD" &&
            rows[0].kind == ServerStore::FileKind::Personal &&
            rows[0].file_size == 7);
      auto mine = s.list_files("", "u1", 200, 0, 0);
      CHECK(mine.size() == 1 && mine[0].pin &&
            mine[0].kind == ServerStore::FileKind::Personal);
      // 秒传语义保持：同归属同哈希 personal 命中旧行
      CHECK(s.check_second_transfer("alice", "HOLD", "g1", "").has_value());
      // 新键含 kind 生效：同哈希进收件箱是空白，不与个人空间串用
      CHECK(!s.check_second_transfer("alice", "HP", "", "u1",
                                     ServerStore::FileKind::Inbox)
                 .has_value());
      CHECK(s.list_files("", "u1", 200, 0, 1).empty());
      // 迁移补建 memos 表可用
      CHECK(s.create_memo("alice", "after migration", 1000) > 0);
    }
    {
      // 重开幂等：二次 open 不再触发迁移，数据照旧
      ServerStore s;
      CHECK(s.open(path));
      CHECK(s.list_files("", "", 200, 0, -1).size() == 2);
      CHECK(s.list_memos("alice").size() == 1);
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
