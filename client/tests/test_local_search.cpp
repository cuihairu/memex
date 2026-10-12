// 聊天记录模糊检索验收（客户端本机库）：核心口径腿与窗口冒烟同库同口径。
// 核心腿（LocalStore::search_messages，与服务端 search_messages 同口径）：
// ① 多关键词 AND（全部命中才出，不做 OR 联想）；② 英文大小写不敏感；
// ③ 中文按字符直接匹配；④ 只记得一半（半词命中）；⑤ 词在长句中间；
// ⑥ LIKE 元字符按字面；⑦ 分词口径（ASCII 空白／半角逗号／全角逗号）；
// ⑧ 相关度排序（命中次数×1000 ＋ 首命中位置提前量封顶 999）。
// 窗口腿（MessageSearchDialog 离屏）：绑定本机库→检索→命中列表→
// 选中回传对端键（打开会话信号）。
#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>

#include <functional>

#include <app/message_search_dialog.hpp>
#include <core/local_store.hpp>

using memex::client::LocalStore;
using memex::client::MessageSearchDialog;
using memex::client::StoredMessage;

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
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());

  LocalStore store;
  CHECK(store.open(":memory:"));
  const qint64 t0 = 1760000000000;
  CHECK(store.append(make_msg("bob", "alice", "bob", 1, t0 + 1000,
                              "合同 合同 合同", "collab", "m1")));
  CHECK(store.append(make_msg("bob", "alice", "bob", 2, t0 + 2000,
                              "合同已签，请归档", "collab", "m2")));
  CHECK(store.append(make_msg("bob", "alice", "bob", 3, t0 + 3000,
                              "合同扫描件已发", "collab", "m3")));
  CHECK(store.append(make_msg("alice", "bob", "alice", 4, t0 + 4000,
                              "请财务本周三前把发票已开那条对账一下", "collab",
                              "m4")));
  CHECK(store.append(make_msg("alice", "bob", "alice", 5, t0 + 5000,
                              "发票已开", "collab", "m5")));
  CHECK(store.append(make_msg("alice", "bob", "alice", 6, t0 + 6000,
                              "please invite me to the review", "collab",
                              "m6")));
  CHECK(store.append(make_msg("alice", "bob", "alice", 7, t0 + 7000,
                              "Invite only for internal folks", "collab",
                              "m7")));
  CHECK(store.append(make_msg("bob", "carol", "bob", 8, t0 + 8000,
                              "周末团建报名", "direct", "")));

  const auto search = [&store](const QString& kw) {
    return store.search_messages(kw, 1000);
  };
  { // ⑤ 词在长句中间
    const auto h = search(QStringLiteral("对账一下"));
    CHECK(h.size() == 1);
    if (!h.empty()) CHECK(h[0].msg_id == "m4");
  }
  { // ④ 只记得一半（中文半词）
    const auto h = search(QStringLiteral("合同扫"));
    CHECK(h.size() == 1);
    if (!h.empty()) CHECK(h[0].msg_id == "m3");
  }
  { // ④ 只记得一半（英文只记得开头几个字母）
    const auto h = search(QStringLiteral("PLEA"));
    CHECK(h.size() == 1);
    if (!h.empty()) CHECK(h[0].msg_id == "m6");
  }
  { // ① 多关键词 AND：全部命中才出
    const auto h = search(QStringLiteral("发票 对账"));
    CHECK(h.size() == 1);
    if (!h.empty()) CHECK(h[0].msg_id == "m4");
  }
  { // ① AND 而非 OR：无行同时含两词 → 0 命中
    CHECK(search(QStringLiteral("合同 发票")).isEmpty());
  }
  { // ② 英文大小写不敏感（invite/Invite 都命中）
    const auto h = search(QStringLiteral("INVITE"));
    CHECK(h.size() == 2);
    if (h.size() == 2) {
      CHECK(h[0].msg_id == "m7"); // Invite 在句首，位置提前量高
      CHECK(h[1].msg_id == "m6");
    }
  }
  { // ③ 中文按字符直接匹配（直连行 msg_id 为空，按正文断言）
    const auto h = search(QStringLiteral("团建"));
    CHECK(h.size() == 1);
    if (!h.empty()) CHECK(h[0].text == "周末团建报名");
  }
  { // ⑥ LIKE 元字符按字面：下划线不当通配符
    CHECK(search(QStringLiteral("合同_")).isEmpty());
  }
  { // ⑦ 分词口径：全角逗号「，」同样分词
    const auto h = search(QStringLiteral("发票，已"));
    CHECK(h.size() == 2);
  }
  { // ⑦ 半角逗号同样分词
    const auto h = search(QStringLiteral("发票,已"));
    CHECK(h.size() == 2);
  }
  { // ⑧ 相关度排序：命中次数多者居前，同分按时间倒序
    const auto h = search(QStringLiteral("合同"));
    CHECK(h.size() == 3);
    if (h.size() == 3) {
      CHECK(h[0].msg_id == "m1"); // 「合同 合同 合同」3 次命中
      CHECK(h[1].msg_id == "m3");
      CHECK(h[2].msg_id == "m2");
      CHECK(h[0].score > h[1].score);
      CHECK(h[1].score == h[2].score);
    }
  }
  { // 空关键词＝不过滤（返回空表，窗口态不列全量）
    CHECK(store.search_messages(QStringLiteral("")).isEmpty());
  }
  qInfo("[local-search] 核心口径腿 %s", g_failures == 0 ? "PASS" : "FAIL");

  // —— 窗口冒烟：绑定本机库 → 检索 → 命中列表 → 选中回传 ——
  {
    MessageSearchDialog dlg;
    dlg.bind_store(&store);
    dlg.search_box()->setText(QStringLiteral("合同扫"));
    dlg.run_search();
    CHECK(dlg.result_count() == 1);
    CHECK(dlg.status_text().contains(QStringLiteral("1 条")));
    dlg.results()->setCurrentRow(0);
    QString got_peer, got_msg_id;
    QObject::connect(&dlg, &MessageSearchDialog::open_message,
                     [&](const QString& peer, const QString& msg_id) {
                       got_peer = peer;
                       got_msg_id = msg_id;
                     });
    dlg.open_selected();
    CHECK(got_peer == QStringLiteral("bob"));
    CHECK(got_msg_id == QStringLiteral("m3"));

    // 多词 AND 在窗口面同样成立
    dlg.search_box()->setText(QStringLiteral("发票 对账"));
    dlg.run_search();
    CHECK(dlg.result_count() == 1);
    // 0 命中可表达（不残留上一轮行）
    dlg.search_box()->setText(QStringLiteral("合同 发票"));
    dlg.run_search();
    CHECK(dlg.result_count() == 0);
    // 空关键词清空列表
    dlg.search_box()->setText(QStringLiteral(" "));
    dlg.run_search();
    CHECK(dlg.result_count() == 0);
    qInfo("[local-search] 窗口冒烟腿 %s", g_failures == 0 ? "PASS" : "FAIL");
  }

  qInfo("[local-search] %s", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
