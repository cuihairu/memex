// R27-1 任务清单窗口冒烟：真服务端进程 × 离屏 QDialog。
// CollabEngine 建群（alice+bob 同群=分配资格夹具）→ TaskDialog 连接文件
// 面→自建→分配给 bob（权限模型「同群可派」）→ bob 侧清单见分派来源→
// bob 勾完成→bob 撤回→到期提醒（通知中心触发＋服务端回执只提醒一次）。
// 判权矩阵（无关系 default-deny/主人专属动作）走 test_files_api 协议腿，
// 不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <app/notify_center.hpp>
#include <app/task_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::TaskDialog;
using memex::client::LocalStore;
using memex::client::NotificationCenter;

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
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }

  // 平台-12 建群需特权：alice 授 group_creator（建同群夹具=分配资格）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("alice"),
             QStringLiteral("group_creator"), QStringLiteral("--by"),
             QStringLiteral("alice"), QStringLiteral("--db"),
             db}) == 0);

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

  // 建群（alice 建，拉 bob）：同群=task-assign 规则的现查数据面
  LocalStore store_ce;
  CHECK(store_ce.open(tmp.filePath(QStringLiteral("ce.db"))));
  CollabEngine ce;
  ce.attach_store(&store_ce);
  bool ce_in = false;
  quint64 gid = 0;
  QObject::connect(&ce, &CollabEngine::logged_in, &ce,
                   [&](const QString&, const QString&) { ce_in = true; });
  QObject::connect(&ce, &CollabEngine::group_result, &ce,
                   [&](bool ok, const QString&, const QString& op, quint64 id) {
                     if (ok && op == QStringLiteral("create")) gid = id;
                   });
  ce.login(QStringLiteral("127.0.0.1"), collab_port, QStringLiteral("alice"),
           QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return ce_in; }, 8000));
  ce.create_group(QStringLiteral("任务测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));
  (void)gid;

  // 通知计数（提醒腿的观察点）
  int tray_notifies = 0;
  QObject::connect(&NotificationCenter::instance(),
                   &NotificationCenter::want_tray_notify, &app,
                   [&](const QString&, const QString&) { ++tray_notifies; });

  TaskDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // 正口令：已连接并自动拉空列表
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      8000));

  // 自建一条（不设提醒）
  CHECK(dlg.add_task(QStringLiteral("自建任务"), QString(), 0, QString()));
  CHECK(wait_until([&] { return dlg.list()->count() == 1; }, 8000));
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("自建")));
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("自建任务")));

  // 空标题本地拒（不发网）
  const int before = dlg.list()->count();
  CHECK(!dlg.add_task(QString(), QString(), 0, QString()));

  // 分配给 bob（alice/bob 同在任务测试群→task-assign 命中）
  CHECK(dlg.add_task(QStringLiteral("帮我看下机器"), QStringLiteral("10.0.0.9"),
                     0, QStringLiteral("bob")));
  CHECK(wait_until([&] { return dlg.list()->count() == 2; }, 8000));
  // 我的清单在前、我派出的行缀尾（[→] 派给 bob）
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("自建任务")));
  CHECK(dlg.list()->item(1)->text().contains(QStringLiteral("派给 bob")));

  // bob 侧：清单出现 alice 派的任务（标来源）
  TaskDialog dlg_bob;
  dlg_bob.connect_to(QStringLiteral("127.0.0.1"), files_port,
                     QStringLiteral("bob"), QStringLiteral("pass-2"));
  CHECK(wait_until(
      [&] {
        return dlg_bob.is_connected() &&
               dlg_bob.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until([&] { return dlg_bob.list()->count() == 1; }, 8000));
  CHECK(dlg_bob.list()->item(0)->text().contains(
      QStringLiteral("由 alice 分配")));
  CHECK(dlg_bob.list()->item(0)->text().contains(QStringLiteral("帮我看下机器")));

  // bob 勾完成（完成归清单主人）：列表翻 [x]
  dlg_bob.list()->setCurrentRow(0);
  CHECK(dlg_bob.toggle_selected_done());
  CHECK(wait_until([&] {
    return dlg_bob.list()->item(0) != nullptr &&
           dlg_bob.list()->item(0)->text().startsWith(QStringLiteral("[x]"));
  }, 8000));

  // bob 撤回（清单主人可删）：列表清空，alice 侧显式刷新后派出行消失
  //（跨端不推数据——各端靠自己的轮询/刷新对齐）
  dlg_bob.list()->setCurrentRow(0);
  CHECK(dlg_bob.delete_selected());
  CHECK(wait_until([&] { return dlg_bob.list()->count() == 0; }, 8000));
  dlg.refresh();
  CHECK(wait_until([&] { return dlg.list()->count() == 1; }, 8000));

  // 到期提醒：alice 建一条到期任务（due 在过去）→ check_due 触发通知＋
  // 服务端回执；再次 check_due 不重复提醒（reminded_ms 已落）
  const qint64 past = QDateTime::currentMSecsSinceEpoch() - 1000;
  CHECK(dlg.add_task(QStringLiteral("到期任务"), QString(), past, QString()));
  CHECK(wait_until([&] { return dlg.list()->count() == 2; }, 8000));
  dlg.check_due();
  CHECK(wait_until([&] { return tray_notifies >= 1; }, 8000));
  // 回执落库有竞态（提醒回执与列表刷新两个连接不保证序）——节流重拉
  qint64 last_pull = 0;
  CHECK(wait_until([&] {
    for (int i = 0; i < dlg.list()->count(); ++i) {
      if (dlg.list()->item(i)->text().contains(QStringLiteral("到期任务")) &&
          dlg.list()->item(i)->data(Qt::UserRole + 3).toLongLong() > 0) {
        return true;
      }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_pull > 500) {
      last_pull = now;
      dlg.refresh();
    }
    return false;
  }, 8000));
  const int notifies_after_first = tray_notifies;
  dlg.check_due();
  dlg.refresh(); // 回执落库后显式刷新对齐（轮询窗 30s 内手动对齐）
  QApplication::processEvents();
  CHECK(tray_notifies == notifies_after_first);

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_task_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_task_dialog: " << g_failures << " check(s) FAILED\n";
  return 1;
}
