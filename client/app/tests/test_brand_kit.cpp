// 品牌物料客户端面冒烟（设计稿 docs/design/品牌物料.md §6）：真服务端
// 进程×离屏——CLI 配牌→BrandKit fetch 免鉴权 GET→换牌（标题/公司名/
// accent/logo）→缓存落地→新实例 load_cache 离线复原→CLI 清牌→fetch 回
// 默认标兜底→坏端口 fetch 静默不改现状。默认标兜底（没配就不变）＝
// 品牌面最核心口径，首腿钉死。
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPixmap>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/brand_kit.hpp>

using memex::client::BrandKit;

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
  // 缓存目录隔离：显式注入（不走真实用户缓存；首腿空目录=未配牌前提）
  const QString cache_path = tmp.filePath(QStringLiteral("brand-cache"));

  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("account"), QStringLiteral("add"),
                           QStringLiteral("alice"),
                           QStringLiteral("pass-1"), QStringLiteral("--db"),
                           db}) == 0);

  const quint16 files_port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(free_port()),
                QStringLiteral("--webhook-port"), QStringLiteral("0"),
                QStringLiteral("--files-port"),
                QString::number(files_port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, files_port)
               ? (probe.close(), false)
               : true;
  }, 8000));

  // —— 默认标兜底（没配就不变）：无牌=Memex 标题/logo、branded false、
  //     空缓存目录 load_cache 不命中 ——
  BrandKit kit;
  kit.set_cache_dir(cache_path);
  CHECK(!kit.load_cache());
  CHECK(!kit.branded());
  CHECK(kit.window_title() == QStringLiteral("Memex"));
  CHECK(!kit.window_icon().isNull()); // 默认标恒有（qrc 或程序绘制）
  CHECK(kit.logo().isNull());

  // —— CLI 配牌→fetch 换牌：标题/公司名/accent/slogan 落地 ——
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("branding"), QStringLiteral("set"),
             QStringLiteral("--company"), QStringLiteral("甲乙丙有限公司"),
             QStringLiteral("--accent"), QStringLiteral("#1a2b3c"),
             QStringLiteral("--slogan"), QStringLiteral("高效协作"),
             QStringLiteral("--db"), db}) == 0);
  kit.fetch(QStringLiteral("127.0.0.1"), files_port);
  CHECK(wait_until([&] { return kit.branded(); }, 8000));
  CHECK(kit.window_title() == QStringLiteral("甲乙丙有限公司"));
  CHECK(kit.company_name() == QStringLiteral("甲乙丙有限公司"));
  CHECK(kit.accent() == QColor(QStringLiteral("#1a2b3c")));
  CHECK(kit.slogan() == QStringLiteral("高效协作"));
  CHECK(kit.version() == 1);
  // 缓存落地（brand.json）
  CHECK(wait_until([&] {
    return QFile::exists(cache_path + QStringLiteral("/brand.json"));
  }, 8000));

  // —— 配 logo（真 PNG 由 QPixmap 产出）→fetch 后 logo 位图落地 ——
  {
    QPixmap pm(32, 32);
    pm.fill(QColor(QStringLiteral("#e16531")));
    CHECK(pm.save(tmp.filePath(QStringLiteral("logo.png")), "PNG"));
  }
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("branding"), QStringLiteral("set"),
             QStringLiteral("--logo"),
             tmp.filePath(QStringLiteral("logo.png")), QStringLiteral("--db"),
             db}) == 0);
  kit.fetch(QStringLiteral("127.0.0.1"), files_port);
  CHECK(wait_until([&] { return !kit.logo().isNull(); }, 8000));
  CHECK(kit.version() == 2);
  CHECK(wait_until([&] {
    return QFile::exists(cache_path + QStringLiteral("/logo.png"));
  }, 8000));

  // —— 离线用缓存：新实例 load_cache 复原（不走网络）——
  BrandKit offline_kit;
  offline_kit.set_cache_dir(cache_path);
  CHECK(offline_kit.load_cache());
  CHECK(offline_kit.window_title() == QStringLiteral("甲乙丙有限公司"));
  CHECK(!offline_kit.logo().isNull());
  CHECK(offline_kit.accent() == QColor(QStringLiteral("#1a2b3c")));

  // —— fetch 失败静默（坏端口）：状态不被清 ——
  kit.fetch(QStringLiteral("127.0.0.1"), 1);
  wait_until([] { return false; }, 300); // 给失败回包时间（静默=不发信号）
  CHECK(kit.window_title() == QStringLiteral("甲乙丙有限公司"));

  // —— CLI 清牌→fetch 回默认标兜底 ——
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("branding"), QStringLiteral("set"),
             QStringLiteral("--company"), QString(),
             QStringLiteral("--accent"), QString(),
             QStringLiteral("--slogan"), QString(),
             QStringLiteral("--clear-logo"), QStringLiteral("--db"),
             db}) == 0);
  kit.fetch(QStringLiteral("127.0.0.1"), files_port);
  CHECK(wait_until([&] { return !kit.branded(); }, 8000));
  CHECK(kit.window_title() == QStringLiteral("Memex"));
  CHECK(kit.logo().isNull());
  CHECK(!kit.window_icon().isNull()); // 默认标回落仍恒有

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_brand_kit: all checks passed\n";
    return 0;
  }
  std::cout << "test_brand_kit: " << g_failures << " check(s) FAILED\n";
  return 1;
}
