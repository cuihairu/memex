// T4.3 客户端半边验收（进程级，真实服务端）：已读上报→发送方收
// READ_NOTICE（message_read 信号）、在线表推送（presence_changed 信号，
// 含自己，他人上线/下线即刷新）、同账号第二台桌面登录触发互踢（kicked）。
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

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("read-presence-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  for (const auto& row : {std::pair<QString, QString>{QStringLiteral("alice"), QStringLiteral("pass-a")},
                          std::pair<QString, QString>{QStringLiteral("bob"), QStringLiteral("pass-b")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"), QStringLiteral("add"),
                             row.first, row.second, QStringLiteral("--db"), db}) == 0);
  }

  const quint16 port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin, {QStringLiteral("serve"), QStringLiteral("--db"),
                            db, QStringLiteral("--port"),
                            QString::number(port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));

  LocalStore store_a, store_b;
  CHECK(store_a.open(tmp.filePath(QStringLiteral("a.db"))));
  CHECK(store_b.open(tmp.filePath(QStringLiteral("b.db"))));

  CollabEngine a, b;
  a.attach_store(&store_a);
  b.attach_store(&store_b);

  // 在线表推送（登录/他人上线即刷新，含自己）
  QStringList a_presence, b_presence;
  QObject::connect(&a, &CollabEngine::presence_changed, &a,
                   [&](const QStringList& accounts) { a_presence = accounts; });
  QObject::connect(&b, &CollabEngine::presence_changed, &b,
                   [&](const QStringList& accounts) { b_presence = accounts; });

  // 已读回执（发送方视角）
  struct ReadNotice {
    QString msg_id, reader;
  };
  std::vector<ReadNotice> a_reads;
  QObject::connect(&a, &CollabEngine::message_read, &a,
                   [&](const QString& msg_id, const QString& reader, qint64) {
                     a_reads.push_back({msg_id, reader});
                   });

  QString b_last_msg;
  QObject::connect(
      &b, &CollabEngine::message_received, &b,
      [&](const QString&, const QString&, qint64, const QString& msg_id) {
        b_last_msg = msg_id;
      });

  bool a_in = false, b_in = false;
  QObject::connect(&a, &CollabEngine::logged_in, &a,
                   [&](const QString&, const QString&) { a_in = true; });
  QObject::connect(&b, &CollabEngine::logged_in, &b,
                   [&](const QString&, const QString&) { b_in = true; });

  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return a_in; }, 8000));
  // 登录推送含自己（PRESENCE_DATA 先于 LOGIN_RESULT 到达，引擎照收）
  CHECK(wait_until([&] { return a_presence.contains(QStringLiteral("alice")); },
                   5000));

  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
          QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return b_in; }, 8000));
  // bob 上线 → 双方推送都含对方；bob 的推送含自己
  CHECK(wait_until([&] { return b_presence.contains(QStringLiteral("alice")) &&
                                  b_presence.contains(QStringLiteral("bob")); },
                   5000));
  CHECK(wait_until([&] { return a_presence.contains(QStringLiteral("bob")); },
                   5000));

  // 主动拉取（query_presence 回执同结构）
  a.query_presence();
  CHECK(wait_until([&] { return a_presence.size() == 2; }, 5000));

  // a→b 发消息，b 上报已读 → a 收 READ_NOTICE（msg_id/已读方正确）
  CHECK(a.send_text(QStringLiteral("bob"), QStringLiteral("已读验收")) != 0);
  CHECK(wait_until([&] { return !b_last_msg.isEmpty(); }, 8000));
  b.mark_read(b_last_msg);
  CHECK(wait_until([&] { return a_reads.size() == 1; }, 8000));
  CHECK(a_reads[0].msg_id == b_last_msg);
  CHECK(a_reads[0].reader == QStringLiteral("bob"));

  // bob 登出 → alice 收到变更推送（bob 消失，alice 仍在）
  b.logout();
  CHECK(wait_until([&] { return !b.is_logged_in(); }, 5000));
  CHECK(wait_until([&] { return a_presence.size() == 1 &&
                                  a_presence.contains(QStringLiteral("alice")); },
                   8000));

  // 同账号第二台桌面登录 → 先登录端被踢（桌面单点在线，T2.1 合并验收）
  CollabEngine a2;
  bool a_kicked = false;
  QString kick_reason, kick_by;
  QObject::connect(&a, &CollabEngine::kicked, &a,
                   [&](const QString& reason, const QString& by) {
                     a_kicked = true;
                     kick_reason = reason;
                     kick_by = by;
                   });
  bool a2_in = false;
  QObject::connect(&a2, &CollabEngine::logged_in, &a2,
                   [&](const QString&, const QString&) { a2_in = true; });
  a2.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
           QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return a2_in; }, 8000));
  CHECK(wait_until([&] { return a_kicked; }, 8000));
  CHECK(kick_reason.contains(QStringLiteral("单点在线")));
  CHECK(!kick_by.isEmpty());
  CHECK(!a.is_logged_in());

  server.kill();
  CHECK(server.waitForFinished(5000));

  if (g_failures == 0) {
    qInfo("read_presence client tests: all passed");
    return 0;
  }
  qCritical("read_presence client tests: %d failure(s)", g_failures);
  return 1;
}
