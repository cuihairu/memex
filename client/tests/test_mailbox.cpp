// 邮箱·统一收件箱视图冒烟（原型 desktop-mailbox.html）：真服务端进程 ×
// 离屏 MainWindow 全编译面（rail 邮箱徽章＋MailboxView 五列表＋infopanel）。
// 夹具：member1（群主）建群＋announce → owner1 收公告条目（待确认＋已读
// 确认台账 1/2，收件人不含设置者＝群成员 owner1/member2）；member1 群消息
// @王航（owner1 显示名）→ owner1 收 @我通知；owner1 自建任务＋提醒回执 →
// 任务到期通知；member1 连续 3 次错口令 → owner1（admin）收登录异常告警
// ＝4 条未读（公告 1＋通知 2＋告警 1）。
// 腿：①rail 徽章=4 →②进邮箱视图（五列 4 行/页签计数/脚注未读 4）＋亮暗
// 实况截图入 docs →③告警页签过滤 1 行 →④搜索「Q3」命中任务条目 →⑤公告
// 确认（台账 owner1 已确认/member2 未确认＋1 / 2；徽章降至 3）→⑥告警标记
// 已处理（处理人 owner1 留痕；state=2 清未读→徽章 2）→⑦全部标记已读
// （徽章归零＝摘牌 -1）→⑧去会话跳转离开邮箱视图（正文留在群会话）。
// 服务端 API/权限/去重全腿走 server/tests/test_mailbox.cpp，此处只验桌面面。
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <functional>

#include <app/main_window.hpp>
#include <app/mailbox_view.hpp>
#include <app/rail_nav.hpp>
#include <app/theme.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>
#include <engine/collab/files_client.hpp>

using memex::client::CollabEngine;
using memex::client::FilesClient;
using memex::client::LocalStore;
using memex::client::MainWindow;
using memex::client::MailboxView;
using memex::client::RailNav;

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
    // 行/详情面板重建走 deleteLater——嵌套泵不消化 DeferredDelete（Qt 语义，
    // 详见 test_archive_console 同款收尾）：显式排空，陈旧行不滞留计数
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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

// 布局/回包后让事件队列排干（LayoutRequest、HTTP 回包投递）
void settle() { wait_until([] { return true; }, 120); }

// 条目行定位：行内任一 QLabel 文本含 needle（标题/副题/类目胶囊）即该行
// —— 通知类可能多条（@我/任务），按标题子串消歧
QFrame* row_with_text(MailboxView* view, const QString& needle) {
  if (!view) return nullptr;
  for (QFrame* row :
       view->findChildren<QFrame*>(QStringLiteral("mbx_row"))) {
    for (QLabel* l : row->findChildren<QLabel*>()) {
      if (l->text().contains(needle)) return row;
    }
  }
  return nullptr;
}

