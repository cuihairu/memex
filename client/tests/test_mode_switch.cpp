// T2.4 模式切换与降级验收（真实服务端进程 + 真实主窗）：
// ① 登录／登出不重启切换形态，本地历史跨切换保留，同库合并展示且按来源
//    字段标注（直连·仅本机／协作·已归档）；
// ② 服务端不可达 → 回落直连态，界面明确提示「消息不进归档」；
#include "mode_switch/common.hpp"

int main(int argc, char** argv) {
  // 独立 UDP 发现口：并发测试进程互不串扰（生产默认口 2425 不受影响）
  {
    QUdpSocket probe;
    if (probe.bind(QHostAddress::AnyIPv4, 0,
                   QAbstractSocket::ShareAddress |
                       QAbstractSocket::ReuseAddressHint)) {
      qputenv("MEMEX_TEST_DISCOVERY_PORT",
              QByteArray::number(probe.localPort()));
      probe.close();
    }
  }
  if (!tmp.isValid()) return 1;
  // 主窗直连引擎默认库落在 AppDataLocation——测试隔离到临时目录
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME", tmp.filePath(QStringLiteral("xdg-config")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("mode-switch-test"));
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("account"), QStringLiteral("add"),
                           QStringLiteral("alice"), QStringLiteral("pass-a"),
                           QStringLiteral("--db"), srv_db}) == 0);
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("account"), QStringLiteral("add"),
                           QStringLiteral("bob"), QStringLiteral("pass-b"),
                           QStringLiteral("--db"), srv_db}) == 0);

  port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin, {QStringLiteral("serve"), QStringLiteral("--db"),
                            srv_db, QStringLiteral("--port"),
                            QString::number(port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));


  leg_empty();
  // 直连双实例（独立于主窗，验证直连子系统与协作服务端完全无关）
  DirectEngine da("dev-A2", tmp.filePath(QStringLiteral("a.db")));
  DirectEngine db("dev-B2", tmp.filePath(QStringLiteral("b.db")));
  QObject::connect(&db, &DirectEngine::message_received, &db,
                   [&](const QString&, const QString&, qint64, quint64) { ++db_received; });
  QObject::connect(&da, &DirectEngine::text_delivered, &da,
                   [&](quint64, bool ok) { da_delivered = ok; });
  // 需求批⑤：db 接收目录隔离到临时目录（须 start 前设置）——文件夹腿
  // 按「目录结构重建是否原样」断言（与窗口/db 共用的默认下载目录分开）
  db.set_download_dir(tmp.filePath(QStringLiteral("b-files")));
  CHECK(da.start());
  CHECK(db.start());
  CHECK(wait_until([&] { return da.has_peer("dev-B2") && db.has_peer("dev-A2"); },
                   8000));
  CHECK(da.send_text("dev-B2", "直连基线（服务端在场）") != 0);
  CHECK(wait_until([&] { return db_received == 1 && da_delivered; }, 6000));
  leg_folder(da);
  // —— 主窗：初始直连态，常驻「消息不进归档」提示 ——
  MainWindow window;
  window.show();
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));
  CHECK(!window.collab_logged_in());
  leg_layout_find(window);
  leg_collab_views(window, db);
  leg_collab_flow(window);
  CollabEngine peer;
  QObject::connect(&peer, &CollabEngine::logged_in, &peer,
                   [&](const QString&, const QString&) { peer_in = true; });
  QObject::connect(
      &peer, &CollabEngine::message_received, &peer,
      [&](const QString&, const QString&, qint64, const QString& msg_id) {
        peer_msg = msg_id;
      });
  leg_msg_cross(window, peer, da, db, run_cli);
  leg_nudge_fallback(window, peer, da, db, server, run_cli);
  leg_settings(window, da, db);
  window.close();

  if (g_failures == 0) {
    qInfo("mode switch tests: all passed");
    return 0;
  }
  qCritical("mode switch tests: %d failure(s)", g_failures);
  return 1;
}
