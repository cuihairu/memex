// T2.2 客户端半边验收（进程级，真实服务端）：双引擎互发（送达回执＋
// msg_id 去重落库）、接收方离线后上线补投、服务端被杀后的心跳判死与
// 自动重连（退避重试＋自动重登＋离线消息续达）。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <vector>

#include <nlohmann/json.hpp>

#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::LocalStore;
using memex::client::StoredMessage;

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return true;
}

quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

bool port_listening(quint16 port) {
  QTcpServer probe;
  if (probe.listen(QHostAddress::LocalHost, port)) {
    probe.close();
    return false;
  }
  return true;
}

struct Received {
  QString from, text, msg_id;
};

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("collab-chat-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  // 建号（CLI）：alice 与 bob
  for (const auto& row : {std::pair<QString, QString>{QStringLiteral("alice"), QStringLiteral("pass-a")},
                          std::pair<QString, QString>{QStringLiteral("bob"), QStringLiteral("pass-b")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"), QStringLiteral("add"),
                             row.first, row.second, QStringLiteral("--db"), db}) == 0);
  }

  // 平台-12 建群需特权（权限模型「建群需授权」）：alice 授 group_creator
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("alice"),
             QStringLiteral("group_creator"), QStringLiteral("--by"),
             QStringLiteral("alice"), QStringLiteral("--db"),
             db}) == 0);

  const quint16 port = free_port();
  const QStringList server_args = {QStringLiteral("serve"), QStringLiteral("--db"), db,
                                   QStringLiteral("--port"), QString::number(port)};
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin, server_args);
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));

  // 双引擎：各自独立本地库（协作缓存，msg_id 去重）；心跳加密以便断线测试
  LocalStore store_a, store_b;
  CHECK(store_a.open(tmp.filePath(QStringLiteral("a.db"))));
  CHECK(store_b.open(tmp.filePath(QStringLiteral("b.db"))));

  CollabEngine a, b;
  a.attach_store(&store_a);
  b.attach_store(&store_b);
  a.set_heartbeat(300, 2);
  b.set_heartbeat(300, 2);

  std::vector<Received> a_got, b_got;
  QObject::connect(&a, &CollabEngine::message_received, &a,
                   [&](const QString& from, const QString& text, qint64, const QString& msg_id) {
                     a_got.push_back({from, text, msg_id});
                   });
  QObject::connect(&b, &CollabEngine::message_received, &b,
                   [&](const QString& from, const QString& text, qint64, const QString& msg_id) {
                     b_got.push_back({from, text, msg_id});
                   });

  bool a_in = false, b_in = false;
  QObject::connect(&a, &CollabEngine::logged_in, &a,
                   [&](const QString&, const QString&) { a_in = true; });
  QObject::connect(&b, &CollabEngine::logged_in, &b,
                   [&](const QString&, const QString&) { b_in = true; });

  std::vector<std::pair<quint64, bool>> a_receipts, b_receipts;
  QObject::connect(&a, &CollabEngine::text_delivered, &a,
                   [&](quint64 seq, bool ok) { a_receipts.push_back({seq, ok}); });
  QObject::connect(&b, &CollabEngine::text_delivered, &b,
                   [&](quint64 seq, bool ok) { b_receipts.push_back({seq, ok}); });

  // —— R24-1 群公告腿的信号收集：connect 一律顶层（生命周期=测试全程，
  // 防腿块结束局部捕获悬垂后下一条 NOTICE 撞悬垂槽崩溃）——
  quint64 ann_gid = 0;
  QString b_fail_reason;
  std::vector<int> b_urgencies;
  QString b_notice_title;
  quint64 hist_gid = 0;
  QString hist_json;
  QObject::connect(&a, &CollabEngine::group_result, &a,
                   [&](bool ok, const QString&, const QString& op,
                       quint64 id) {
                     if (ok && op == "create") ann_gid = id;
                   });
  QObject::connect(&b, &CollabEngine::group_result, &b,
                   [&](bool ok, const QString& reason, const QString& op,
                       quint64) {
                     if (!ok && op == "announce") b_fail_reason = reason;
                   });
  QObject::connect(&b, &CollabEngine::notice_received, &b,
                   [&](const QString&, const QString& title, const QString&,
                       int urgency, const QString&, qint64, const QString&) {
                     b_urgencies.push_back(urgency);
                     b_notice_title = title;
                   });
  QObject::connect(&a, &CollabEngine::announcement_history_received, &a,
                   [&](quint64 g, const QString& j) {
                     hist_gid = g;
                     hist_json = j;
                   });

  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"), QStringLiteral("pass-a"));
  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"), QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return a_in && b_in; }, 8000));

  // 在线互发：送达回执（受理即 ok=true）＋对端收到（带 msg_id）
  const quint64 s1 = a.send_text(QStringLiteral("bob"), QStringLiteral("你好-bob"));
  CHECK(s1 > 0);
  CHECK(wait_until([&] { return b_got.size() == 1; }, 8000));
  CHECK(b_got[0].from == QStringLiteral("alice"));
  CHECK(b_got[0].text == QStringLiteral("你好-bob"));
  CHECK(!b_got[0].msg_id.isEmpty());
  CHECK(wait_until([&] { return !a_receipts.empty(); }, 8000));
  CHECK(a_receipts[0].first == s1);
  CHECK(a_receipts[0].second);

  const quint64 s2 = b.send_text(QStringLiteral("alice"), QStringLiteral("回你-alice"));
  CHECK(wait_until([&] { return a_got.size() == 1 && b_receipts.size() == 1; }, 8000));
  CHECK(a_got[0].text == QStringLiteral("回你-alice"));
  CHECK(b_receipts[0].first == s2);
  CHECK(b_receipts[0].second);

  // 双方本地库均落协作消息（发送方与接收方各自完整记录，双向同属该对端）
  CHECK(wait_until(
      [&] {
        return store_a.history(QStringLiteral("bob")).size() == 2 &&
               store_b.history(QStringLiteral("alice")).size() == 2;
      },
      5000));
  const auto a_hist = store_a.history(QStringLiteral("bob"));
  bool a_has_sent = false;
  for (const auto& m : a_hist) {
    if (m.source == "collab" && m.text == "你好-bob") a_has_sent = true;
  }
  CHECK(a_has_sent);
  const auto b_hist = store_b.history(QStringLiteral("alice"));
  bool b_match = false;
  for (const auto& m : b_hist) {
    if (m.source == "collab" &&
        m.msg_id == b_got[0].msg_id.toStdString() && m.text == "你好-bob")
      b_match = true;
  }
  CHECK(b_match);

  // 离线补投：bob 登出，alice 发消息入队；bob 重新登录收到，且本地库只此一条
  b.logout();
  CHECK(wait_until([&] { return !b.is_logged_in(); }, 5000));
  const quint64 s3 = a.send_text(QStringLiteral("bob"), QStringLiteral("离线也送达"));
  CHECK(wait_until([&] { return a_receipts.size() == 2 && a_receipts[1].first == s3 && a_receipts[1].second; }, 8000));

  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"), QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return b_got.size() == 2; }, 8000));
  CHECK(b_got[1].text == QStringLiteral("离线也送达"));
  CHECK(wait_until([&] { return store_b.history(QStringLiteral("alice")).size() == 3; }, 5000));
  {
    int copies = 0;
    for (const auto& m : store_b.history(QStringLiteral("alice"))) {
      if (m.text == "离线也送达") ++copies;
      CHECK(!m.msg_id.empty()); // 协作态收发都有 msg_id（发出＝本地派生、收到＝服务端分配）
    }
    CHECK(copies == 1);
  }

  // —— R24-1 群公告：建群→设公告（全员 NOTICE＝重要级强提醒）→普通成员拒→
  // 历史留痕查询回带（倒序，清除也是一笔）——信号收集 connect 已在顶层
  {
    a.create_group(QStringLiteral("公告测试群"), {QStringLiteral("bob")});
    CHECK(wait_until([&] { return ann_gid > 0; }, 8000));
    const quint64 gid = ann_gid;

    // bob 视角的分级推送信号：公告=重要（urgency=2），标题带群名
    a.announce_group(gid, QStringLiteral("每周五例会"));
    CHECK(wait_until([&] { return !b_urgencies.empty(); }, 8000));
    CHECK(b_urgencies[0] == 2); // IMPORTANT＝桌面强提醒级
    CHECK(b_notice_title == QStringLiteral("群公告：公告测试群"));

    // 普通成员设公告被拒（R24-1 权限=群主/管理员）——拒收回执发给 b
    b.announce_group(gid, QStringLiteral("bob 版"));
    CHECK(wait_until([&] { return !b_fail_reason.isEmpty(); }, 8000));
    CHECK(b_fail_reason.contains(QStringLiteral("群主/管理员")));

    // 清除（不推 NOTICE）→重设→历史三笔倒序（新者在前，清除也是一笔）
    a.announce_group(gid, QString());
    a.announce_group(gid, QStringLiteral("新版公告"));
    a.announce_history(gid);
    CHECK(wait_until([&] { return hist_gid == gid && !hist_json.isEmpty(); }, 8000));
    const auto arr = nlohmann::json::parse(hist_json.toStdString(), nullptr, false);
    CHECK(arr.is_array() && arr.size() == 3);
    if (arr.is_array() && arr.size() == 3) {
      CHECK(arr[0]["editor"] == "alice" && arr[0]["content"] == "新版公告");
      CHECK(arr[1]["editor"] == "alice" && arr[1]["content"] == "");
      CHECK(arr[2]["editor"] == "alice" && arr[2]["content"] == "每周五例会");
    }
  }

  // 断线重连：杀服务端 → 双端判死（connection_lost）→ 同库同端口重启 →
  // 退避重试＋自动重登（reconnected）→ 消息续达
  bool a_lost = false, b_lost = false, a_re = false, b_re = false;
  QObject::connect(&a, &CollabEngine::connection_lost, &a, [&] { a_lost = true; });
  QObject::connect(&b, &CollabEngine::connection_lost, &b, [&] { b_lost = true; });
  QObject::connect(&a, &CollabEngine::reconnected, &a, [&] { a_re = true; });
  QObject::connect(&b, &CollabEngine::reconnected, &b, [&] { b_re = true; });

  server.kill();
  CHECK(server.waitForFinished(5000));
  // 判死断言用稳态登出态而非一次性 connection_lost 信号：kill 若落在某端
  // 的重连待命窗（前一串心跳判死后的退避期内，引擎本就不在登录态），
  // disconnected 路径的 was_logged_in 条件不成立、信号不会二次补发——
  // 「服务端死透后客户端不停留在登录态」才是可依赖断言面
  CHECK(wait_until([&] { return !a.is_logged_in() && !b.is_logged_in(); }, 8000));

  QProcess server2;
  server2.setProcessChannelMode(QProcess::ForwardedChannels);
  server2.start(server_bin, server_args);
  CHECK(server2.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));
  CHECK(wait_until([&] { return a_re && b_re; }, 20000));
  // 等双端稳态登录而非读到 reconnected 就断言：双端同刻重连时，服务端
  // 处理后到者登录（PBKDF2 等约 1s）会堵住先到者的心跳回包，测试心跳
  // 300ms×2 会把刚重连的会话判死一次再重试——reconnected 信号可能在
  // 重试窗内到达，只有稳态登录才是可依赖断言面
  CHECK(wait_until([&] { return a.is_logged_in() && b.is_logged_in(); }, 20000));

  // 重连后通道可用（若期间有消息已入离线队，登录补投路径同上）
  const quint64 s4 = a.send_text(QStringLiteral("bob"), QStringLiteral("重启后续达"));
  CHECK(wait_until([&] { return b_got.size() == 3; }, 10000));
  CHECK(b_got[2].text == QStringLiteral("重启后续达"));
  CHECK(wait_until([&] { return !a_receipts.empty() && a_receipts.back().first == s4 && a_receipts.back().second; }, 8000));

  a.logout();
  b.logout();
  server2.terminate();
  server2.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("collab chat tests: all passed");
    return 0;
  }
  qCritical("collab chat tests: %d failure(s)", g_failures);
  return 1;
}
