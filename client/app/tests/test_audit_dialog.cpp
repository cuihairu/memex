// 二期·会话审计窗口冒烟：真服务端进程 × 离屏 QDialog。
// CollabEngine member1→owner1 造归档消息（协作态全量落库）→AuditDialog
// 连接文件面→无角色（member1）检索 403 状态行明示→授 auditor（CLI）→
// owner1 持证检索关键词命中归档行→查阅日志台账见本人检索（含条件摘要
// 与命中数）与 member1 被拒留痕（audit.denied 可对账）。admin/auditor
// 分立（SystemAdmin 不自动可读）协议腿走 test_files_api，不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/audit_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::AuditDialog;
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
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return cond();
}

quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("member1"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("owner1"),
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }

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

  // 造归档夹具：member1 登录协作态发一条含关键词的直聊（T2.3 全量落库）
  LocalStore store_ce;
  CHECK(store_ce.open(tmp.filePath(QStringLiteral("ce.db"))));
  CollabEngine ce;
  ce.attach_store(&store_ce);
  bool ce_in = false;
  quint64 ack_seq = 0;
  bool ack_ok = false;
  QObject::connect(&ce, &CollabEngine::logged_in, &ce,
                   [&](const QString&, const QString&) { ce_in = true; });
  QObject::connect(&ce, &CollabEngine::text_delivered, &ce,
                   [&](quint64 seq, bool ok) {
                     if (seq == ack_seq) ack_ok = ok;
                   });
  ce.login(QStringLiteral("127.0.0.1"), collab_port, QStringLiteral("member1"),
           QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return ce_in; }, 8000));
  ack_seq = ce.send_text(QStringLiteral("owner1"),
                         QStringLiteral("含审计暗号XYZ的正文"));
  CHECK(ack_seq != 0);
  CHECK(wait_until([&] { return ack_ok; }, 8000));

  AuditDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // member1 无 auditor 角色：检索 403 状态行明示（被拒尝试服务端留痕）
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(dlg.run_search(QString(), QStringLiteral("审计暗号"), 0, 0));
  CHECK(wait_until(
      [&] {
        return dlg.status_text().contains(QStringLiteral("操作失败")) &&
               dlg.status_text().contains(QStringLiteral("auditor"));
      },
      8000));
  CHECK(dlg.result_count() == 0);

  // 授 owner1 auditor（CLI；admin≠auditor 的分立腿在协议段）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("owner1"),
             QStringLiteral("auditor"), QStringLiteral("--by"),
             QStringLiteral("owner1"), QStringLiteral("--db"),
             db}) == 0);

  // owner1 持证检索：关键词命中归档行
  AuditDialog dlg_owner;
  dlg_owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                       QStringLiteral("owner1"), QStringLiteral("pass-2"));
  CHECK(wait_until(
      [&] {
        return dlg_owner.is_connected() &&
               dlg_owner.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(dlg_owner.run_search(QString(), QStringLiteral("审计暗号"), 0, 0));
  CHECK(wait_until([&] { return dlg_owner.result_count() == 1; }, 8000));
  CHECK(dlg_owner.list()->item(0)->text().contains(
      QStringLiteral("审计暗号XYZ")));
  CHECK(dlg_owner.list()->item(0)->text().contains(
      QStringLiteral("member1→owner1")));

  // 查阅日志台账：member1 的被拒（audit.denied）与 owner1 的检索
  //（含条件摘要与命中数）都在——审计自身被拒可对账
  CHECK(wait_until([&] {
    for (int i = 0; i < dlg_owner.reads_list()->count(); ++i) {
      const QString row = dlg_owner.reads_list()->item(i)->text();
      if (row.contains(QStringLiteral("audit.denied")) &&
          row.contains(QStringLiteral("member1"))) {
        return true;
      }
    }
    return false;
  }, 8000));
  CHECK(wait_until([&] {
    for (int i = 0; i < dlg_owner.reads_list()->count(); ++i) {
      const QString row = dlg_owner.reads_list()->item(i)->text();
      if (row.contains(QStringLiteral("audit.message.search")) &&
          row.contains(QStringLiteral("owner1")) &&
          row.contains(QStringLiteral("关键词=审计暗号")) &&
          row.contains(QStringLiteral("命中1条"))) {
        return true;
      }
    }
    return false; // 检索后台账回包落地才可见（轮询等第二次刷新）
  }, 8000));

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_audit_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_audit_dialog: " << g_failures << " check(s) FAILED\n";
  return 1;
}
