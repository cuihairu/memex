// T2.4 模式切换与降级验收（真实服务端进程 + 真实主窗）：
// ① 登录／登出不重启切换形态，本地历史跨切换保留，同库合并展示且按来源
//    字段标注（直连·仅本机／协作·已归档）；
// ② 服务端不可达 → 回落直连态，界面明确提示「消息不进归档」；
// ③ 停服务端后直连态仍可正常收发（A3、A11）。
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <cstdlib>
#include <functional>

#include <app/main_window.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>
#include <engine/direct/direct_engine.hpp>

using memex::client::CollabEngine;
using memex::client::DirectEngine;
using memex::client::LocalStore;
using memex::client::MainWindow;
using memex::client::StoredMessage;

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
  return true;
}

quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

bool port_listening(quint16 port) {
  QTcpServer probe;
  if (probe.listen(QHostAddress::LocalHost, port)) {
    probe.close();
    return false;
  }
  return true;
}

} // namespace

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
  QTemporaryDir tmp;
  if (!tmp.isValid()) return 1;
  // 主窗直连引擎默认库落在 AppDataLocation——测试隔离到临时目录
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME", tmp.filePath(QStringLiteral("xdg-config")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("mode-switch-test"));

  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);
  const QString srv_db = tmp.filePath(QStringLiteral("srv.db"));
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("account"), QStringLiteral("add"),
                           QStringLiteral("alice"), QStringLiteral("pass-a"),
                           QStringLiteral("--db"), srv_db}) == 0);
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("account"), QStringLiteral("add"),
                           QStringLiteral("bob"), QStringLiteral("pass-b"),
                           QStringLiteral("--db"), srv_db}) == 0);

  const quint16 port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin, {QStringLiteral("serve"), QStringLiteral("--db"),
                            srv_db, QStringLiteral("--port"),
                            QString::number(port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));

  // 服务端 CLI 直读库（与在跑的 serve 进程并发：短暂锁冲突在轮询中自愈）
  auto run_cli = [&](const QStringList& args) {
    QProcess p;
    p.start(server_bin, args);
    if (!p.waitForFinished(5000)) return QString();
    return QString::fromUtf8(p.readAllStandardOutput());
  };

  // 直连双实例（独立于主窗，验证直连子系统与协作服务端完全无关）
  DirectEngine da("dev-A2", tmp.filePath(QStringLiteral("a.db")));
  DirectEngine db("dev-B2", tmp.filePath(QStringLiteral("b.db")));
  int db_received = 0;
  bool da_delivered = false;
  QObject::connect(&db, &DirectEngine::message_received, &db,
                   [&](const QString&, const QString&, qint64) { ++db_received; });
  QObject::connect(&da, &DirectEngine::text_delivered, &da,
                   [&](quint64, bool ok) { da_delivered = ok; });
  CHECK(da.start());
  CHECK(db.start());
  CHECK(wait_until([&] { return da.has_peer("dev-B2") && db.has_peer("dev-A2"); },
                   8000));
  CHECK(da.send_text("dev-B2", "直连基线（服务端在场）") != 0);
  CHECK(wait_until([&] { return db_received == 1 && da_delivered; }, 6000));

  // —— 主窗：初始直连态，常驻「消息不进归档」提示 ——
  MainWindow window;
  window.show();
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));
  CHECK(!window.collab_logged_in());

  // —— 登录协作态（不重启）——
  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));

  // —— T3.1：登录后可拉取组织架构（ORG_QUERY→ORG_DATA 下发生效）——
  window.request_org();
  CHECK(wait_until([&] { return window.org_json().contains("bob"); }, 8000));

  // —— 协作会话：提示条切换为归档口径 ——
  window.open_collab_peer(QStringLiteral("bob"));
  CHECK(window.banner_text().contains(QStringLiteral("全量归档")));
  CHECK(!window.banner_text().contains(QStringLiteral("消息不进归档")));

  // —— 协作会话发送：经服务端受理（回执）——
  CHECK(window.send_in_current_chat(QStringLiteral("模式切换验收消息")));
  CHECK(wait_until([&] {
    return window.status_text().contains(QStringLiteral("已送达"));
  }, 8000));

  // —— T4.5 常用联系人（主窗验收）：发送后 FAV 数据应含 bob，星标可切换 ——
  CHECK(wait_until([&] { return window.fav_json().contains("bob"); }, 8000));

  // —— T4.5 自定义表情导入（A18「表情包可导入」；发送半边＝文件通道，
  //    direct_file 已验；面板走 import_emoji 缝，QFileDialog 面不进断言）——
  {
    QTemporaryDir emoji_root;
    CHECK(emoji_root.isValid());
    const QString src_dir = emoji_root.filePath(QStringLiteral("import-src"));
    const QString dst_dir = emoji_root.filePath(QStringLiteral("emoji-store"));
    CHECK(QDir().mkpath(src_dir));
    CHECK(QDir().mkpath(dst_dir));
    qputenv("MEMEX_TEST_EMOJI_DIR", dst_dir.toUtf8());
    const QString src = src_dir + QStringLiteral("/验收表情.png");
    {
      QFile f(src);
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("fake-png-bytes");
    }
    CHECK(window.import_emoji(src)); // 首次导入成功
    CHECK(window.status_text().contains(QStringLiteral("已导入表情")));
    CHECK(QFile::exists(dst_dir + QStringLiteral("/验收表情.png")));
    CHECK(window.import_emoji(src)); // 同名重复导入＝覆盖，幂等
    CHECK(!window.import_emoji(
        src_dir + QStringLiteral("/不存在.png"))); // 源缺失明确失败不半就
    qunsetenv("MEMEX_TEST_EMOJI_DIR");
  }

  // —— 登出／再登录：不重启切换，历史不丢 ——
  window.logout_collab();
  CHECK(wait_until([&] { return !window.collab_logged_in(); }, 5000));
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));
  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));

  // —— T4.3 消息状态与多端（主窗验收）——
  // 在线表：登录推送含自己（服务端在线表变化即广播，含自己）。
  CHECK(wait_until([&] {
    return window.online_accounts().contains(QStringLiteral("alice"));
  }, 8000));
  // 发送状态＋已读：对端 bob 真引擎在线——窗口发 bob，bob 端上报已读，
  // 窗口收 READ_NOTICE → 发送中…→已送达→对方已读（单调推进，不回退）。
  CollabEngine peer;
  bool peer_in = false;
  QString peer_msg;
  QObject::connect(&peer, &CollabEngine::logged_in, &peer,
                   [&](const QString&, const QString&) { peer_in = true; });
  QObject::connect(
      &peer, &CollabEngine::message_received, &peer,
      [&](const QString&, const QString&, qint64, const QString& msg_id) {
        peer_msg = msg_id;
      });
  peer.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
             QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return peer_in; }, 8000));
  // bob 上线 → 窗口在线表含双方
  CHECK(wait_until([&] {
    return window.online_accounts().contains(QStringLiteral("bob"));
  }, 8000));
  window.open_collab_peer(QStringLiteral("bob"));
  // 先吃掉离线补投（前文发 bob 的消息，bob 离线时入队）：清空后再走本次验收
  CHECK(wait_until([&] { return !peer_msg.isEmpty(); }, 8000));
  peer_msg.clear();
  CHECK(window.send_in_current_chat(QStringLiteral("T43主窗发送状态验收")));
  CHECK(window.delivery_text().contains(QStringLiteral("发送中")));
  CHECK(wait_until([&] {
    return window.delivery_text().contains(QStringLiteral("已送达"));
  }, 8000));
  CHECK(wait_until([&] { return !peer_msg.isEmpty(); }, 8000));
  peer.mark_read(peer_msg);
  CHECK(wait_until([&] {
    return window.delivery_text().contains(QStringLiteral("已读"));
  }, 8000));

  // 单点在线提示：peer 转登 alice → 本窗被踢 → 互踢文案常驻可查、
  // 回落直连提示条；重登后恢复（peer 随块结束析构）。
  {
    peer.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
               QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return !window.collab_logged_in(); }, 8000));
    CHECK(window.kick_text().contains(QStringLiteral("顶替下线")));
    CHECK(wait_until([&] {
      return window.banner_text().contains(QStringLiteral("消息不进归档"));
    }, 5000));
    window.login_collab(QStringLiteral("127.0.0.1"), port,
                        QStringLiteral("alice"), QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));
    CHECK(window.kick_text().isEmpty()); // 新会话清除旧互踢提示
  }

  // —— T4.7 系统集成（主窗验收）——
  // 开机启动登记往返（MEMEX_TEST_AUTOSTART_DIR 覆盖到临时目录，不污染家目录）
  {
    QTemporaryDir autostart_dir;
    CHECK(autostart_dir.isValid());
    qputenv("MEMEX_TEST_AUTOSTART_DIR", autostart_dir.path().toUtf8());
    CHECK(!window.autostart_enabled());
    window.set_autostart(true);
    CHECK(window.autostart_enabled());
    CHECK(QFile::exists(autostart_dir.filePath(
        QStringLiteral("memex-client.desktop"))));
    window.set_autostart(false);
    CHECK(!window.autostart_enabled());
    qunsetenv("MEMEX_TEST_AUTOSTART_DIR");
  }
  // 系统通知：窗口隐藏（未激活）时收协作消息 → 通知面记录发送方；
  // 互踢同样走通知（T4.3 kick_text_ 不变，通知面叠加不断言面）。
  // offscreen 无托盘：tray_available() 为假但不断言（有屏环境人工核托盘气泡）。
  {
    CollabEngine peer2;
    bool peer2_in = false;
    QObject::connect(&peer2, &CollabEngine::logged_in, &peer2,
                     [&](const QString&, const QString&) { peer2_in = true; });
    peer2.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
                QStringLiteral("pass-b"));
    CHECK(wait_until([&] { return peer2_in; }, 8000));
    window.hide(); // 隐藏即未激活 → 通知路径必触发
    CHECK(peer2.send_text(QStringLiteral("alice"),
                          QStringLiteral("T47通知验收")) != 0);
    CHECK(wait_until([&] {
      return window.last_notify().contains(QStringLiteral("bob"));
    }, 8000));
    window.show();
    // 互踢通知：peer2 转登 alice → 本窗被踢 → 通知面含顶替下线
    peer2.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
                QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return !window.collab_logged_in(); }, 8000));
    CHECK(window.last_notify().contains(QStringLiteral("顶替下线")));
    // 重登恢复（后文 T4.2 块要求登录态；重登把 peer2 顶掉，其析构无影响）
    window.login_collab(QStringLiteral("127.0.0.1"), port,
                        QStringLiteral("alice"), QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));
  }

  // —— T4.2 跨态互通：已登录端 × 未登录端的直连会话 ——
  // A7：跨态会话固定「未归档」标识（横幅＋正文，常驻不可关闭）；
  // 首触上报会话建立（时间/双方/时长，无内容），登出前服务端可查。
  CHECK(wait_until([&] { return window.has_direct_peer(QStringLiteral("dev-B2")); },
                   10000));
  window.open_direct_peer(QStringLiteral("dev-B2"));
  CHECK(window.banner_text().contains(QStringLiteral("跨态")));
  CHECK(window.banner_text().contains(QStringLiteral("未归档")));
  CHECK(window.chat_html().contains(QStringLiteral("未归档"))); // A7 正文标识
  CHECK(window.chat_html().contains(QStringLiteral("归档自")));  // A8 客户端面
  CHECK(window.send_in_current_chat(QStringLiteral("跨态验收消息")));
  CHECK(wait_until([&] {
    const QString out =
        run_cli({QStringLiteral("cross"), QStringLiteral("50"), QStringLiteral("--db"),
                 srv_db});
    return out.contains(QStringLiteral("alice")) &&
           out.contains(QStringLiteral("dev-B2")) &&
           out.contains(QStringLiteral("进行中"));
  }, 8000));

  // 同库注入一条直连历史（peer 同为 bob）：合并展示按来源标注
  {
    const QString win_db = tmp.filePath(
        QStringLiteral("xdg/memex-test/mode-switch-test/memex-local.db"));
    LocalStore inject;
    CHECK(inject.open(win_db));
    StoredMessage direct_row;
    direct_row.peer = "bob";
    direct_row.from = "bob";
    direct_row.to = "local";
    direct_row.seq = 987654;
    direct_row.ts_ms = QDateTime::currentMSecsSinceEpoch() - 60000;
    direct_row.text = "直连旧消息（仅本机）";
    direct_row.source = "direct";
    CHECK(inject.append(direct_row));
    inject.close();
  }

  window.open_collab_peer(QStringLiteral("bob"));
  CHECK(wait_until([&] {
    return window.chat_html().contains(QStringLiteral("模式切换验收消息"));
  }, 5000));
  CHECK(window.chat_html().contains(QStringLiteral("直连·仅本机")));
  CHECK(window.chat_html().contains(QStringLiteral("协作·已归档")));
  CHECK(window.chat_html().contains(QStringLiteral("直连旧消息（仅本机）")));

  // —— 停服务端 → 登录回落直连态，明确提示「消息不进归档」 ——
  window.logout_collab();
  CHECK(wait_until([&] { return !window.collab_logged_in(); }, 5000));
  // T4.2：登出即闭环跨态会话（end 帧先于 LOGOUT）——服务端日志含
  // 时间/双方/时长；归档起点显示为实际登录时间（A8 服务端面）。
  CHECK(wait_until([&] {
    const QString out =
        run_cli({QStringLiteral("cross"), QStringLiteral("50"), QStringLiteral("--db"),
                 srv_db});
    return out.contains(QStringLiteral("dev-B2")) &&
           out.contains(QStringLiteral("已结束"));
  }, 8000));
  CHECK(wait_until([&] {
    const QString out =
        run_cli({QStringLiteral("messages"), QStringLiteral("alice"),
                 QStringLiteral("--db"), srv_db});
    return out.contains(QStringLiteral("归档自"));
  }, 8000));
  server.kill();
  CHECK(server.waitForFinished(5000));

  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] {
    return window.status_text().contains(QStringLiteral("服务端不可达"));
  }, 8000));
  CHECK(wait_until([&] {
    return window.status_text().contains(QStringLiteral("消息不进归档"));
  }, 3000));
  CHECK(!window.collab_logged_in());
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));

  // —— 服务端已死：直连态仍可收发（A11 直连半边）——
  db_received = 0;
  da_delivered = false;
  CHECK(da.send_text("dev-B2", "服务端已停，直连仍可用") != 0);
  CHECK(wait_until([&] { return db_received == 1 && da_delivered; }, 6000));

  da.stop();
  db.stop();
  window.close();

  if (g_failures == 0) {
    qInfo("mode switch tests: all passed");
    return 0;
  }
  qCritical("mode switch tests: %d failure(s)", g_failures);
  return 1;
}
