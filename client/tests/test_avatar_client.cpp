// 用户头像客户端面（需求批⑫）：真服务端进程 × 离屏部件——
// avatar_store（默认头像组 FNV-1a 稳定取、缩放缓存落盘/命中/回落）、
// CropDialog（程序化取景＝正方窗口、颜色映射、256×256 输出）、
// FilesClient 头像域（登录→四档上传回 ver→下载字节往返→未设置 404 走
// 失败通道→删除后 404）。协议判权与 magic/1MiB 校验腿走服务端
// test_avatar，不在此重复。
#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/avatar_store.hpp>
#include <app/crop_dialog.hpp>
#include <engine/collab/files_client.hpp>

using memex::client::CropDialog;
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

// 真编码 PNG（QImage 出图，与生产上传同路径）
QByteArray png_bytes(int side, QRgb color) {
  QImage img(side, side, QImage::Format_ARGB32);
  img.fill(color);
  QByteArray bytes;
  QBuffer buf(&bytes);
  buf.open(QIODevice::WriteOnly);
  img.save(&buf, "PNG");
  return bytes;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  // 缩放缓存隔离到临时目录（镜像 MEMEX_TEST_EMOJI_DIR 惯例）
  qputenv("MEMEX_TEST_AVATAR_DIR", tmp.filePath(QStringLiteral("avatar-cache"))
                                       .toUtf8());
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME", tmp.filePath(QStringLiteral("xdg-config")).toUtf8());

  // —— avatar_store：默认头像组（稳定 hash＋资源加载＋缓存面）——
  CHECK(memex::client::default_avatar_index(QStringLiteral("a")) ==
        1); // FNV-1a("a")=0xe40c292c，%4+1 已知向量（防算法漂移）
  for (const QString& a : {QStringLiteral("alice"), QStringLiteral("bob"),
                           QStringLiteral("张三"), QString()}) {
    const int idx = memex::client::default_avatar_index(a);
    CHECK(idx >= 1 && idx <= memex::client::kDefaultAvatarCount);
    CHECK(idx == memex::client::default_avatar_index(a)); // 稳定
    const QPixmap pm = memex::client::default_avatar(a, 32);
    CHECK(!pm.isNull() && pm.width() == 32 && pm.height() == 32);
  }
  { // 分布性：20 账号至少命中 2 张（hash 全挤一张＝取面退化）
    QSet<int> seen;
    for (int i = 0; i < 20; ++i) {
      seen.insert(memex::client::default_avatar_index(
          QStringLiteral("user%1").arg(i)));
    }
    CHECK(seen.size() >= 2);
  }
  const QString me = QStringLiteral("alice");
  const QPixmap none = memex::client::cached_avatar(me, 7, 32);
  CHECK(none.isNull()); // 未落盘=未命中
  CHECK(memex::client::cached_avatar(me, 0, 32).isNull()); // ver=0 恒默认
  QPixmap sample(32, 32);
  sample.fill(QColor(255, 0, 0));
  memex::client::store_avatar_cache(me, 7, 32, sample);
  {
    const QPixmap hit = memex::client::cached_avatar(me, 7, 32);
    CHECK(!hit.isNull());
    QPixmap hit2 = memex::client::cached_avatar(QStringLiteral("me2"), 7, 32);
    CHECK(hit2.isNull()); // 账号隔离
  }
  CHECK(!memex::client::avatar_for(me, 7, 32).isNull());
  CHECK(memex::client::avatar_for(me, 0, 32).width() == 32);
  CHECK(memex::client::avatar_tooltip_src(me, 0, 32)
            .startsWith(QStringLiteral(":/avatars/avatar-"))); // 默认走资源
  CHECK(QFile::exists(memex::client::avatar_tooltip_src(me, 7, 32)));

  // —— CropDialog：程序化取景（左红右蓝源图＝映射可断言）——
  {
    QImage src(400, 300, QImage::Format_ARGB32);
    src.fill(QColor(200, 30, 30));
    for (int y = 0; y < 300; ++y) {
      for (int x = 200; x < 400; ++x) src.setPixel(x, y, qRgb(30, 30, 200));
    }
    CropDialog dlg(src);
    const QRectF win = dlg.source_window();
    CHECK(win.width() == win.height()); // 恒正方
    dlg.set_view(1.0, 0.5, 0.5);        // 居中：窗口 300×300，x∈[50,350)
    const QRectF c = dlg.source_window();
    CHECK(c.width() == c.height() && c.width() == 300.0);
    const QImage out = dlg.cropped();
    CHECK(out.width() == 256 && out.height() == 256);
    // 颜色映射：中线左半红右半蓝（窗口横跨分界 x=200）
    CHECK(out.pixel(64, 128) == qRgb(200, 30, 30));
    CHECK(out.pixel(192, 128) == qRgb(30, 30, 200));
    // 放大＋中心钉左半：全红（窗口 150×150 中心 x=40<200）
    dlg.set_view(2.0, 40.0 / 400.0, 0.5);
    const QImage zoomed = dlg.cropped();
    CHECK(zoomed.width() == 256);
    CHECK(zoomed.pixel(128, 128) == qRgb(200, 30, 30));
    // 中心越界夹取：窗口不越出源图
    dlg.set_view(2.0, 5.0, 0.5);
    const QRectF w2 = dlg.source_window();
    CHECK(w2.left() >= 0.0 && w2.right() <= 400.0);
  }

  // —— FilesClient 头像域：真服务端全链 ——
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("account"), QStringLiteral("add"),
                           me, QStringLiteral("pass-1"), QStringLiteral("--db"),
                           db}) == 0);
  // 双端口 free_port() + 重试整个 server 启动（最多 3 次，与
  // client/app/tests/test_approval_dialog 同构）：吸收 CI 共享 runner
  // 竞态——free_port 探活与 serve bind 之间端口可被抢占，文件面 bind
  // 失败即全腿级联
  quint16 collab_port = 0;
  quint16 files_port = 0;
  QProcess server;
  bool server_ok = false;
  for (int attempt = 0; attempt < 3 && !server_ok; ++attempt) {
    collab_port = free_port();
    files_port = free_port();
    if (collab_port == 0 || files_port == 0) {
      QThread::msleep(100);
      continue;
    }
    server.setProcessChannelMode(QProcess::ForwardedChannels);
    server.start(server_bin,
                 {QStringLiteral("serve"), QStringLiteral("--db"), db,
                  QStringLiteral("--port"), QString::number(collab_port),
                  QStringLiteral("--webhook-port"), QStringLiteral("0"),
                  QStringLiteral("--files-port"),
                  QString::number(files_port)});
    if (!server.waitForStarted(5000)) {
      qCritical("FAIL server 启动超时（尝试 %d/3）", attempt + 1);
      continue;
    }
    if (wait_until([&] {
          QTcpServer probe;
          return probe.listen(QHostAddress::LocalHost, files_port)
                     ? (probe.close(), false)
                     : true;
        }, 15000)) {
      server_ok = true;
      break;
    }
    qCritical("FAIL files_port %u 探活超时（尝试 %d/3），重试", files_port,
              attempt + 1);
    server.kill();
    server.waitForFinished(3000);
  }
  if (!server_ok) {
    qCritical("FAIL server 启动重试耗尽");
    return 1;
  }

  bool logged_in = false;
  int failed_status = 0;
  QString failed_op, failed_err;
  qint64 uploaded_ver = 0;
  int uploaded_size = 0;
  QByteArray fetched_bytes;
  bool deleted = false;
  FilesClient client;
  QObject::connect(&client, &FilesClient::logged_in, &client,
                   [&] { logged_in = true; });
  QObject::connect(&client, &FilesClient::request_failed, &client,
                   [&](const QString& op, int status, const QString& error) {
                     failed_op = op;
                     failed_status = status;
                     failed_err = error;
                   });
  QObject::connect(&client, &FilesClient::avatar_uploaded, &client,
                   [&](int size, qint64 ver) {
                     uploaded_size = size;
                     uploaded_ver = ver;
                   });
  QObject::connect(&client, &FilesClient::avatar_fetched, &client,
                   [&](const QString&, int, const QByteArray& bytes) {
                     fetched_bytes = bytes;
                   });
  QObject::connect(&client, &FilesClient::avatar_deleted, &client,
                   [&] { deleted = true; });

  client.login(QStringLiteral("127.0.0.1"), files_port, me,
               QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return logged_in; }, 8000));

  const QByteArray b64 = png_bytes(64, qRgb(10, 200, 60));
  client.avatar_upload(64, b64);
  CHECK(wait_until([&] { return uploaded_ver > 0; }, 8000));
  CHECK(uploaded_size == 64);
  const qint64 ver1 = uploaded_ver;

  // 未设置 404 走失败通道（他人/未设同口径）
  client.avatar_download(QStringLiteral("ghost"), 64);
  CHECK(wait_until([&] { return failed_status == 404; }, 8000));
  CHECK(failed_op == QStringLiteral("avatar.download"));
  failed_status = 0;

  // 下载字节往返（本人；他人可读腿在服务端 test_avatar）
  client.avatar_download(me, 64);
  CHECK(wait_until([&] { return !fetched_bytes.isEmpty(); }, 8000));
  CHECK(fetched_bytes == b64);
  // 下载回包能被 QImage 解（展示面可用性）
  {
    QPixmap pm;
    CHECK(pm.loadFromData(fetched_bytes));
    CHECK(!pm.isNull());
  }
  fetched_bytes.clear();

  // 非图 415 走失败通道（服务端 magic 校验，客户端不预检字节）
  client.avatar_upload(64, "plain text not an image");
  CHECK(wait_until([&] { return failed_status == 415; }, 8000));
  CHECK(failed_op == QStringLiteral("avatar.upload"));
  failed_status = 0;

  // 换图上传：ver 前进；下载到新字节
  const QByteArray b2 = png_bytes(64, qRgb(3, 3, 250));
  client.avatar_upload(64, b2);
  CHECK(wait_until([&] { return uploaded_ver > ver1; }, 8000));
  client.avatar_download(me, 64);
  CHECK(wait_until([&] { return !fetched_bytes.isEmpty(); }, 8000));
  CHECK(fetched_bytes == b2);
  fetched_bytes.clear();

  // 删除：回落默认（再下载 404）
  client.avatar_delete();
  CHECK(wait_until([&] { return deleted; }, 8000));
  client.avatar_download(me, 64);
  CHECK(wait_until([&] { return failed_status == 404; }, 8000));

  server.kill();
  server.waitForFinished(5000);

  if (g_failures == 0) {
    std::cout << "avatar client tests: all passed\n";
    return 0;
  }
  std::cerr << "avatar client tests: " << g_failures << " failure(s)\n";
  return 1;
}
