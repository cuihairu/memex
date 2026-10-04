// R23-3 块2 文件助手窗口冒烟：真服务端进程 × 离屏 QDialog。
// 错口令→状态栏明示连接失败；正口令→已连接并自动拉收件箱；存备忘录→
// 混排流落地（备忘录条目）；选中→编辑→保存→流内容更新；空文本不发。
// 字节面（上传/下载/删除）走 FilesClient 引擎级与真容器 e2e，不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <app/file_assistant.hpp>

using memex::client::FileAssistantDialog;

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

  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("account"), QStringLiteral("add"),
             QStringLiteral("alice"), QStringLiteral("pass-1"),
             QStringLiteral("--db"), db}) == 0);

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

  FileAssistantDialog dlg;
  CHECK(!dlg.is_connected());

  // —— 错口令：状态栏明示连接失败、未连接 ——
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // —— 正口令：已连接并自动拉收件箱（空态占位行 1 条）——
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] {
    return dlg.is_connected() &&
           dlg.status_text().contains(QStringLiteral("已连接"));
  }, 8000));
  CHECK(wait_until([&] { return dlg.stream_count() == 1; }, 8000));

  // —— 空文本不发 ——
  CHECK(!dlg.submit_memo());
  CHECK(!dlg.submit_text(QStringLiteral("   ")));
  CHECK(dlg.stream_count() == 1);

  // —— 存备忘录：流列表落地备忘录条目（类型与正文按角色取） ——
  CHECK(dlg.submit_text(QStringLiteral("assistant ui note")));
  CHECK(wait_until([&] {
    return dlg.stream()->item(0) != nullptr &&
           dlg.stream()
                   ->item(0)
                   ->data(Qt::UserRole + 1)
                   .toString() == QStringLiteral("memo") &&
           dlg.stream()
                   ->item(0)
                   ->data(Qt::UserRole + 3)
                   .toString() == QStringLiteral("assistant ui note");
  }, 8000));

  // —— 选中→编辑→保存：流内容更新 ——
  dlg.stream()->setCurrentRow(0);
  CHECK(dlg.edit_selected());
  CHECK(dlg.submit_text(QStringLiteral("assistant ui note (edited)")));
  CHECK(wait_until([&] {
    return dlg.stream()->item(0) != nullptr &&
           dlg.stream()
                   ->item(0)
                   ->data(Qt::UserRole + 3)
                   .toString() ==
               QStringLiteral("assistant ui note (edited)");
  }, 8000));
  // 编辑态已随保存结束复位（再进一次=取消编辑语义）；刷新重灌会清选中，先重选
  dlg.stream()->setCurrentRow(0);
  CHECK(dlg.edit_selected());
  CHECK(dlg.edit_selected()); // 取消编辑

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("files assistant tests: all passed");
    return 0;
  }
  qCritical("files assistant tests: %d failure(s)", g_failures);
  return 1;
}