// 行内操作钮（按文案；离屏 click() 同步走真实槽链）
bool click_row_op(QFrame* row, const QString& text) {
  if (!row) return false;
  for (QPushButton* b : row->findChildren<QPushButton*>()) {
    if (b->text() == text) {
      b->click();
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  // 独立 UDP 发现口：并发测试进程互不串扰（同 live_shots 夹具）
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
  // QSettings 隔离（含 MainWindow 硬编码 QSettings("memex","collab") 的
  // files_port——NativeFormat 落 XDG_CONFIG_HOME 下，重定向后即净室）
  qputenv("XDG_DATA_HOME", tmp.filePath(QStringLiteral("xdg")).toUtf8());
  qputenv("XDG_CONFIG_HOME",
          tmp.filePath(QStringLiteral("xdg-config")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("mailbox-test"));

  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  // owner1=admin（告警受众＋王航）；member1=李文（群主）；member2=王倩
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("account"), QStringLiteral("add"),
                           QStringLiteral("owner1"),
                           QStringLiteral("pass-1"), QStringLiteral("--name"),
                           QStringLiteral("王航"), QStringLiteral("--role"),
                           QStringLiteral("admin"), QStringLiteral("--db"),
                           db}) == 0);
  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("member1"),
                                    QStringLiteral("李文")},
        std::pair<QString, QString>{QStringLiteral("member2"),
                                    QStringLiteral("王倩")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"),
                             QStringLiteral("add"), row.first,
                             QStringLiteral("pass-2"), QStringLiteral("--name"),
                             row.second, QStringLiteral("--db"), db}) == 0);
  }
  // 建群需特权（权限模型「建群需授权」）：member1 授 group_creator
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("member1"),
             QStringLiteral("group_creator"), QStringLiteral("--by"),
             QStringLiteral("owner1"), QStringLiteral("--db"), db}) == 0);

  // 双端口 free_port() + 重试整个 server 启动（最多 3 次，与
  // test_group_memo_dialog 同构）：吸收 CI 共享 runner 竞态
  quint16 collab_port = 0;
  quint16 files_port = 0;
  QProcess server;
  bool server_ok = false;
  for (int attempt = 0; attempt < 3 && !server_ok; ++attempt) {
    collab_port = free_port();
    files_port = free_port();
    if (collab_port == 0 || files_port == 0 || collab_port == files_port) {
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
    if (wait_until([&] { return port_listening(files_port); }, 15000)) {
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

  // —— 夹具①②：member1 建群（拉 owner1/member2）＋发公告 ——
  LocalStore store_fx;
  CHECK(store_fx.open(tmp.filePath(QStringLiteral("fx.db"))));
  CollabEngine fx;
  fx.attach_store(&store_fx);
  bool fx_in = false;
  quint64 gid = 0;
  bool fx_announced = false;
  quint64 ack_mention = 0;
  bool ack_mention_ok = false;
  QObject::connect(&fx, &CollabEngine::logged_in, &fx,
                   [&](const QString&, const QString&) { fx_in = true; });
  QObject::connect(
      &fx, &CollabEngine::group_result, &fx,
      [&](bool ok, const QString&, const QString& op, quint64 id) {
        if (ok && op == QStringLiteral("create")) gid = id;
        if (ok && op == QStringLiteral("announce")) fx_announced = true;
      });
  QObject::connect(&fx, &CollabEngine::text_delivered, &fx,
                   [&](quint64 seq, bool ok) {
                     if (seq == ack_mention) ack_mention_ok = ok;
                   });
  fx.login(QStringLiteral("127.0.0.1"), collab_port,
           QStringLiteral("member1"), QStringLiteral("pass-2"));
  CHECK(wait_until([&] { return fx_in; }, 8000));
  fx.create_group(QStringLiteral("研发组"),
                  {QStringLiteral("owner1"), QStringLiteral("member2")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));
  fx.announce_group(gid, QStringLiteral("季度复盘安排（需确认已读）\n第二行正文"));
  CHECK(wait_until([&] { return fx_announced; }, 8000));

  // —— 夹具③：member1 群消息 @王航 → owner1 收 @我通知 ——
  ack_mention = fx.send_text(
      QStringLiteral("group:%1").arg(gid),
      QStringLiteral("对齐会材料 @王航 请先审"));
  CHECK(ack_mention != 0);
  CHECK(wait_until([&] { return ack_mention_ok; }, 8000));

  // —— 夹具④：owner1 自建任务＋提醒回执 → 任务到期通知 ——
  FilesClient fx_files;
  bool fx_files_in = false;
  qint64 fx_task_id = 0;
  bool fx_task_reminded = false;
  QObject::connect(&fx_files, &FilesClient::logged_in, &fx_files,
                   [&] { fx_files_in = true; });
  QObject::connect(&fx_files, &FilesClient::task_created, &fx_files,
                   [&](qint64 id) { fx_task_id = id; });
  QObject::connect(&fx_files, &FilesClient::task_reminded, &fx_files,
                   [&](qint64) { fx_task_reminded = true; });
  fx_files.login(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("owner1"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return fx_files_in; }, 8000));
  fx_files.create_task(QStringLiteral("Q3 复盘文档"), QString(), 1770000000000);
  CHECK(wait_until([&] { return fx_task_id > 0; }, 8000));
  fx_files.mark_task_reminded(fx_task_id);
  CHECK(wait_until([&] { return fx_task_reminded; }, 8000));

  // —— 夹具⑤：member1 连续 3 次错口令 → admin owner1 收登录异常告警 ——
  {
    CollabEngine evil;
    int fails = 0;
    QObject::connect(&evil, &CollabEngine::login_failed, &evil,
                     [&](const QString&) { ++fails; });
    for (int i = 0; i < 3; ++i) {
      evil.login(QStringLiteral("127.0.0.1"), collab_port,
                 QStringLiteral("member1"), QStringLiteral("wrong-pw"));
      CHECK(wait_until([&] { return fails == i + 1; }, 8000));
    }
  }

  // —— 主窗登录（owner1）：协作面→文件面同源自动登录→rail 邮箱徽章首拉 ——
  MainWindow w;
  w.show();
  {
    QSettings collab_cfg(QStringLiteral("memex"), QStringLiteral("collab"));
    collab_cfg.setValue(QStringLiteral("files_port"),
                        QString::number(files_port));
    collab_cfg.sync();
  }
  w.login_collab(QStringLiteral("127.0.0.1"), collab_port,
                 QStringLiteral("owner1"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return w.collab_logged_in(); }, 8000));
  auto* rail = w.findChild<RailNav*>(QStringLiteral("rail_nav"));
  CHECK(rail);
  auto* view = w.findChild<MailboxView*>(QStringLiteral("mailbox_page"));
  CHECK(view);

  // ① 未读徽章=4（告警 1＋通知 2＋公告 1；服务端为唯一来源）
  CHECK(wait_until([&] { return rail && rail->badge_count(
                             QStringLiteral("mailbox")) == 4; }, 8000));

  // ② 进邮箱视图：视图在屏＋五列 4 行＋页签计数＋脚注未读 4
  w.enter_mailbox_view();
  settle();
  CHECK(w.mailbox_view_active());
  CHECK(view->view_active());
  CHECK(wait_until([&] { return view->row_count() == 4; }, 8000));
  CHECK(view->foot_text().contains(QStringLiteral("未读 4（告警 1 · 通知 2 · 公告 1）")));
  {
    const auto tabs = view->findChildren<QPushButton*>(
        QStringLiteral("mbx_tab"));
    CHECK(tabs.size() == 4);
    CHECK(tabs[0]->text() == QStringLiteral("全部 4"));
    CHECK(tabs[1]->text() == QStringLiteral("告警 1"));
    CHECK(tabs[2]->text() == QStringLiteral("通知 2"));
    CHECK(tabs[3]->text() == QStringLiteral("公告 1"));
  }

  // 亮暗实况截图入 docs（随拍随回亮色基线——显式 set_mode("light")）
  {
    const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
    auto& tm = memex::client::ThemeManager::instance();
    tm.apply(qApp);
    settle();
    CHECK(w.grab().save(dir + QStringLiteral("/live-mailbox-light.png")));
    tm.set_mode(QStringLiteral("dark"));
    settle();
    CHECK(w.grab().save(dir + QStringLiteral("/live-mailbox-dark.png")));
    tm.set_mode(QStringLiteral("light"));
    settle();
  }

  // ③ 告警页签：过滤 1 行（登录异常告警）
  {
    const auto tabs = view->findChildren<QPushButton*>(
        QStringLiteral("mbx_tab"));
    CHECK(tabs.size() == 4);
    tabs[1]->click();
    CHECK(wait_until([&] { return view->row_count() == 1; }, 8000));
    CHECK(view->items().size() == 1);
    CHECK(view->items()[0].toObject().value(QStringLiteral("kind"))
              .toString() == QStringLiteral("warning"));
    tabs[0]->click(); // 回「全部」
    CHECK(wait_until([&] { return view->row_count() == 4; }, 8000));
  }

  // ④ 搜索「Q3」：命中任务条目（标题含已到截止时间）
  {
    auto* search = view->findChild<QLineEdit*>();
    CHECK(search);
    search->setText(QStringLiteral("Q3"));
    CHECK(wait_until([&] { return view->row_count() == 1; }, 8000));
    CHECK(view->items()[0].toObject().value(QStringLiteral("title"))
              .toString()
              .contains(QStringLiteral("已到截止时间")));
    search->clear();
    CHECK(wait_until([&] { return view->row_count() == 4; }, 8000));
  }

  // ⑤ 公告确认：详情→台账（0/2 待确认）→确认已读→台账 owner1 已确认/
  //    member2 未确认＋1 / 2＋徽章 3
  {
    QFrame* announce_row = row_with_text(view, QStringLiteral("季度复盘安排"));
    CHECK(announce_row);
    CHECK(click_row_op(announce_row, QStringLiteral("详情")));
    const auto detail_labels = [&] {
      return view->findChild<QWidget*>(QStringLiteral("mbx_detail"))
          ->findChildren<QLabel*>();
    };
    CHECK(wait_until([&] {
      for (QLabel* l : detail_labels()) {
        if (l->text() == QStringLiteral("0 / 2")) return true;
      }
      return false;
    }, 8000));
    // 确认已读（详情面板主钮）
    bool clicked = false;
    for (QPushButton* b :
         view->findChild<QWidget*>(QStringLiteral("mbx_detail"))
             ->findChildren<QPushButton*>()) {
      if (b->text() == QStringLiteral("确认已读")) {
        b->click();
        clicked = true;
        break;
      }
    }
    CHECK(clicked);
    CHECK(wait_until([&] { return rail->badge_count(
                             QStringLiteral("mailbox")) == 3; }, 8000));
    // 台账：owner1 已确认在前、member2 未确认在后（P2 谁/何时可查）
    CHECK(wait_until([&] {
      bool owner_ok = false, member2_pending = false, ratio = false;
      for (QLabel* l : detail_labels()) {
        if (l->text().contains(QStringLiteral("owner1")) &&
            l->text().contains(QStringLiteral("已确认")))
          owner_ok = true;
        if (l->text().contains(QStringLiteral("member2")) &&
            l->text().contains(QStringLiteral("未确认")))
          member2_pending = true;
        if (l->text() == QStringLiteral("1 / 2")) ratio = true;
      }
      return owner_ok && member2_pending && ratio;
    }, 8000));
  }

  // ⑥ 告警标记已处理：处理人 owner1 留痕＋state=2 清未读→徽章 2
  {
    QFrame* warn_row = row_with_text(view, QStringLiteral("口令不符"));
    CHECK(warn_row);
    CHECK(click_row_op(warn_row, QStringLiteral("详情")));
    const auto detail_labels = [&] {
      return view->findChild<QWidget*>(QStringLiteral("mbx_detail"))
          ->findChildren<QLabel*>();
    };
    CHECK(wait_until([&] {
      for (QLabel* l : detail_labels()) {
        if (l->text() == QStringLiteral("未处理（红点持续）")) return true;
      }
      return false;
    }, 8000));
    CHECK(click_row_op(warn_row, QStringLiteral("标记已处理")));
    CHECK(wait_until([&] { return rail->badge_count(
                             QStringLiteral("mailbox")) == 2; }, 8000));
    CHECK(wait_until([&] {
      bool by_owner = false, action = false, state_pill = false;
      for (QLabel* l : detail_labels()) {
        if (l->text() == QStringLiteral("owner1")) by_owner = true;
        if (l->text() == QStringLiteral("标记已处理")) action = true;
        if (l->text() == QStringLiteral("已处理")) state_pill = true;
      }
      return by_owner && action && state_pill;
    }, 8000));
  }

  // ⑦ 全部标记已读：notice 2 条清零→徽章归零＝摘牌（badge_count -1）
  {
    auto* mark_all = view->findChild<QPushButton*>(
        QStringLiteral("mbx_mark_all"));
    CHECK(mark_all);
    mark_all->click();
    CHECK(wait_until([&] {
      return rail->badge_count(QStringLiteral("mailbox")) == -1;
    }, 8000));
    CHECK(wait_until([&] {
      return view->foot_text().contains(QStringLiteral("未读 0（告警 0 · 通知 0 · 公告 0）"));
    }, 8000));
  }

  // ⑧ 去会话跳转：@我条目正文留在群会话（离开邮箱视图＝路由落位）
  {
    QFrame* mention_row = row_with_text(view, QStringLiteral("@ 了你"));
    CHECK(mention_row);
    CHECK(click_row_op(mention_row, QStringLiteral("去会话")));
    settle();
    CHECK(!w.mailbox_view_active());
  }

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("mailbox view tests: all passed");
    return 0;
  }
  qCritical("mailbox view tests: %d failure(s)", g_failures);
  return 1;
}
