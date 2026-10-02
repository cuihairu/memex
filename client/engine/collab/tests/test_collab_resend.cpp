// T2.5 断线补传归档验收（进程级，真实服务端）：
// 协作态双方在线 → 服务端被杀 → 发送方中断期消息本地暂存（不判失败）→
// 服务端恢复 → 自动重连后补传 → 归档无重复无缺失（CLI messages 检索面核对），
// 接收端按 msg_id 去重只收一次（A16）。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <vector>

#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::LocalStore;

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

QString run_capture(const QString& bin, const QStringList& args) {
  QProcess p;
  p.start(bin, args);
  if (!p.waitForFinished(5000)) return QString();
  return QString::fromUtf8(p.readAllStandardOutput());
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("collab-resend-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-a")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-b")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"), QStringLiteral("add"),
                             row.first, row.second, QStringLiteral("--db"),
                             db}) == 0);
  }

  const quint16 port = free_port();
  const QStringList server_args = {QStringLiteral("serve"),
                                   QStringLiteral("--db"), db,
                                   QStringLiteral("--port"),
                                   QString::number(port)};
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin, server_args);
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));

  LocalStore store_a, store_b;
  CHECK(store_a.open(tmp.filePath(QStringLiteral("a.db"))));
  CHECK(store_b.open(tmp.filePath(QStringLiteral("b.db"))));

  CollabEngine a, b;
  a.attach_store(&store_a);
  b.attach_store(&store_b);
  a.set_heartbeat(300, 2);
  b.set_heartbeat(300, 2);

  std::vector<QString> b_got;
  QObject::connect(&b, &CollabEngine::message_received, &b,
                   [&](const QString&, const QString& text, qint64,
                       const QString&) { b_got.push_back(text); });
  std::vector<std::pair<quint64, bool>> a_receipts;
  QObject::connect(&a, &CollabEngine::text_delivered, &a,
                   [&](quint64 seq, bool ok) { a_receipts.push_back({seq, ok}); });

  bool a_in = false, b_in = false;
  QObject::connect(&a, &CollabEngine::logged_in, &a,
                   [&](const QString&, const QString&) { a_in = true; });
  QObject::connect(&b, &CollabEngine::logged_in, &b,
                   [&](const QString&, const QString&) { b_in = true; });
  bool a_lost = false, b_lost = false, a_re = false, b_re = false;
  QObject::connect(&a, &CollabEngine::connection_lost, &a, [&] { a_lost = true; });
  QObject::connect(&b, &CollabEngine::connection_lost, &b, [&] { b_lost = true; });
  QObject::connect(&a, &CollabEngine::reconnected, &a, [&] { a_re = true; });
  QObject::connect(&b, &CollabEngine::reconnected, &b, [&] { b_re = true; });

  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-a"));
  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
          QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return a_in && b_in; }, 8000));

  // 在线基线消息（归档）
  const quint64 s1 = a.send_text(QStringLiteral("bob"), QStringLiteral("在线基线消息"));
  CHECK(wait_until([&] { return b_got.size() == 1; }, 8000));
  CHECK(wait_until(
      [&] {
        return !a_receipts.empty() && a_receipts[0].first == s1 &&
               a_receipts[0].second;
      },
      8000));

  // —— 服务端中断：中断期消息本地暂存（不判失败）——
  server.kill();
  CHECK(server.waitForFinished(5000));
  CHECK(wait_until([&] { return a_lost && b_lost; }, 8000));

  const quint64 s2 = a.send_text(QStringLiteral("bob"), QStringLiteral("中断期消息甲"));
  const quint64 s3 = a.send_text(QStringLiteral("bob"), QStringLiteral("中断期消息乙"));
  CHECK(s2 != 0); // 暂存受理（待补传），非失败
  CHECK(s3 != 0);

  // —— 服务端恢复：自动重连 → 补传 → 归档无重复无缺失 ——
  QProcess server2;
  server2.setProcessChannelMode(QProcess::ForwardedChannels);
  server2.start(server_bin, server_args);
  CHECK(server2.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));
  CHECK(wait_until([&] { return a_re && b_re; }, 20000));

  // 接收端收到补传消息（重复投递由 msg_id 去重）
  CHECK(wait_until([&] { return b_got.size() == 3; }, 10000));
  CHECK(b_got[1] == QStringLiteral("中断期消息甲"));
  CHECK(b_got[2] == QStringLiteral("中断期消息乙"));

  // 发送方回执：补传消息均被服务端受理
  CHECK(wait_until(
      [&] {
        return a_receipts.size() == 3 && a_receipts[1].first == s2 &&
               a_receipts[1].second && a_receipts[2].first == s3 &&
               a_receipts[2].second;
      },
      10000));

  // 接收端本地库：每条只落一次
  CHECK(wait_until(
      [&] {
        int copies_jia = 0, copies_yi = 0;
        for (const auto& m : store_b.history(QStringLiteral("alice"))) {
          if (m.text == "中断期消息甲") ++copies_jia;
          if (m.text == "中断期消息乙") ++copies_yi;
        }
        return copies_jia == 1 && copies_yi == 1;
      },
      5000));

  // 归档核对（CLI messages 检索面）：三条全在、无重复、无缺失
  {
    const QString out = run_capture(
        server_bin, {QStringLiteral("messages"), QStringLiteral("--db"), db,
                     QStringLiteral("--limit"), QStringLiteral("50")});
    CHECK(out.contains(QStringLiteral("在线基线消息")));
    CHECK(out.count(QStringLiteral("中断期消息甲")) == 1);
    CHECK(out.count(QStringLiteral("中断期消息乙")) == 1);
    CHECK(out.contains(QStringLiteral("共 3 条")));
    CHECK(out.contains(QStringLiteral("alice")) &&
          out.contains(QStringLiteral("bob")));
  }

  // 恢复后正常通道仍可用
  const quint64 s4 = a.send_text(QStringLiteral("bob"), QStringLiteral("恢复后新消息"));
  CHECK(wait_until([&] { return b_got.size() == 4; }, 8000));
  CHECK(wait_until(
      [&] {
        return !a_receipts.empty() && a_receipts.back().first == s4 &&
               a_receipts.back().second;
      },
      8000));
  {
    const QString out = run_capture(
        server_bin, {QStringLiteral("messages"), QStringLiteral("--db"), db,
                     QStringLiteral("--limit"), QStringLiteral("50")});
    CHECK(out.contains(QStringLiteral("共 4 条")));
  }

  a.logout();
  b.logout();
  server2.terminate();
  server2.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("collab resend tests: all passed");
    return 0;
  }
  qCritical("collab resend tests: %d failure(s)", g_failures);
  return 1;
}
