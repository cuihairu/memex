// 实况截图重摄（2026-10-10 原型走查 rail 面落地后的五屏浅深配平）：
// live-direct（列表态窄面板）／live-chat（直连单聊互发）／live-collab
// （协作态·bob 会话互发）／live-group（服务端群聊）／live-files（直连
// 文件通道收发）。真实服务端进程＋双直连引擎＋对端协作引擎，程序化
// 驱动真实主窗抓图（同 test_mode_switch 布局腿套路）。深色抓后显式回
// 亮色（system 档判暗兜底回读已染暗调色板——2026-10-10 配平轮教训）。
// live-org（组织架构对话框独抓）与 live-search（服务端 CLI 终端输出，
// 无窗口 chrome）不随 rail 改版，本轮核对后沿用不重摄。
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QPixmap>
#include <QProcess>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <cstdlib>
#include <functional>

#include <app/main_window.hpp>
#include <app/theme.hpp>
#include <engine/collab/collab_engine.hpp>
#include <engine/direct/direct_engine.hpp>

using memex::client::CollabEngine;
using memex::client::DirectEngine;
using memex::client::MainWindow;

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

// 布局/主题切换后让事件队列排干（resize 的 LayoutRequest、令牌推送重刷）
void settle() { wait_until([] { return true; }, 120); }

// 浅色抓一张→切暗抓一张→显式回亮（配平轮：还原必须显式 set_mode("light")）
void grab_pair(MainWindow& w, const QString& base) {
  const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
  CHECK(QDir().mkpath(dir));
  auto& tm = memex::client::ThemeManager::instance();
  tm.apply(qApp);
  settle();
  CHECK(w.grab().save(dir + QStringLiteral("/") + base +
                      QStringLiteral("-light.png")));
  tm.set_mode(QStringLiteral("dark"));
  settle();
  CHECK(w.grab().save(dir + QStringLiteral("/") + base +
                      QStringLiteral("-dark.png")));
  tm.set_mode(QStringLiteral("light"));
  settle();
}

} // namespace

