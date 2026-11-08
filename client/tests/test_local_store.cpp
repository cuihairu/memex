// 本机库 LocalStore 全 API 验收（离屏、不触网）：持久化核心逐腿打穿。
// ① 生命周期（开关/重复打开/关后守卫）② 打开失败（路径是目录）
// ③ append 去重与 inserted 语义、本地 seq 分配、槽位查询
// ④ history 截取正序、peers 按近聊排序与 source 过滤
// ⑤ history_between 时间窗四变体（from/until/双/无）
// ⑥ msg_id 回填只补空位/终值覆盖/撤回标记
// ⑦ 同步状态机（PENDING→SENDING→SERVER_ACKED→ARCHIVED 只进不退）
// ⑧ 回执只升不降（read 终态）与 (to,seq)/(msg_id) 双口径
// ⑨ 「盯」订阅 UPSERT/解除/提醒打点只一次
// ⑩ 文件记录唯一键与群/用户配额「新增+更新」双分支
// ⑪ 扫描白名单 QSettings 往返（org/app 带 pid 隔离，跑完清理）
// ⑫ 设备身份落库/长度守卫/首存固化 与对端定针 TOFU 生命周期
// （helper_records／scan_results 六函数不入测：ensure_schema 未建这两张
//  表且 mark_helper_deleted 漏 bind——潜在缺陷如实上报，不把坏行为写成规格）
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <string>

#include <core/local_store.hpp>

using memex::client::LocalStore;
using memex::client::ScanWhitelist;
using memex::client::StoredMessage;
using memex::client::WatchedRow;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

