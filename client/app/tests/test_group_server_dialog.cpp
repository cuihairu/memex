// R26-2 群服务器面板冒烟：真服务端进程 × 离屏 QDialog。
// 管理员腿：登记（令牌只显示一次）→真 memex_agent --once 心跳→列表绿灯
// （在线）＋未打点的第二台红（从未）→重登记换台后令牌刷新。成员腿：
// 列表只读可见、越权登记 403 状态行明示。R26-3 会话腿：成员选中服务器
// 发起（签发即兑现→命令框）→收尾落时长；群主第二笔进行中同屏可见。
// R26-4 凭据腿：群主存/删凭据（列表掩码态翻转）、成员越权 403。
// 服务端判权矩阵/轮换语义/短票一次性/令牌摘要落库/凭据密文落库走
// test_files_api 与 test_agent，不在此重复。
#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <app/group_server_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::GroupServerDialog;
using memex::client::LocalStore;

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif
#ifndef MEMEX_AGENT_BIN
#error "MEMEX_AGENT_BIN 未定义（应传入 $<TARGET_FILE:memex_agent>）"
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

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }

  // 平台-12 建群需特权（权限模型「建群需授权」）：alice 授 group_creator
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("alice"),
             QStringLiteral("group_creator"), QStringLiteral("--by"),
             QStringLiteral("alice"), QStringLiteral("--db"),
             db}) == 0);

  const quint16 collab_port = free_port();
  const quint16 files_port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(collab_port),
                QStringLiteral("--webhook-port"), QStringLiteral("0"),
                QStringLiteral("--files-port"), QString::number(files_port),
                QStringLiteral("--tool-cred-secret"),
                QStringLiteral("dialog-test-cred-secret")});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, files_port)
               ? (probe.close(), false)
               : true;
  }, 8000));

  // 建群（alice 建，拉 bob）
  LocalStore store_ce;
  CHECK(store_ce.open(tmp.filePath(QStringLiteral("ce.db"))));
  CollabEngine ce;
  ce.attach_store(&store_ce);
  bool ce_in = false;
  quint64 gid = 0;
  QObject::connect(&ce, &CollabEngine::logged_in, &ce,
                   [&](const QString&, const QString&) { ce_in = true; });
  QObject::connect(&ce, &CollabEngine::group_result, &ce,
                   [&](bool ok, const QString&, const QString& op, quint64 id) {
                     if (ok && op == QStringLiteral("create")) gid = id;
                   });
  ce.login(QStringLiteral("127.0.0.1"), collab_port, QStringLiteral("alice"),
           QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return ce_in; }, 8000));
  ce.create_group(QStringLiteral("服务器测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));

  // —— 管理员腿：空列表占位→登记（令牌一次性显示）→agent 心跳→绿灯 ——
  GroupServerDialog owner;
  owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                   QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] {
    return owner.is_connected() &&
           owner.status_text().contains(QStringLiteral("已连接"));
  }, 8000));
  owner.set_group(gid, QStringLiteral("服务器测试群"));
  CHECK(owner.windowTitle().contains(QStringLiteral("服务器测试群")));
  CHECK(wait_until([&] {
    return owner.server_count() == 1 &&
           owner.server_list_widget()->item(0)->text().contains(
               QStringLiteral("暂无登记服务器"));
  }, 8000));

  // 程序化登记（确认框只挂按钮路径）：令牌只显示这一次
  CHECK(owner.enroll_server(QStringLiteral("web-1"), QStringLiteral("10.0.0.9")));
  CHECK(wait_until([&] {
    return !owner.enroll_token_text().isEmpty() &&
           owner.server_count() == 1 &&
           owner.server_list_widget()->item(0)->text().contains(
               QStringLiteral("web-1"));
  }, 8000));
  // 未打点＝红：条目明示「从未」
  CHECK(owner.server_list_widget()->item(0)->text().contains(
      QStringLiteral("从未")));

  // 真 agent --once 一拍心跳 → 刷新后绿灯（在线）＋指标落账＋「从未」消失
  const QString token = owner.enroll_token_text();
  CHECK(QProcess::execute(QStringLiteral(MEMEX_AGENT_BIN),
                          {QStringLiteral("--server"),
                           QStringLiteral("127.0.0.1:") + QString::number(files_port),
                           QStringLiteral("--token"), token,
                           QStringLiteral("--once")}) == 0);
  owner.refresh();
  CHECK(wait_until([&] {
    const auto* it = owner.server_list_widget()->item(0);
    return it != nullptr && it->text().contains(QStringLiteral("web-1")) &&
           !it->text().contains(QStringLiteral("从未")) &&
           it->foreground().color() == QColor(Qt::darkGreen); // 绿灯=在线
  }, 8000));

  // 第二台登记（db-1）不打点：两台一绿一红同屏
  CHECK(owner.enroll_server(QStringLiteral("db-1"), QStringLiteral("10.0.0.8")));
  CHECK(wait_until([&] {
    const auto* green = owner.server_list_widget()->item(0);
    const auto* red = owner.server_list_widget()->item(1);
    return owner.server_count() == 2 &&
           owner.enroll_token_text() != token && // 令牌随重登记刷新
           green != nullptr && red != nullptr &&
           green->text().contains(QStringLiteral("web-1")) &&
           green->foreground().color() == QColor(Qt::darkGreen) &&
           red->text().contains(QStringLiteral("db-1")) &&
           red->foreground().color() == QColor(Qt::red);
  }, 8000));

  // —— 成员腿：列表只读可见；越权登记 403 状态行明示 ——
  GroupServerDialog member;
  member.connect_to(QStringLiteral("127.0.0.1"), files_port,
                    QStringLiteral("bob"), QStringLiteral("pass-2"));
  CHECK(wait_until([&] { return member.is_connected(); }, 8000));
  member.set_group(gid, QStringLiteral("服务器测试群"));
  CHECK(wait_until([&] { return member.server_count() == 2; }, 8000));
  CHECK(member.enroll_server(QStringLiteral("ghost"), QStringLiteral("x")));
  CHECK(wait_until([&] {
    return member.status_text().contains(QStringLiteral("操作失败")) &&
           member.status_text().contains(QStringLiteral("无权登记"));
  }, 8000));
  CHECK(member.server_count() == 2); // 幽灵登记未进台账

  // —— SSH 会话腿：未选中本地拒；选中 web-1 发起（签发即兑现）→命令框——
  // →留痕「进行中」；收尾翻「已收尾」落时长——
  CHECK(!member.request_session()); // 未选中：本地拒绝，不出网
  CHECK(member.status_text().contains(QStringLiteral("选中")));
  member.server_list_widget()->setCurrentRow(0); // web-1（10.0.0.9，绿灯）
  CHECK(member.request_session());
  CHECK(wait_until([&] {
    return member.session_command_text() ==
               QStringLiteral("ssh 10.0.0.9") &&
           member.session_count() >= 1 &&
           member.session_list_widget()->item(0)->text().contains(
               QStringLiteral("bob")) &&
           member.session_list_widget()->item(0)->text().contains(
               QStringLiteral("进行中"));
  }, 8000));
  CHECK(member.close_session());
  CHECK(wait_until([&] {
    const auto* it = member.session_list_widget()->item(0);
    return it != nullptr && it->text().contains(QStringLiteral("已收尾")) &&
           it->text().contains(QStringLiteral("时长"));
  }, 8000));
  CHECK(!member.close_session()); // 无进行中会话再收尾＝本地拒

  // —— 服务器凭据腿（R26-4）：成员越权 403 状态行明示；群主存→列表
  // 掩码「已配置（由 X 更新）」→删→「未配置」（明文只此一次出门，
  // 列表永无凭据内容）——
  member.server_list_widget()->setCurrentRow(0);
  CHECK(member.set_server_credential(QStringLiteral("bob-secret")));
  CHECK(wait_until([&] {
    return member.status_text().contains(QStringLiteral("操作失败")) &&
           member.status_text().contains(QStringLiteral("无权"));
  }, 8000));
  CHECK(member.server_list_widget()->item(0)->text().contains(
      QStringLiteral("凭据 未配置"))); // 越权未改台账
  owner.server_list_widget()->setCurrentRow(0);
  CHECK(owner.set_server_credential(QStringLiteral("owner-secret")));
  CHECK(wait_until([&] {
    return owner.server_count() == 2 &&
           owner.server_list_widget()->item(0)->text().contains(
               QStringLiteral("凭据 已配置"));
  }, 8000));
  owner.server_list_widget()->setCurrentRow(0); // 列表随刷新重建，重选中
  CHECK(owner.delete_server_credential());
  CHECK(wait_until([&] {
    return owner.server_list_widget()->item(0)->text().contains(
        QStringLiteral("凭据 未配置"));
  }, 8000));

  // —— 第二笔（群主发起不收尾）：留痕同屏，最新在前——
  owner.server_list_widget()->setCurrentRow(0);
  CHECK(owner.request_session());
  CHECK(wait_until([&] {
    member.refresh(); // 留痕列表不推送，随刷新拉取
    return member.session_count() == 2 &&
           member.session_list_widget()->item(0)->text().contains(
               QStringLiteral("alice")) &&
           member.session_list_widget()->item(0)->text().contains(
               QStringLiteral("进行中")) &&
           member.session_list_widget()->item(1)->text().contains(
               QStringLiteral("bob")) &&
           member.session_list_widget()->item(1)->text().contains(
               QStringLiteral("已收尾"));
  }, 8000));

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("group server dialog tests: all passed");
    return 0;
  }
  qCritical("group server dialog tests: %d failure(s)", g_failures);
  return 1;
}
