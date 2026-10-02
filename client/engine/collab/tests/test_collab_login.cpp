// T2.1 客户端半边验收：CollabEngine 登录真实服务端（进程级），
// 同账号第二台桌面登录后第一台收到 kicked 信号（客户端提示载体），
// 口令错误收到 login_failed。设备指纹为本机稳定 SHA-256。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;

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

// 找一个空闲 TCP 端口（先绑后放，服务端随即占用；本机回环竞争窗口极小）
quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("collab-login-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  // 建号（CLI）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("account"), QStringLiteral("add"),
             QStringLiteral("alice"), QStringLiteral("pass-1"),
             QStringLiteral("--db"), db}) == 0);

  // 起服务端进程
  const quint16 port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, port) ? (probe.close(), false)
                                                       : true; // 端口被占即已监听
  }, 8000));

  // 引擎 A 登录成功
  CollabEngine a;
  QString a_display;
  bool a_in = false;
  QObject::connect(&a, &CollabEngine::logged_in, &a,
                   [&](const QString&, const QString& display) {
                     a_in = true;
                     a_display = display;
                   });
  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return a_in; }, 8000));
  CHECK(a.is_logged_in());
  CHECK(a_display == QStringLiteral("alice")); // 未设显示名则回落账号

  // 引擎 B 同账号登录：A 收到互踢提示并被下线
  CollabEngine b;
  bool b_in = false;
  QObject::connect(&b, &CollabEngine::logged_in, &b,
                   [&](const QString&, const QString&) { b_in = true; });
  bool a_kicked = false;
  QString kick_reason, kick_by;
  QObject::connect(&a, &CollabEngine::kicked, &a,
                   [&](const QString& reason, const QString& by) {
                     a_kicked = true;
                     kick_reason = reason;
                     kick_by = by;
                   });
  bool a_lost = false;
  QObject::connect(&a, &CollabEngine::connection_lost, &a, [&] { a_lost = true; });

  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return b_in && a_kicked; }, 8000));
  CHECK(b.is_logged_in());
  CHECK(!a.is_logged_in());
  CHECK(kick_reason.contains(QStringLiteral("单点在线")));
  CHECK(!kick_by.isEmpty());
  CHECK(!a_lost); // 互踢属预期断开，不报连接丢失

  // 口令错误：login_failed 带原因
  CollabEngine c;
  QString fail_reason;
  bool c_failed = false;
  QObject::connect(&c, &CollabEngine::login_failed, &c,
                   [&](const QString& reason) {
                     c_failed = true;
                     fail_reason = reason;
                   });
  c.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("wrong"));
  CHECK(wait_until([&] { return c_failed; }, 8000));
  CHECK(fail_reason == QStringLiteral("口令不符"));
  CHECK(!c.is_logged_in());

  // 主动登出：连接干净断开且不触发 connection_lost
  bool b_lost = false;
  QObject::connect(&b, &CollabEngine::connection_lost, &b, [&] { b_lost = true; });
  b.logout();
  CHECK(wait_until([&] { return !b.is_logged_in(); }, 5000));
  CHECK(!b_lost);

  // 登录记录全量可查（CLI 查询面）
  QProcess logins;
  logins.start(server_bin,
               {QStringLiteral("logins"), QStringLiteral("alice"),
                QStringLiteral("--db"), db});
  CHECK(logins.waitForFinished(5000));
  const QString out = QString::fromUtf8(logins.readAllStandardOutput());
  CHECK(out.contains(QStringLiteral("alice")));
  CHECK(out.contains(QStringLiteral("ok")));
  CHECK(out.contains(QStringLiteral("bad_password")));
  CHECK(out.count(QStringLiteral("ok")) >= 2);

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("collab login tests: all passed");
    return 0;
  }
  qCritical("collab login tests: %d failure(s)", g_failures);
  return 1;
}
