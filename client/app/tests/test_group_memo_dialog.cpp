// R24-2 群备忘录窗口冒烟：真服务端进程 × 离屏 QDialog。
// CollabEngine 建群（TCP 面）→ GroupMemoDialog 连接文件面→自动拉列表
// （空态占位）→新建→编辑→中文搜索→开放编辑勾态同步→历史窗填充→
// 选中笔回滚。权限面（成员 403/删恒归管理员）与回滚全链走
// test_files_client 引擎级腿，不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <app/group_memo_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::GroupMemoDialog;
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
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("bob"),
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

  // 建群（alice 建，拉 bob）：group_result 回执取 gid（顶层 connect）
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
  ce.create_group(QStringLiteral("备忘录测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));

  GroupMemoDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // 正口令：已连接；set_group 后自动拉列表（gid=0 时不拉）
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] {
    return dlg.is_connected() &&
           dlg.status_text().contains(QStringLiteral("已连接"));
  }, 8000));
  dlg.set_group(gid, QStringLiteral("备忘录测试群"));
  CHECK(dlg.windowTitle().contains(QStringLiteral("备忘录测试群")));
  CHECK(wait_until([&] { return dlg.stream_count() == 1; }, 8000)); // 空态占位

  // 新建（群主可写）：列表落地真条目
  CHECK(dlg.submit_entry(QStringLiteral("值班表"),
                         QStringLiteral("host=10.0.0.1 port=5432")));
  CHECK(wait_until([&] {
    return dlg.stream_count() == 1 &&
           dlg.stream()->item(0) != nullptr &&
           dlg.stream()->item(0)->data(Qt::UserRole + 3).toString() ==
               QStringLiteral("值班表");
  }, 8000));

  // 选中→编辑态→保存修改：内容更新（kRoleContent=UserRole+4）
  dlg.stream()->setCurrentRow(0);
  CHECK(dlg.edit_selected());
  CHECK(dlg.submit_entry(QStringLiteral("值班表"),
                         QStringLiteral("host=10.0.0.2")));
  CHECK(wait_until([&] {
    return dlg.stream()->item(0) != nullptr &&
           dlg.stream()->item(0)->data(Qt::UserRole + 4).toString() ==
               QStringLiteral("host=10.0.0.2");
  }, 8000));
  dlg.stream()->setCurrentRow(0);
  CHECK(dlg.edit_selected());
  CHECK(dlg.edit_selected()); // 取消编辑

  // 开放编辑勾态随服务端回包同步（listed 带 open_edit=false）
  CHECK(!dlg.open_edit_checked());

  // 中文搜索：搜「值班」仍一条；搜不相关词空态
  dlg.search_box()->setText(QStringLiteral("值班"));
  dlg.refresh();
  CHECK(wait_until([&] { return dlg.stream_count() == 1; }, 8000));
  dlg.search_box()->setText(QStringLiteral("不存在的词"));
  dlg.refresh();
  CHECK(wait_until([&] { return dlg.stream_count() == 1; }, 8000)); // 空态占位
  dlg.search_box()->clear();
  dlg.refresh();
  CHECK(wait_until([&] { return dlg.stream_count() == 1; }, 8000));

  // 历史窗：填充两笔修订（倒序）；选中首笔回滚→窗自关＋列表刷新
  dlg.stream()->setCurrentRow(0);
  dlg.open_history();
  CHECK(wait_until([&] { return dlg.history_count() == 2; }, 8000));
  CHECK(dlg.history_list() != nullptr);
  dlg.history_list()->setCurrentRow(1); // 倒序数组 at(1)=首笔
  dlg.rollback_selected_rev();
  CHECK(wait_until([&] {
    return dlg.history_count() == -1 && // rolled_back 关历史窗
           dlg.stream()->item(0) != nullptr &&
           dlg.stream()->item(0)->data(Qt::UserRole + 4).toString() ==
               QStringLiteral("host=10.0.0.1 port=5432");
  }, 8000));

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("group memo dialog tests: all passed");
    return 0;
  }
  qCritical("group memo dialog tests: %d failure(s)", g_failures);
  return 1;
}
