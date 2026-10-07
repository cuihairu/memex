// 平台-10 验收：直连文件旁路授权（蓝图§十九四问——可发/可收/跨部门/再
// 转发）。文件字节不经服务器，判权必须经服务器：统一 AuthorizationService
// 规则裁决，未登录＝fail-closed 本地拒（权限不能绕开服务器）。
// 腿：①同部门放行 ②跨部门默认禁 ③策略按部门放行跨部门 ④再转发策略
// 禁（forward=1 即拒、forwardable 随单回落） ⑤收方未知拒 ⑥未登录本地拒。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <map>
#include <tuple>

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
  QCoreApplication::setApplicationName(QStringLiteral("file-authz-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  // 账号与部门：alice/bob 同属研发部，carol 在市场部（跨部门腿）
  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-a")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-b")},
        std::pair<QString, QString>{QStringLiteral("carol"),
                                    QStringLiteral("pass-c")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"), QStringLiteral("add"),
                             row.first, row.second, QStringLiteral("--db"),
                             db}) == 0);
  }
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("org"), QStringLiteral("dept"),
                           QStringLiteral("add"), QStringLiteral("公司/研发部"),
                           QStringLiteral("--db"), db}) == 0);
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("org"), QStringLiteral("dept"),
                           QStringLiteral("add"), QStringLiteral("公司/市场部"),
                           QStringLiteral("--db"), db}) == 0);
  for (const auto& [account, dept] :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("公司/研发部")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("公司/研发部")},
        std::pair<QString, QString>{QStringLiteral("carol"),
                                    QStringLiteral("公司/市场部")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("org"), QStringLiteral("set"),
                             account, QStringLiteral("--dept"), dept,
                             QStringLiteral("--db"), db}) == 0);
  }

  const quint16 port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin, {QStringLiteral("serve"), QStringLiteral("--db"),
                            db, QStringLiteral("--port"),
                            QString::number(port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));

  CollabEngine a;
  a.set_heartbeat(300, 2);
  bool a_in = false;
  QObject::connect(&a, &CollabEngine::logged_in, &a,
                   [&](const QString&, const QString&) { a_in = true; });
  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return a_in; }, 8000));

  // 裁决回收：req → (allowed, reason, forwardable)
  std::map<quint64, std::tuple<bool, QString, bool>> verdicts;
  quint64 next_req = 0;
  QObject::connect(&a, &CollabEngine::file_authz_result, &a,
                   [&](quint64 req, bool allowed, const QString& reason,
                       bool forwardable) {
                     verdicts[req] = {allowed, reason, forwardable};
                   });
  const auto ask = [&](const QString& to, bool forward) {
    const quint64 req = ++next_req;
    verdicts.erase(req);
    a.file_authz(req, to, 1024, QStringLiteral("case.bin"), QString(),
                 forward);
    return req;
  };

  // ① 同部门：允许（第四问答复 forwardable 默认允）
  {
    const quint64 req = ask(QStringLiteral("bob"), false);
    CHECK(wait_until([&] { return verdicts.count(req); }, 5000));
    CHECK(std::get<0>(verdicts[req]));
    CHECK(std::get<1>(verdicts[req]) == QStringLiteral("allow:org-transfer"));
    CHECK(std::get<2>(verdicts[req]));
  }

  // ② 跨部门：默认禁（一级部门不同）
  {
    const quint64 req = ask(QStringLiteral("carol"), false);
    CHECK(wait_until([&] { return verdicts.count(req); }, 5000));
    CHECK(!std::get<0>(verdicts[req]));
    CHECK(std::get<1>(verdicts[req]) == QStringLiteral("deny:cross-department"));
  }

  // ③ 策略放行：研发部行开跨部门 → carol 可达（CLI 运行中改库，裁决面
  // 每问现查现裁，无需重启服务端）
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("policy"), QStringLiteral("set"),
                           QStringLiteral("--dept"),
                           QStringLiteral("公司/研发部"),
                           QStringLiteral("--allow-cross-dept-file"),
                           QStringLiteral("on"), QStringLiteral("--db"),
                           db}) == 0);
  {
    const quint64 req = ask(QStringLiteral("carol"), false);
    CHECK(wait_until([&] { return verdicts.count(req); }, 5000));
    CHECK(std::get<0>(verdicts[req]));
    CHECK(std::get<1>(verdicts[req]) == QStringLiteral("allow:org-transfer"));
  }

  // ④ 再转发禁：研发部行关转发 → forward=1 即拒，且 forwardable=false
  //（③ 开的跨部门行保留现值——CLI 基线保留未提开关）
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("policy"), QStringLiteral("set"),
                           QStringLiteral("--dept"),
                           QStringLiteral("公司/研发部"),
                           QStringLiteral("--allow-forward-file"),
                           QStringLiteral("off"), QStringLiteral("--db"),
                           db}) == 0);
  {
    const quint64 req = ask(QStringLiteral("bob"), true);
    CHECK(wait_until([&] { return verdicts.count(req); }, 5000));
    CHECK(!std::get<0>(verdicts[req]));
    CHECK(std::get<1>(verdicts[req]) ==
          QStringLiteral("deny:forward-forbidden"));
    CHECK(!std::get<2>(verdicts[req]));
    // 非 forward 的正常发送不受再转发开关影响
    const quint64 req2 = ask(QStringLiteral("bob"), false);
    CHECK(wait_until([&] { return verdicts.count(req2); }, 5000));
    CHECK(std::get<0>(verdicts[req2]));
  }

  // ⑤ 收方未知：账号不存在即拒
  {
    const quint64 req = ask(QStringLiteral("nobody"), false);
    CHECK(wait_until([&] { return verdicts.count(req); }, 5000));
    CHECK(!std::get<0>(verdicts[req]));
    CHECK(std::get<1>(verdicts[req]) ==
          QStringLiteral("deny:recipient-unknown"));
  }

  // ⑥ 未登录 fail-closed：本地即拒（deny:server-unreachable），不出网络
  a.logout();
  {
    const quint64 req = ask(QStringLiteral("bob"), false);
    CHECK(wait_until([&] { return verdicts.count(req); }, 3000));
    CHECK(!std::get<0>(verdicts[req]));
    CHECK(std::get<1>(verdicts[req]) ==
          QStringLiteral("deny:server-unreachable"));
    CHECK(!std::get<2>(verdicts[req]));
  }

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("file authz tests: all passed");
    return 0;
  }
  qCritical("file authz tests: %d failure(s)", g_failures);
  return 1;
}