int main(int argc, char** argv) {
  // 独立 UDP 发现口：并发测试进程互不串扰（同 mode_switch 夹具）
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
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME", tmp.filePath(QStringLiteral("xdg-config")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("live-shots-test"));

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

  // 直连双实例（真实互发现，同生产 UDP 通道）
  DirectEngine da("dev-A2", tmp.filePath(QStringLiteral("a.db")));
  DirectEngine db("dev-B2", tmp.filePath(QStringLiteral("b.db")));
  db.set_download_dir(tmp.filePath(QStringLiteral("b-files")));
  CHECK(da.start());
  CHECK(db.start());
  CHECK(wait_until([&] { return da.has_peer("dev-B2") && db.has_peer("dev-A2"); },
                   8000));

  MainWindow window;
  window.show();
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));

  // —— ① live-direct：列表态窄面板（双设备已互发现，无聊天面板）——
  {
    auto* list = window.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(list != nullptr);
    CHECK(wait_until([&] {
      if (!list) return false;
      int n = 0;
      for (int i = 0; i < list->count(); ++i) {
        if (!list->item(i)->isHidden() &&
            !list->item(i)->data(Qt::UserRole).toString().isEmpty())
          ++n;
      }
      return n >= 2; // dev-A2 + dev-B2 都已漂入列表
    }, 10000));
    window.resize(320, 660); // 60 rail + 260 侧栏（窄面板实况口径）
    settle();
    grab_pair(window, QStringLiteral("live-direct"));
    window.resize(960, 640);
    settle();
  }

  // —— ② live-chat：直连单聊互发（未登录＝「直连·仅本机」＋常驻横幅）——
  {
    window.open_direct_peer(QStringLiteral("dev-B2"));
    CHECK(window.chat_panel_visible());
    CHECK(window.send_in_current_chat(
        QStringLiteral("会议室 3 点对齐合同条款，请确认")));
    const QString win_id =
        QSettings().value(QStringLiteral("direct/device_id")).toString();
    CHECK(!win_id.isEmpty());
    CHECK(db.send_text(win_id.toStdString(),
                       std::string("收到，我提前到会议室准备投影")) != 0);
    CHECK(wait_until([&] {
      const QString html = window.chat_html();
      return html.contains(QStringLiteral("会议室 3 点对齐合同条款")) &&
             html.contains(QStringLiteral("收到，我提前到会议室准备投影"));
    }, 8000));
    grab_pair(window, QStringLiteral("live-chat"));
  }

  // —— ③ 登录协作态（不重启）：bob 对端真引擎在线 ——
  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));
  CollabEngine bob;
  bool bob_in = false;
  QObject::connect(&bob, &CollabEngine::logged_in, &bob,
                   [&](const QString&, const QString&) { bob_in = true; });
  bob.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
            QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return bob_in; }, 8000));

  // —— ④ live-collab：协作单聊互发（侧栏 置顶／最近联系 分组＋在线点）——
  {
    CHECK(wait_until([&] {
      return window.online_accounts().contains(QStringLiteral("bob"));
    }, 8000));
    window.open_collab_peer(QStringLiteral("bob"));
    CHECK(window.send_in_current_chat(
        QStringLiteral("本周四 14:00 里程碑评审，B 区会议室，请提前准备联调数据")));
    CHECK(bob.send_text(QStringLiteral("alice"),
                        QStringLiteral("收到，我这边用「合同」复核了 3 条命中，"
                                       "归档时间线完整")) != 0);
    CHECK(wait_until([&] {
      const QString html = window.chat_html();
      return html.contains(QStringLiteral("里程碑评审，B 区会议室")) &&
             html.contains(QStringLiteral("归档时间线完整"));
    }, 8000));
    grab_pair(window, QStringLiteral("live-collab"));
  }

  // —— ⑤ live-group：服务端群聊（bob 建群拉 alice，群消息扇出归档）——
  {
    // 平台-12 建群需特权（同 test_notify 腿）：bob 授 group_creator
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("org"), QStringLiteral("role"),
                             QStringLiteral("grant"), QStringLiteral("bob"),
                             QStringLiteral("group_creator"),
                             QStringLiteral("--by"), QStringLiteral("alice"),
                             QStringLiteral("--db"), srv_db}) == 0);
    quint64 gid = 0;
    QObject::connect(&bob, &CollabEngine::group_result, &bob,
                     [&](bool ok, const QString&, const QString&, quint64 id) {
                       if (ok && id > 0 && gid == 0) gid = id;
                     });
    bob.create_group(QStringLiteral("研发部·里程碑评审"),
                     {QStringLiteral("alice"), QStringLiteral("bob")});
    CHECK(wait_until([&] { return gid > 0; }, 15000));
    window.refresh_groups(); // 服务端无群列表推送，重拉即得（GROUP_DATA）
    CHECK(wait_until([&] {
      return window.groups_json().contains(
          QStringLiteral("研发部·里程碑评审"));
    }, 8000));
    window.open_group(QStringLiteral("group:") + QString::number(gid));
    CHECK(bob.send_text(QStringLiteral("group:") + QString::number(gid),
                        QStringLiteral("评审纪要按合同基线归档，检索关键词已挂"
                                       "「里程碑」")) != 0);
    CHECK(wait_until([&] {
      return window.chat_html().contains(QStringLiteral("检索关键词已挂"));
    }, 8000));
    grab_pair(window, QStringLiteral("live-group"));
  }

  // —— ⑥ live-files：直连文件通道收发（跨态直连会话＋真实传输落盘）——
  {
    window.open_direct_peer(QStringLiteral("dev-B2"));
    // 收方宣告真实协作账号（授权四问「收方须为真实账号」——同 mode_switch
    // 图片消息腿）：宣告落地＝跨态判定解除
    db.set_collab_account("bob");
    CHECK(wait_until([&] {
      return !window.banner_text().contains(QStringLiteral("跨态"));
    }, 8000));
    // 发：主窗发整目录（生产缝，[文件夹] 系统行明示进度与完成）
    const QString win_dir = tmp.filePath(QStringLiteral("合同归档"));
    CHECK(QDir().mkpath(win_dir));
    {
      QFile f(win_dir + QStringLiteral("/对账单-202609.csv"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("period,amount\n2026-09,128400\n");
    }
    CHECK(!window.send_folder_to_current_chat(win_dir).isEmpty());
    CHECK(wait_until([&] {
      return window.chat_html().contains(
          QStringLiteral("[文件夹] 合同归档 发送完成"));
    }, 10000));
    // 收：对端引擎发真实 PDF（file_received→[文件] 已接收系统行）
    const QString pdf = tmp.filePath(QStringLiteral("合同扫描件-2026Q3.pdf"));
    {
      QFile f(pdf);
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("%PDF-1.4\n% live-shots fixture\n");
    }
    const QString win_id =
        QSettings().value(QStringLiteral("direct/device_id")).toString();
    CHECK(!db.send_file(win_id.toStdString(), pdf).empty());
    CHECK(wait_until([&] {
      return window.chat_html().contains(QStringLiteral("合同扫描件-2026Q3.pdf"));
    }, 15000));
    grab_pair(window, QStringLiteral("live-files"));
    db.set_collab_account("");
  }

  da.stop();
  db.stop();
  server.kill();
  CHECK(server.waitForFinished(5000));
  window.close();

  if (g_failures == 0) {
    qInfo("live shots: all passed");
    return 0;
  }
  qCritical("live shots: %d failure(s)", g_failures);
  return 1;
}
