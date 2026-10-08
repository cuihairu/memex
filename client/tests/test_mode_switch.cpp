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
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QProcess>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QShortcut>
#include <QUdpSocket>

#include <cstdlib>
#include <functional>

#include <app/main_window.hpp>
#include <app/net_guard.hpp>
#include <app/net_remote_settings.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>
#include <engine/direct/direct_engine.hpp>

namespace away_lock = memex::client::away_lock; // 离开锁屏设置面（C++17 无 using-ns）
namespace net_blacklist = memex::client::net_blacklist;
namespace remote_control = memex::client::remote_control;
using memex::client::CollabEngine;
using memex::client::DirectEngine;
using memex::client::LocalStore;
using memex::client::MainWindow;
using memex::client::NetRemoteSettingsDialog;
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
  // 需求批⑤：db 接收目录隔离到临时目录（须 start 前设置）——文件夹腿
  // 按「目录结构重建是否原样」断言（与窗口/db 共用的默认下载目录分开）
  db.set_download_dir(tmp.filePath(QStringLiteral("b-files")));
  CHECK(da.start());
  CHECK(db.start());
  CHECK(wait_until([&] { return da.has_peer("dev-B2") && db.has_peer("dev-A2"); },
                   8000));
  CHECK(da.send_text("dev-B2", "直连基线（服务端在场）") != 0);
  CHECK(wait_until([&] { return db_received == 1 && da_delivered; }, 6000));

  // —— 需求批⑤ 发文件夹（引擎级）：递归遍历＋相对路径重建＋聚合进度 ——
  {
    const QString src_root = tmp.filePath(QStringLiteral("dir-src"));
    CHECK(QDir().mkpath(src_root + QStringLiteral("/sub")));
    {
      QFile f(src_root + QStringLiteral("/a.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("A");
    }
    {
      QFile f(src_root + QStringLiteral("/sub/b.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("B");
    }
    bool dir_done = false, dir_ok = false;
    quint64 prog_done = 0, prog_total = 0;
    QObject::connect(&da, &DirectEngine::directory_finished, &da,
                     [&](const QString&, bool ok) {
                       dir_done = true;
                       dir_ok = ok;
                     });
    QObject::connect(&da, &DirectEngine::directory_progress, &da,
                     [&](const QString&, quint64 d, quint64 t) {
                       prog_done = qMax(prog_done, d);
                       prog_total = qMax(prog_total, t);
                     });
    const QString job = da.send_directory("dev-B2", src_root);
    CHECK(!job.isEmpty());
    CHECK(wait_until([&] { return dir_done; }, 10000));
    CHECK(dir_ok);
    CHECK(prog_total == 2); // 聚合面：作业总文件数＝2
    CHECK(prog_done + 1 == prog_total); // 末次回报＝第 2/2 个文件起传
    const QString dst_root = tmp.filePath(QStringLiteral("b-files"));
    CHECK(QFile::exists(dst_root + QStringLiteral("/a.txt")));
    CHECK(QFile::exists(dst_root + QStringLiteral("/sub/b.txt"))); // 结构重建
    {
      QFile f(dst_root + QStringLiteral("/sub/b.txt"));
      CHECK(f.open(QIODevice::ReadOnly));
      CHECK(f.readAll() == QByteArray("B")); // 内容一致
    }
    // 空目录不起作业（目录须存在但无文件＝空集拒绝）
    const QString empty_dir = tmp.filePath(QStringLiteral("dir-empty"));
    CHECK(QDir().mkpath(empty_dir));
    CHECK(da.send_directory("dev-B2", empty_dir).isEmpty());
  }

  // —— 主窗：初始直连态，常驻「消息不进归档」提示 ——
  MainWindow window;
  window.show();
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));
  CHECK(!window.collab_logged_in());

  // —— 布局腿（布局令）：启动＝列表态（「左菜单+好友列表」长方形面板，
  // 不摆聊天面板）；点好友→对话面板展开；再点当前好友→关回列表态；
  // 重开＝会话照常展开（历史在库里重载）
  CHECK(!window.chat_panel_visible());
  window.open_direct_peer(QStringLiteral("dev-B2"));
  CHECK(window.chat_panel_visible());
  {
    auto* list =
        window.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(list);
    // 列表里 dev-B2 项可能还没到（发现广播在途），到不了就只验开/关口径
    QListWidgetItem* target = nullptr;
    wait_until([&] {
      for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole).toString() ==
            QStringLiteral("dev-B2")) {
          target = list->item(i);
          return true;
        }
      }
      return false;
    }, 8000);
    if (target) {
      emit list->itemClicked(target); // 再点当前会话项＝关回列表态
      CHECK(!window.chat_panel_visible());
      // 验收截图①：启动无对话＝「左菜单+好友列表」长方形面板（无聊天面板）
      {
        const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
        CHECK(QDir().mkpath(dir));
        CHECK(window.grab().save(dir + QStringLiteral("/layout-list.png")));
      }
      window.open_direct_peer(QStringLiteral("dev-B2"));
      CHECK(window.chat_panel_visible());
      // 验收截图②：点好友后＝对话面板展开
      CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                               QStringLiteral("/layout-chat.png")));

      // —— 需求批⑨ 查找：昵称/账号(设备 id)/IP 多字段，模糊+精确 ——
      // 模糊＝子串（dev- 前缀全命中）；精确＝整 id（dev-A2 只剩一台）；
      // 不存在串全隐；清空恢复（列表里 dev-A2/dev-B2 来自双引擎宣告）
      QLineEdit* search = nullptr;
      for (QLineEdit* e : window.findChildren<QLineEdit*>()) {
        if (e->placeholderText().contains(QStringLiteral("查找："))) {
          search = e;
          break;
        }
      }
      CHECK(search != nullptr);
      if (search) {
        const auto visible_count = [&]() {
          int n = 0;
          for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->isHidden() &&
                !list->item(i)->data(Qt::UserRole).toString().isEmpty()) ++n;
          }
          return n;
        };
        const int total = visible_count();
        CHECK(total >= 2); // dev-A2 + dev-B2
        search->setText(QStringLiteral("dev-"));
        CHECK(visible_count() == total); // 模糊：前缀两台都在
        search->setText(QStringLiteral("dev-A2"));
        CHECK(visible_count() == 1); // 精确：整 id 只剩一台
        search->setText(QStringLiteral("zzz-不存在"));
        CHECK(visible_count() == 0); // 无命中全隐
        search->setText(QString());
        CHECK(visible_count() == total); // 清空恢复
      }
    }
  }

  // —— 登录协作态（不重启）——
  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));

  // —— T3.1：登录后可拉取组织架构（ORG_QUERY→ORG_DATA 下发生效）——
  window.request_org();
  CHECK(wait_until([&] { return window.org_json().contains("bob"); }, 8000));

  // —— 需求批⑪ 个性签名：发送面（验收缝 apply_signature）→ 服务端受理
  //    回执 → 重拉 org → own_signature 生效（持久面断言；回执状态行是
  //    瞬态提示，不等它）——
  window.apply_signature(QStringLiteral("今天也要专注交付"));
  CHECK(wait_until([&] {
    return window.own_signature() == QStringLiteral("今天也要专注交付");
  }, 8000));

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
  // 需求批⑪：登出后签名设置被登录门拦（同步状态行断言＝同轮取值无竞态）
  window.apply_signature(QStringLiteral("不应生效"));
  CHECK(window.status_text().contains(QStringLiteral("设置签名需登录协作态")));
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

  // —— 需求批④ 截图发送＝图片消息（气泡内直接渲染图片，非系统行）——
  // 授权链修复（用户令 2026-10-09）：发送腿须落在「服务端可达＋已登录」
  // 区——此前排在停服务端降级区，文件授权 fail-closed 必拒、气泡乐观上屏
  // 掩盖未达（靠收方来图侥幸过断言）。可达态：截图/文件夹真送达。
  {
    window.open_direct_peer(QStringLiteral("dev-B2"));
    // 可达态 allow 全链：对端须有真实协作账号（授权四问第二问「收方须为
    // 真实账号」）——db 临时宣告 bob 账号（立即补一轮宣告），块尾还原。
    // 授权裁决四问：alice→bob 同部门（双方均未建档＝空路径相等）→允许。
    db.set_collab_account("bob");
    CHECK(wait_until([&] {
      return !window.banner_text().contains(QStringLiteral("跨态"));
    }, 5000)); // 对端宣告落地（跨态判定解除）＝peer 表已带出账号
    // 本端发送流：真 PNG 走 send_shot_to_current_chat（截图确认回调同一路径）
    const QString shot = tmp.filePath(QStringLiteral("shot-msg.png"));
    {
      QPixmap pm(96, 60);
      pm.fill(Qt::red);
      CHECK(pm.save(shot, "PNG"));
    }
    // 可达态授权回执：截图真送达对端（db 侧文件落地，非仅本端气泡）
    QString shot_recv;
    QObject::connect(&db, &DirectEngine::file_received, &db,
                     [&](const QString&, const QString&, const QString& p) {
                       if (p.endsWith(QStringLiteral("shot-msg.png")))
                         shot_recv = p;
                     });
    CHECK(window.send_shot_to_current_chat(shot));
    CHECK(wait_until([&] {
      return window.chat_html().contains(QStringLiteral("<img"));
    }, 8000));
    CHECK(wait_until([&] { return !shot_recv.isEmpty(); }, 15000));
    CHECK(window.last_file_error().isEmpty()); // 无失败终态
    CHECK(window.chat_html().contains(QStringLiteral("图片消息")));
    CHECK(!window.chat_html().contains(QStringLiteral("[截图] 开始发送")));
    // 收方渲染腿：db 发真 PNG 给窗口（窗口正开着 dev-B2 会话）→
    // file_received 图片且会话匹配＝气泡追加（非文件系统行）。
    // 文件名带随机段：同进程窗口与 db 共用 AppData 下载目录，基线腿
    // 落过同名文件会撞幂等重发直达终态（收不到 FILE_RESUME 数据面）
    const QString recv_png = tmp.filePath(QStringLiteral("recv-msg-%1.png")
                                              .arg(QRandomGenerator::global()
                                                       ->generate()));
    {
      QPixmap pm(80, 50);
      pm.fill(Qt::blue);
      CHECK(pm.save(recv_png, "PNG"));
    }
    const int imgs_before = window.chat_html().count(
        QStringLiteral("<img"));
    const QString win_id =
        QSettings().value(QStringLiteral("direct/device_id")).toString();
    // 对照基线：裸引擎对文件通道（da→db，test_screenshot 已验窗口发方向）
    QString db_recv;
    QObject::connect(&db, &DirectEngine::file_received, &db,
                     [&](const QString&, const QString&, const QString& path) {
                       db_recv = path;
                     });
    CHECK(!da.send_file("dev-B2", recv_png).empty());
    CHECK(wait_until([&] { return !db_recv.isEmpty(); }, 15000));
    // 探针：窗口 file_received 是否到达（验收面分离传输层 vs 渲染层）
    CHECK(!db.send_file(win_id.toStdString(), recv_png).empty());
    CHECK(wait_until([&] {
      return window.last_received_file().startsWith(
          QStringLiteral("dev-B2|"));
    }, 15000));
    CHECK(wait_until([&] {
      return window.chat_html().count(QStringLiteral("<img")) ==
             imgs_before + 1;
    }, 5000));
    // 验收截图：图片消息气泡（本端发送+收方接收两条）
    {
      const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
      CHECK(QDir().mkpath(dir));
      CHECK(window.grab().save(dir + QStringLiteral("/image-message.png")));
    }

    // —— 需求批⑤ 发文件夹（主窗验收缝）：会话开着 dev-B2 → 真发整目录，
    //    等系统行（持久面）明示完成；落盘结构一并断言 ——
    CHECK(window.has_direct_peer(QStringLiteral("dev-B2")));
    const QString win_src = tmp.filePath(QStringLiteral("win-dir"));
    CHECK(QDir().mkpath(win_src + QStringLiteral("/nested")));
    {
      QFile f(win_src + QStringLiteral("/t.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("T");
    }
    const QString win_job = window.send_folder_to_current_chat(win_src);
    CHECK(!win_job.isEmpty());
    CHECK(wait_until([&] {
      return window.chat_html().contains(
          QStringLiteral("[文件夹] win-dir 发送完成"));
    }, 10000));
    CHECK(QFile::exists(
        tmp.filePath(QStringLiteral("b-files/t.txt")))); // rel 相对作业根
    db.set_collab_account(""); // 还原匿名宣告（后文降级态跨态判定不受扰）
  }

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
  // 平台-7 降级显式化：状态栏（已有断言）之外——横幅明示「已降级」、
  // 输入框提示随态（未归档通信）、直连会话标记「已降级 · 未归档」前缀
  CHECK(window.banner_text().contains(QStringLiteral("已降级")));
  CHECK(window.input_hint().contains(QStringLiteral("未归档")));
  CHECK(window.input_hint().contains(QStringLiteral("已降级")));
  window.open_direct_peer(QStringLiteral("dev-B2"));
  CHECK(window.chat_meta().contains(QStringLiteral("已降级 · 未归档")));

  // —— 服务端已死：直连态仍可收发（A11 直连半边）——
  db_received = 0;
  da_delivered = false;
  CHECK(da.send_text("dev-B2", "服务端已停，直连仍可用") != 0);
  CHECK(wait_until([&] { return db_received == 1 && da_delivered; }, 6000));

  // —— 授权链回归（用户令 2026-10-09）：服务端不可达态＝未登录，
  //    fail-closed 本地拒（权限面不可绕开服务器，设计口径）——
  //    截图/文件夹缝同步返回失败、不乐观上屏，回执 deny:server-unreachable
  {
    window.open_direct_peer(QStringLiteral("dev-B2"));
    const QString shot_deny = tmp.filePath(QStringLiteral("shot-deny.png"));
    {
      QPixmap pm(60, 40);
      pm.fill(Qt::green);
      CHECK(pm.save(shot_deny, "PNG"));
    }
    const int imgs_before_deny =
        window.chat_html().count(QStringLiteral("<img"));
    CHECK(!window.send_shot_to_current_chat(shot_deny)); // 拒＝false
    CHECK(window.chat_html().count(QStringLiteral("<img")) ==
          imgs_before_deny); // 无假「已发送」气泡
    CHECK(window.last_file_error().contains(
        QStringLiteral("deny:server-unreachable")));
    const QString deny_dir = tmp.filePath(QStringLiteral("deny-dir"));
    CHECK(QDir().mkpath(deny_dir));
    {
      QFile f(deny_dir + QStringLiteral("/d.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("D");
    }
    CHECK(window.send_folder_to_current_chat(deny_dir).isEmpty()); // 拒＝空
    CHECK(window.last_file_error().contains(
        QStringLiteral("deny:server-unreachable")));
  }

  // —— 离开锁屏（用户令 2026-10-08 ④）：无操作超时→锁屏只显未读数不显
  //     内容→错密码拒→对密码解锁（db 仍活着：锁屏期间真来一条直连消息）——
  {
    CHECK(away_lock::set_password(QStringLiteral("lock-pwd")));
    away_lock::set_timeout_seconds(1); // 1 秒超时（测试内无输入事件＝必到期）
    away_lock::set_enabled(true);
    window.apply_away_lock_settings(); // 设置对话框关闭即此路径＝即时生效
    window.hide(); // 无输入事件，超时即锁
    CHECK(wait_until([&] { return window.lock_screen_visible(); }, 8000));
    CHECK(window.lock_screen_text().contains(QStringLiteral("暂无新消息")));
    // 锁屏期间真来一条直连消息：只累计条数；弹窗与提示音全静默
    // （消息文本不出锁屏面——last_notify 不动＝通知路径被抑制）
    const QString prev_notify = window.last_notify();
    const QString win_id =
        QSettings().value(QStringLiteral("direct/device_id")).toString();
    CHECK(!win_id.isEmpty());
    CHECK(db.send_text(win_id.toStdString(),
                       std::string("锁屏期间不该上屏的内容")) != 0);
    CHECK(wait_until([&] { return window.lock_unread_count() == 1; }, 6000));
    CHECK(window.last_notify() == prev_notify);
    CHECK(window.lock_screen_text().contains(
        QStringLiteral("锁屏期间新消息 1 条")));
    CHECK(!window.lock_screen_text().contains(QStringLiteral("不该上屏")));
    // 截图验收：锁屏遮罩面只含数量（docs/src/public/screenshots/lock-screen.png）
    {
      QWidget* lock_w = QApplication::activeWindow(); // 全屏遮罩＝当前激活窗
      CHECK(lock_w && lock_w->isVisible());
      const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
      CHECK(QDir().mkpath(dir));
      if (lock_w)
        CHECK(lock_w->grab().save(dir + QStringLiteral("/lock-screen.png")));
    }
    // 错密码拒（仍在屏）；对密码解锁
    window.lock_try_unlock(QStringLiteral("wrong-pwd"));
    CHECK(window.lock_screen_visible());
    window.lock_try_unlock(QStringLiteral("lock-pwd"));
    CHECK(wait_until([&] { return !window.lock_screen_visible(); }, 3000));
    CHECK(window.lock_unread_count() == 0); // 解锁即清（随遮罩销毁）
    // 还原：关开关（QSettings 面 XDG_CONFIG_HOME 已隔离；也保重复执行可重跑）
    away_lock::set_enabled(false);
    window.apply_away_lock_settings();
    window.show();
  }

  // —— 网络与远程设置页（用户令 2026-10-08 ⑤）：坏段本地门、开关与
  //     配对密码落盘、保存即时生效（apply_net_settings 同源调用）——
  {
    NetRemoteSettingsDialog dlg(&window);
    // 坏段本地拒不入表（段输入过 validate_cidr 门）
    CHECK(!dlg.add_entry(QStringLiteral("10.0.0/8")).isEmpty());
    CHECK(dlg.add_entry(QStringLiteral("172.16.0.0/12")).isEmpty());
    CHECK(dlg.add_entry(QStringLiteral("172.16.0.0/12")).isEmpty() ==
          false); // 重复不双录
    // 远程控制开且未设密码＝保存拒（先设配对密码门）
    auto* chk_net = dlg.findChild<QCheckBox*>("chk_blacklist");
    auto* chk_remote = dlg.findChild<QCheckBox*>("chk_remote");
    auto* edit_pwd = dlg.findChild<QLineEdit*>("edit_pwd");
    auto* edit_confirm = dlg.findChild<QLineEdit*>("edit_confirm");
    auto* list_entries = dlg.findChild<QListWidget*>("list_entries");
    CHECK(chk_net && chk_remote && edit_pwd && edit_confirm && list_entries);
    chk_net->setChecked(true);
    chk_remote->setChecked(true);
    CHECK(dlg.save_settings().contains(QStringLiteral("配对密码")));
    // 密码两次不一致拒
    edit_pwd->setText(QStringLiteral("pair-1"));
    edit_confirm->setText(QStringLiteral("pair-2"));
    CHECK(dlg.save_settings().contains(QStringLiteral("不一致")));
    // 合法保存：段表+两开关+密码落盘
    edit_confirm->setText(QStringLiteral("pair-1"));
    CHECK(dlg.save_settings().isEmpty());
    CHECK(net_blacklist::enabled());
    CHECK(net_blacklist::entries() ==
          QStringList{QStringLiteral("172.16.0.0/12")});
    CHECK(remote_control::enabled());
    CHECK(remote_control::verify_password(QStringLiteral("pair-1")));
    CHECK(!remote_control::verify_password(QStringLiteral("pair-2")));
    // 删段：选中入表段删除→保存=段表清空
    list_entries->setCurrentRow(0);
    dlg.del_entry();
    CHECK(dlg.save_settings().isEmpty());
    CHECK(net_blacklist::entries().isEmpty());
    // 还原（保重复执行可重跑；XDG_CONFIG_HOME 面已隔离）
    net_blacklist::set_enabled(false);
    remote_control::set_enabled(false);
    window.apply_net_settings();
  }

  da.stop();
  db.stop();

  // —— 截图快捷键改键＋冲突检测（用户令 2026-10-08 ③；设置页
  // ShortcutSettingsDialog 走同一路径 apply_screenshot_shortcut）——
  {
    // 造一个占用键的既有快捷键＝冲突检测的比对对象
    auto* other = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+K")),
                                &window);
    other->setObjectName(QStringLiteral("other_sc"));
    // 冲突：不改现状、返回冲突文案
    const QString conflict = window.apply_screenshot_shortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    CHECK(conflict.contains(QStringLiteral("冲突")));
    CHECK(MainWindow::screenshot_shortcut() ==
          QStringLiteral("Ctrl+Alt+A")); // 落盘未动
    // 合法键：重绑＋落盘＋回读
    CHECK(window.apply_screenshot_shortcut(
              QKeySequence(QStringLiteral("Ctrl+Alt+S")))
              .isEmpty());
    CHECK(MainWindow::screenshot_shortcut() ==
          QStringLiteral("Ctrl+Alt+S"));
    // 还原默认（不污染后续执行体；QSettings 面 XDG_CONFIG_HOME 已隔离）
    CHECK(window.apply_screenshot_shortcut(
              QKeySequence(QStringLiteral("Ctrl+Alt+A")))
              .isEmpty());
    delete other;
  }

  window.close();

  if (g_failures == 0) {
    qInfo("mode switch tests: all passed");
    return 0;
  }
  qCritical("mode switch tests: %d failure(s)", g_failures);
  return 1;
}