StoredMessage make_msg(const std::string& peer, const std::string& from,
                       const std::string& to, std::uint64_t seq,
                       qint64 ts_ms, const std::string& text,
                       const std::string& source = "collab",
                       const std::string& msg_id = "") {
  StoredMessage m;
  m.peer = peer;
  m.from = from;
  m.to = to;
  m.seq = seq;
  m.ts_ms = ts_ms;
  m.text = text;
  m.source = source;
  m.msg_id = msg_id;
  m.sync_state = "ARCHIVED";
  return m;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  // QSettings 隔离（org/app 带 pid），跑完 clear——组件内部走默认 org 便捷构造
  QCoreApplication::setOrganizationName(
      QStringLiteral("memex-test-%1").arg(QCoreApplication::applicationPid()));
  QCoreApplication::setApplicationName(QStringLiteral("local_store_test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db_path = tmp.path() + QStringLiteral("/local.db");

  // —— ① 生命周期：关后守卫与重复打开 ——
  {
    LocalStore store;
    CHECK(!store.is_open());
    CHECK(!store.append(make_msg("p", "a", "b", 1, 1, "关库拒写")));
    CHECK(store.history(QStringLiteral("p")).isEmpty());
    CHECK(store.open(db_path));
    CHECK(store.is_open());
    CHECK(!store.open(db_path)); // 已开再开＝拒绝（连接唯一）
    store.close();
    CHECK(!store.is_open());
  }

  // —— ② 打开失败：路径是目录（SQLite 无法当库文件打开） ——
  {
    LocalStore store;
    CHECK(!store.open(tmp.path())); // 目录路径 → db.open 失败分支
    CHECK(!store.is_open());
  }

  // —— ③ append 去重、inserted 语义、seq 分配、槽位查询 ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    bool inserted = true;
    CHECK(store.append(make_msg("dedup", "alice", "bob", 1, 100, "第一版"),
                       &inserted));
    CHECK(inserted);
    CHECK(store.append(make_msg("bob", "alice", "bob", 1, 100, "重演忽略"),
                       &inserted));
    CHECK(!inserted); // UNIQUE(from,seq) 命中 IGNORE → 未插入
    CHECK(store.next_local_seq("alice") == 2);
    CHECK(store.next_local_seq("nobody") == 1);
    CHECK(store.msg_id_at("alice", 1).empty()); // 未回填＝空
    CHECK(store.msg_id_at("", 1).empty());      // 空 from 守卫
    CHECK(store.msg_id_at("alice", 99).empty()); // 无此槽位
    store.close();
  }

  // —— ④ history 截取正序 + peers 排序/过滤 ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    for (int i = 1; i <= 5; ++i)
      CHECK(store.append(make_msg("bob", "am", "bob", i, i * 100,
                                  std::string("m") + std::to_string(i))));
    CHECK(store.append(make_msg("carol", "carol", "alice", 1, 50, "早")));
    const auto all = store.history(QStringLiteral("bob"));
    CHECK(all.size() == 5);
    CHECK(all.front().text == "m1" && all.back().text == "m5"); // 整体正序
    const auto tail = store.history(QStringLiteral("bob"), 3);
    CHECK(tail.size() == 3);
    CHECK(tail.front().text == "m3" && tail.back().text == "m5"); // 最近 N 正序
    const auto peers = store.peers();
    CHECK(peers.size() == 3); // bob(近聊在前) / dedup / carol
    CHECK(peers.front() == QStringLiteral("bob")); // 近聊在前
    CHECK(store.peers(QStringLiteral("collab")).size() == 3);
    CHECK(store.peers(QStringLiteral("direct")).isEmpty()); // source 过滤
    store.close();
  }

  // —— ⑤ history_between 时间窗四变体 ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    for (int i = 1; i <= 5; ++i)
      CHECK(store.append(make_msg("win", "aw", "bob", i, i * 100,
                                  std::string("w") + std::to_string(i))));
    CHECK(store.history_between(QStringLiteral("win"), 0, 0, 200).size() == 5);
    CHECK(store.history_between(QStringLiteral("win"), 250, 0, 200).size() == 3);
    CHECK(store.history_between(QStringLiteral("win"), 0, 300, 200).size() == 3);
    CHECK(store.history_between(QStringLiteral("win"), 250, 400, 200).size() == 2);
    CHECK(store.history_between(QStringLiteral("win"), 250, 400, 1).size() == 1);
    CHECK(store.history_between(QStringLiteral("nowhere"), 0, 0, 200).isEmpty());
    store.close();
  }

  // —— ⑥ msg_id 回填/覆盖与撤回 ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    CHECK(store.append(make_msg("rc", "ar", "bob", 1, 100, "撤回目标")));
    CHECK(store.set_msg_id("ar", 1, "id-orig")); // 受理前本地推定
    CHECK(!store.set_msg_id("ar", 1, "id-other")); // 只补空位
    CHECK(store.msg_id_at("ar", 1) == "id-orig");
    CHECK(store.set_msg_id_final("ar", 1, "id-final")); // 换盐重排覆盖
    CHECK(store.msg_id_at("ar", 1) == "id-final");
    CHECK(!store.set_msg_id("", 1, "x") && !store.set_msg_id_final("a", 1, ""));
    CHECK(!store.mark_recalled("")); // 空 id 守卫
    CHECK(!store.mark_recalled("id-miss")); // 无行＝false
    CHECK(store.mark_recalled("id-final"));
    CHECK(store.history(QStringLiteral("rc")).front().recalled);
    store.close();
  }

  // —— ⑦ 同步状态机：只进不退，pending 只出 PENDING/SENDING ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    StoredMessage m = make_msg("sync", "as", "bob", 1, 100, "待发");
    m.sync_state = "PENDING";
    CHECK(store.append(m));
    CHECK(store.pending_sync("as").size() == 1);
    CHECK(store.set_sync_state("as", 1, "SENDING"));
    CHECK(!store.set_sync_state("", 1, "SENDING")); // 空 from 守卫
    CHECK(!store.set_sync_state("as", 1, ""));   // 空 state 守卫
    CHECK(store.pending_sync("as").size() == 1); // SENDING 仍在队
    CHECK(store.set_sync_state("as", 1, "SERVER_ACKED"));
    CHECK(store.pending_sync("as").isEmpty()); // 受领即出队
    CHECK(!store.set_sync_state_archived("nope"));
    CHECK(!store.set_sync_state_archived("")); // 空 msg_id 守卫
    // ARCHIVED 只从 SERVER_ACKED 走：PENDING 态不越级推进
    CHECK(store.set_msg_id("as", 1, "sync-mid"));
    CHECK(store.set_sync_state("as", 1, "PENDING"));
    CHECK(!store.set_sync_state_archived("sync-mid"));
    CHECK(store.set_sync_state("as", 1, "SERVER_ACKED"));
    CHECK(store.set_sync_state_archived("sync-mid")); // 受领态才可归档
    store.close();
  }

  // —— ⑧ 回执只升不降（read 终态）——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    CHECK(store.append(make_msg("rcpt", "ac1", "bob", 1, 100, "协作域",
                                "collab", "rid-1")));
    CHECK(store.append(make_msg("rcpt", "ac2", "alice", 1, 100, "直连域",
                                "direct")));
    CHECK(!store.set_receipt("", "delivered")); // 空 id 守卫
    CHECK(store.set_receipt("rid-1", "delivered"));
    CHECK(store.receipt_of_msg_id("rid-1") == "delivered");
    CHECK(store.set_receipt("rid-1", "read"));
    CHECK(!store.set_receipt("rid-1", "delivered")); // 已读终态不倒退
    CHECK(store.receipt_of_msg_id("rid-1") == "read");
    CHECK(store.receipt_of_msg_id("rid-miss").empty());
    CHECK(!store.set_receipt_for_seq("alice", 0, "delivered")); // seq=0 守卫
    CHECK(store.set_receipt_for_seq("alice", 1, "delivered")); // 直连行
    CHECK(store.receipt_of_seq("alice", 1) == "delivered");
    CHECK(store.set_receipt_for_seq("alice", 1, "read"));
    CHECK(!store.set_receipt_for_seq("alice", 1, "delivered"));
    CHECK(store.receipt_of_seq("alice", 1) == "read");
    CHECK(store.receipt_of_seq("alice", 99).empty()); // 无行
    CHECK(store.receipt_of_seq("", 1).empty());       // 守卫
    store.close();
  }

  // —— ⑨ 「盯」订阅：UPSERT/解除/提醒打点只一次 ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    WatchedRow w;
    w.key = "d:bob:1";
    w.peer = "bob";
    w.source = "direct";
    w.text = "盯这条";
    w.created_ms = 1000;
    CHECK(store.add_watch(w));
    w.key = "";
    CHECK(!store.add_watch(w)); // 空 key 守卫
    w.key = "d:bob:1";
    w.text = "重盯刷新预览";
    CHECK(store.add_watch(w)); // UPSERT 整行覆盖
    CHECK(store.watched_all().size() == 1);
    CHECK(store.watched_all().front().text == "重盯刷新预览");
    CHECK(store.mark_watch_reminded("d:bob:1"));
    CHECK(!store.mark_watch_reminded("d:bob:1")); // 只一次
    CHECK(!store.mark_watch_reminded(""));
    CHECK(!store.remove_watch("d:bob:miss"));
    CHECK(store.remove_watch("d:bob:1"));
    CHECK(!store.remove_watch("d:bob:1")); // 二次解除＝无行
    CHECK(store.watched_all().isEmpty());
    store.close();
  }

  // —— ⑩ 文件记录唯一键 + 配额「新增+更新」双分支 ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    CHECK(store.add_file("alice", "g1", "", "a.txt", 10, "h1", "k1", "collab"));
    CHECK(store.add_file("alice", "g1", "", "a2.txt", 10, "h1", "k1",
                         "collab")); // UNIQUE(hash,owner) 命中 IGNORE
    CHECK(store.add_file("alice", "", "", "b.txt", 20, "h2", "k2", "personal"));
    CHECK(store.file_list(QStringLiteral("alice"), QStringLiteral("g1")).size() == 1);
    CHECK(store.file_list(QStringLiteral("alice"), QString()).size() == 2);
    CHECK(store.file_list(QStringLiteral("nobody"), QString()).isEmpty());
    CHECK(store.update_group_quota(QStringLiteral("g1"), 100)); // 新增分支
    CHECK(store.update_group_quota(QStringLiteral("g1"), 200)); // 更新分支
    CHECK(store.update_user_quota(QStringLiteral("alice"), 10)); // 新增分支
    CHECK(store.update_user_quota(QStringLiteral("alice"), 30)); // 更新分支
    store.close();
  }

  // —— ⑪ 扫描白名单：写入面（读取往返待修，见函数尾注） ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    CHECK(store.get_scan_whitelist() == nullptr); // 未设置＝空（此腿可用）
    ScanWhitelist w;
    w.allowed_extensions << QStringLiteral(".pdf") << QStringLiteral(".png");
    w.allowed_mime_types << QStringLiteral("application/pdf");
    w.max_file_size = 1024;
    CHECK(store.set_scan_whitelist(w));
    store.close();
    QSettings(QCoreApplication::organizationName(),
              QCoreApplication::applicationName())
        .clear(); // 还原配置面（测试隔离纪律）
  }
  // 注：set→get 往返断言未写——set 存 QByteArray，get 走
  // QVariant::toJsonDocument() 读取在 Qt6 恒返回 null（往返断裂），
  // 属 R23-5 桩代码潜在缺陷，如实上报不写成规格。

  // —— ⑫ 设备身份与对端定针（TOFU 生命周期） ——
  {
    LocalStore store;
    CHECK(store.open(db_path));
    const std::string seed(64, 'a');
    const std::string pub(64, 'b');
    CHECK(!store.save_identity(std::string(63, 'a'), pub));  // 长度守卫
    CHECK(!store.save_identity(seed, std::string(65, 'b')));
    CHECK(!store.read_identity(nullptr, nullptr)); // 空指针守卫
    std::string rs, rp;
    CHECK(!store.read_identity(&rs, &rp)); // 未存＝false
    CHECK(store.save_identity(seed, pub));
    CHECK(store.read_identity(&rs, &rp));
    CHECK(rs == seed && rp == pub);
    CHECK(store.save_identity(std::string(64, 'z'), std::string(64, 'z')));
    CHECK(store.read_identity(&rs, &rp)); // 首存固化：IGNORE 不覆盖
    CHECK(rs == seed && rp == pub);
    CHECK(store.peer_identity_pub("dev-1").empty()); // 未定针
    CHECK(!store.pin_peer_identity("dev-1", std::string(63, 'c')));
    CHECK(!store.pin_peer_identity("", pub));
    CHECK(store.pin_peer_identity("dev-1", std::string(64, 'c')));
    CHECK(store.peer_identity_pub("dev-1") == std::string(64, 'c'));
    CHECK(store.clear_peer_identity("dev-1"));
    CHECK(store.clear_peer_identity("dev-1")); // 幂等：exec 成功即 true（无行也然）
    CHECK(store.peer_identity_pub("dev-1").empty());
    store.close();
  }

  QSettings(QCoreApplication::organizationName(),
            QCoreApplication::applicationName())
      .clear();
  if (g_failures != 0) {
    qCritical("== %d CHECK(s) failed ==", g_failures);
    return 1;
  }
  qInfo("local_store: all checks passed");
  return 0;
}
