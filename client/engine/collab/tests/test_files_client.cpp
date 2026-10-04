// R23-3 文件助手客户端面验收：FilesClient × 真实服务端进程（进程级，
// 同 test_collab_login 口径）。--files-port 起面但不给 --s3-*＝存储未配置
// （503 降级口径）——备忘录/列表/判权全在元数据面，全链路可验；字节面
// （上传/下载过真对象存储）由服务端 test_files_api/test_s3_e2e 覆盖，
// 此处验「未配置=503 明示错误」的边界与客户端错误通道。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <engine/collab/files_client.hpp>

using memex::client::FilesClient;

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
  return cond();
}

// 找一个空闲 TCP 端口（先绑后放；本机回环竞争窗口极小）
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
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  // 建号（CLI，与协作面同源账号）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("account"), QStringLiteral("add"),
             QStringLiteral("alice"), QStringLiteral("pass-1"),
             QStringLiteral("--db"), db}) == 0);
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("account"), QStringLiteral("add"),
             QStringLiteral("bob"), QStringLiteral("pass-2"),
             QStringLiteral("--db"), db}) == 0);

  // 起服务端：文件面给端口、S3 不给＝存储未配置（元数据面照常）
  const quint16 collab_port = free_port();
  const quint16 files_port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(collab_port),
                QStringLiteral("--webhook-port"), QStringLiteral("0"),
                QStringLiteral("--files-port"), QString::number(files_port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, files_port)
               ? (probe.close(), false)
               : true;
  }, 8000));

  FilesClient cli;
  int fail_status = 0;
  QString fail_error, fail_op;
  QObject::connect(&cli, &FilesClient::request_failed,
                   [&](const QString& op, int status, const QString& error) {
                     fail_op = op;
                     fail_status = status;
                     fail_error = error;
                   });

  // —— 错口令：login_failed 带服务端原因（不产生会话）——
  bool login_failed_fired = false;
  QString login_reason;
  QObject::connect(&cli, &FilesClient::login_failed,
                   [&](const QString& reason) {
                     login_failed_fired = true;
                     login_reason = reason;
                   });
  cli.login(QStringLiteral("127.0.0.1"), files_port, QStringLiteral("alice"),
            QStringLiteral("wrong"));
  CHECK(wait_until([&] { return login_failed_fired; }, 8000));
  CHECK(!login_reason.isEmpty());
  CHECK(!cli.is_logged_in());

  // —— 正常登录 ——
  bool logged_in = false;
  QObject::connect(&cli, &FilesClient::logged_in, &cli,
                   [&] { logged_in = true; });
  cli.login(QStringLiteral("127.0.0.1"), files_port, QStringLiteral("alice"),
            QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return logged_in && cli.is_logged_in(); }, 8000));

  // —— 未登录拒发：logout 后操作统一走失败通道 ——
  bool unauth_fired = false;
  QString unauth_op;
  const QMetaObject::Connection unauth_conn = QObject::connect(
      &cli, &FilesClient::request_failed,
      [&](const QString& op, int status, const QString&) {
        if (status == 0) {
          unauth_fired = true;
          unauth_op = op;
        }
      });
  cli.logout();
  CHECK(!cli.is_logged_in());
  cli.list_inbox();
  CHECK(wait_until([&] { return unauth_fired; }, 3000));
  CHECK(unauth_op == QStringLiteral("inbox.list"));
  QObject::disconnect(unauth_conn);

  // 重新登录（继续后续用例）
  logged_in = false;
  cli.login(QStringLiteral("127.0.0.1"), files_port, QStringLiteral("alice"),
            QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return logged_in; }, 8000));

  // —— 备忘录：建/查单条/列表/改/删 全链路 ——
  // QNAM 不保证请求串行（服务端 Connection: close，两请求两条连接并发），
  // 后续列表/改/删断言前必须等前序操作完成信号落地
  qint64 created_id = 0;
  int created_count = 0;
  QObject::connect(&cli, &FilesClient::memo_created, &cli,
                   [&](qint64 id) {
                     created_id = id;
                     ++created_count;
                   });
  cli.create_memo(QStringLiteral("client walkthrough note"));
  CHECK(wait_until([&] { return created_id > 0; }, 8000));

  QString fetched;
  QObject::connect(&cli, &FilesClient::memo_fetched, &cli,
                   [&](qint64, const QString& content) { fetched = content; });
  cli.fetch_memo(created_id);
  CHECK(wait_until([&] { return fetched == QStringLiteral("client walkthrough note"); },
                   8000));

  QJsonArray memos;
  QObject::connect(&cli, &FilesClient::memo_listed, &cli,
                   [&](const QJsonArray& arr) { memos = arr; });
  cli.create_memo(QStringLiteral("newer note"));
  CHECK(wait_until([&] { return created_count == 2; }, 8000));
  memos = QJsonArray{};
  cli.list_memos();
  CHECK(wait_until([&] { return memos.size() == 2; }, 8000));
  // updated_ms 倒序：新条目在前
  CHECK(memos.at(0).toObject().value(QStringLiteral("content")).toString() ==
        QStringLiteral("newer note"));

  bool updated = false;
  QObject::connect(&cli, &FilesClient::memo_updated, &cli,
                   [&](qint64) { updated = true; });
  cli.update_memo(created_id, QStringLiteral("edited note"));
  CHECK(wait_until([&] { return updated; }, 8000));
  fetched.clear();
  cli.fetch_memo(created_id);
  CHECK(wait_until([&] { return fetched == QStringLiteral("edited note"); }, 8000));

  // —— 收件箱列表：memo 类型条目（无 S3＝无文件条目，类型字段在） ——
  QJsonArray items;
  QObject::connect(&cli, &FilesClient::inbox_listed, &cli,
                   [&](const QJsonArray& arr) { items = arr; });
  cli.list_inbox();
  CHECK(wait_until([&] { return items.size() == 2; }, 8000));
  CHECK(items.at(0).toObject().value(QStringLiteral("type")).toString() ==
        QStringLiteral("memo"));

  // —— 跨账号：bob 读/改 alice 的备忘录 → 403 default-deny ——
  FilesClient bob;
  bool bob_in = false;
  QObject::connect(&bob, &FilesClient::logged_in, &bob,
                   [&] { bob_in = true; });
  // bob 的失败通道并入同一断言旗标（403 判权走查用）
  QObject::connect(&bob, &FilesClient::request_failed,
                   [&](const QString&, int status, const QString&) {
                     fail_status = status;
                   });
  bob.login(QStringLiteral("127.0.0.1"), files_port, QStringLiteral("bob"),
            QStringLiteral("pass-2"));
  CHECK(wait_until([&] { return bob_in; }, 8000));
  fail_status = 0;
  bob.fetch_memo(created_id);
  CHECK(wait_until([&] { return fail_status == 403; }, 8000));
  fail_status = 0;
  bob.update_memo(created_id, QStringLiteral("hijack"));
  CHECK(wait_until([&] { return fail_status == 403; }, 8000));
  fail_status = 0;
  bob.delete_memo(created_id);
  CHECK(wait_until([&] { return fail_status == 403; }, 8000));

  // —— 存储未配置：inbox 上传 503 明示（字节面边界，见文件头注） ——
  fail_status = 0;
  cli.upload_inbox(QStringLiteral("/tmp/memex_files_client_missing.bin"));
  // 文件不存在 → 本地打开失败（status 0），先锁这个分支
  CHECK(wait_until([&] { return fail_op == QStringLiteral("inbox.upload") && fail_status == 0; },
                   3000));
  const QString existing = tmp.filePath(QStringLiteral("upload.bin"));
  {
    QFile w(existing);
    CHECK(w.open(QIODevice::WriteOnly));
    w.write("bytes");
  }
  fail_status = 0;
  cli.upload_inbox(existing);
  CHECK(wait_until([&] { return fail_status == 503; }, 8000));
  CHECK(fail_error.contains(QStringLiteral("503")) ||
        fail_error.contains(QStringLiteral("存储")));

  // —— 删除收尾：本人删自己的备忘录 ——
  bool deleted = false;
  QObject::connect(&cli, &FilesClient::memo_deleted, &cli,
                   [&](qint64) { deleted = true; });
  cli.delete_memo(created_id);
  CHECK(wait_until([&] { return deleted; }, 8000));
  memos = QJsonArray{};
  cli.list_memos();
  CHECK(wait_until([&] { return memos.size() == 1; }, 8000));

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("files client tests: all passed");
    return 0;
  }
  qCritical("files client tests: %d failure(s)", g_failures);
  return 1;
}
