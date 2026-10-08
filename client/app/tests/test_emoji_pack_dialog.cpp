// 表情包云素材管理冒烟（需求批②）：真服务端进程 × 离屏 QDialog——
// 未连接上传本地拒→错口令状态行明示→正口令连接即拉清单（空清单）→
// 上传 PNG 素材入册（清单行含名字与大小）→幽灵文件本地拒→选中下载到
// 本地表情目录（字节往返＝面板即发口径）。上传/删除的协议判权与 magic/
// 1MiB 校验腿走服务端 test_emoji，不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/emoji_pack_dialog.hpp>

using memex::client::EmojiPackDialog;

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

  // 本地素材源（最小 PNG：magic＋IDR 头；服务端只验 magic）与下载落点
  const QString png_path = tmp.filePath(QStringLiteral("smile.png"));
  {
    QFile f(png_path);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write(QByteArray::fromHex("89504e470d0a1a0a"));
    f.write(QByteArray::fromHex("0000000d49484452"));
    f.write(QByteArray::fromHex("0000002000000020"));
    f.write(QByteArray::fromHex("0806000000"));
    f.close();
  }
  const QString local_dir = tmp.filePath(QStringLiteral("emoji"));

  EmojiPackDialog dlg(nullptr, local_dir);
  CHECK(dlg.asset_count() == 0);
  CHECK(!dlg.is_connected());

  // 未连接上传：本地拒（状态行明示）
  CHECK(!dlg.upload_from(png_path));
  CHECK(dlg.status_text().contains(QStringLiteral("未连接")));

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // 正口令：连接即拉清单（空态文案）
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("清单为空")); },
      8000));

  // 幽灵文件：本地拒（不发请求）
  CHECK(!dlg.upload_from(tmp.filePath(QStringLiteral("ghost.png"))));
  CHECK(dlg.status_text().contains(QStringLiteral("文件不存在")));

  // 上传 PNG 入册：清单一行含名字；上传回执自动刷新
  CHECK(dlg.upload_from(png_path));
  CHECK(wait_until([&] { return dlg.asset_count() == 1; }, 8000));
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("smile.png")));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("共 1 个表情")); },
      8000));

  // 下载到本地表情目录：字节往返（面板即发口径）
  dlg.list()->item(0)->setSelected(true);
  dlg.list()->setCurrentItem(dlg.list()->item(0));
  dlg.download_selected();
  const QString saved = local_dir + QStringLiteral("/smile.png");
  CHECK(wait_until([&] { return QFileInfo::exists(saved); }, 8000));
  {
    QFile f(saved);
    CHECK(f.open(QIODevice::ReadOnly));
    QFile src(png_path);
    CHECK(src.open(QIODevice::ReadOnly));
    CHECK(f.readAll() == src.readAll()); // 字节与源一致
    f.close();
    src.close();
  }
  CHECK(wait_until([&] {
    return dlg.status_text().contains(QStringLiteral("已下载到本地"));
  }, 8000));

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_emoji_pack_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_emoji_pack_dialog: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
