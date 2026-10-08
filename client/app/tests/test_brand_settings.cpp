// 二期·品牌物料设置页冒烟（设计稿 docs/design/品牌物料.md §4/§6）：
// 真服务端进程×离屏——未连接本地拒→错口令状态行→member 保存 403 归属
// 文案→org-admin 保存文本三件换牌（本机 BrandKit 即时生效）→accent 坏
// 形态本地拒不发网→logo 选图预览＋上传（非 PNG 本地拒）→清除 logo 回落
// →截图验收（配置前后主窗 grab 落 docs/src/public/branding/）。判权与
// PNG 校验协议腿归 test_files_api，不在此重复。
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPixmap>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/brand_kit.hpp>
#include <app/brand_settings_page.hpp>
#include <app/main_window.hpp>
#include <app/theme.hpp>

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif

using memex::client::BrandKit;
using memex::client::BrandSettingsDialog;

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
  // BrandKit 缓存隔离（单例＝设置页保存后本机换牌的真面目；env 须在
  // 首次 ensure_cache_dir 前设置）
  qputenv("MEMEX_TEST_BRAND_DIR",
          tmp.filePath(QStringLiteral("brand-cache")).toUtf8());

  for (const auto* acc : {"member1", "owner1"}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"), QStringLiteral("add"),
                             QString::fromUtf8(acc),
                             QStringLiteral("pass-1"), QStringLiteral("--db"),
                             db}) == 0);
  }
  // owner1 授 org-admin（品牌判权=org-admin；CLI 授角色先例同审批窗口）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("owner1"),
             QStringLiteral("org-admin"), QStringLiteral("--by"),
             QStringLiteral("owner1"), QStringLiteral("--db"), db}) == 0);

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

  BrandSettingsDialog dlg;
  CHECK(!dlg.is_connected());

  // —— 未连接本地拒：不发包即明示 ——
  CHECK(!dlg.save_text());
  CHECK(dlg.status_text() == QStringLiteral("未连接文件面"));

  // —— 错口令：状态行明示连接失败 ——
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // —— member1（非 org-admin）：保存 403 状态行归属文案 ——
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] { return dlg.is_connected(); }, 8000));
  dlg.set_company(QStringLiteral("越权公司"));
  CHECK(dlg.save_text());
  CHECK(wait_until(
      [&] {
        return dlg.status_text().contains(
            QStringLiteral("品牌设置归 org-admin"));
      },
      8000));
  CHECK(BrandKit::instance().window_title() ==
        QStringLiteral("Memex")); // 403 不换牌

  // —— owner1（org-admin）：保存文本三件→「已保存（版本 1）」＋本机
  //     BrandKit 即时换牌 ——
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("owner1"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] { return dlg.is_connected(); }, 8000));
  dlg.set_company(QStringLiteral("甲乙丙有限公司"));
  dlg.set_accent(QStringLiteral("#1a2b3c"));
  dlg.set_slogan(QStringLiteral("高效协作"));
  CHECK(dlg.save_text());
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("已保存")); },
      8000));
  CHECK(wait_until(
      [&] {
        return BrandKit::instance().window_title() ==
               QStringLiteral("甲乙丙有限公司");
      },
      8000));
  CHECK(BrandKit::instance().accent() == QColor(QStringLiteral("#1a2b3c")));

  // —— accent 坏形态本地拒（不发网；状态行形态文案）——
  dlg.set_accent(QStringLiteral("red"));
  CHECK(!dlg.save_text());
  CHECK(dlg.status_text() ==
        QStringLiteral("主题色须为 #rrggbb 形态"));
  dlg.set_accent(QStringLiteral("#1a2b3c")); // 还原，供后续腿续用

  // —— logo：本地产真 PNG→选图预览→上传→版本 2＋BrandKit 位图落地；
  //     非 PNG 本地拒（QPixmap 打不开即拒，不发包）——
  {
    QPixmap pm(32, 32);
    pm.fill(QColor(QStringLiteral("#e16531")));
    CHECK(pm.save(tmp.filePath(QStringLiteral("logo.png")), "PNG"));
    QFile bogus(tmp.filePath(QStringLiteral("bogus.png")));
    CHECK(bogus.open(QIODevice::WriteOnly));
    bogus.write("not a png at all");
    bogus.close();
  }
  CHECK(!dlg.pick_logo(tmp.filePath(QStringLiteral("bogus.png"))));
  CHECK(dlg.status_text() == QStringLiteral("图片打不开（须 PNG）"));
  CHECK(dlg.pick_logo(tmp.filePath(QStringLiteral("logo.png"))));
  CHECK(!dlg.logo_preview().isNull()); // 本地选中即时预览
  CHECK(dlg.upload_assets());
  CHECK(wait_until(
      [&] {
        return dlg.status_text().contains(QStringLiteral("logo 已更新"));
      },
      8000));
  CHECK(wait_until([&] { return !BrandKit::instance().logo().isNull(); },
                   8000));

  // —— 清除 logo：路径/预览清空＋BrandKit 位图回落默认（文本不丢）——
  CHECK(dlg.clear_logo());
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("logo 已清除")); },
      8000));
  CHECK(wait_until([&] { return BrandKit::instance().logo().isNull(); },
                   8000));
  CHECK(BrandKit::instance().window_title() ==
        QStringLiteral("甲乙丙有限公司"));

  // —— 截图验收（设计稿 §6「配置→另一客户端连接→截图前后对比」）：
  //     独立 MainWindow 实例＝另一客户端；CLI 配新牌（与设置页保存内容
  //     不同以示前后差异）→连接后拉取→换牌前后 grab 落
  //     docs/src/public/branding/ ——
  {
    memex::client::ThemeManager::instance().apply(&app);
    memex::client::MainWindow w;
    w.show();
    QApplication::processEvents();
    const QString shot_dir = QStringLiteral(MEMEX_DOCS_BRAND_DIR);
    CHECK(QDir().mkpath(shot_dir));
    CHECK(w.grab().save(shot_dir + QStringLiteral("/before.png")));
    // 另一客户端视角：CLI 配新牌（含新 logo）
    {
      QPixmap pm(48, 48);
      pm.fill(QColor(QStringLiteral("#0b5f8a")));
      CHECK(pm.save(tmp.filePath(QStringLiteral("logo2.png")), "PNG"));
    }
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("branding"), QStringLiteral("set"),
               QStringLiteral("--company"), QStringLiteral("星河科技"),
               QStringLiteral("--accent"), QStringLiteral("#0b5f8a"),
               QStringLiteral("--slogan"), QStringLiteral("连接每一台设备"),
               QStringLiteral("--logo"),
               tmp.filePath(QStringLiteral("logo2.png")),
               QStringLiteral("--db"), db}) == 0);
    BrandKit::instance().fetch(QStringLiteral("127.0.0.1"), files_port);
    CHECK(wait_until(
        [&] { return w.windowTitle() == QStringLiteral("星河科技"); },
        8000));
    QApplication::processEvents();
    CHECK(w.grab().save(shot_dir + QStringLiteral("/after.png")));
  }

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_brand_settings: all checks passed\n";
    return 0;
  }
  std::cout << "test_brand_settings: " << g_failures << " check(s) FAILED\n";
  return 1;
}
