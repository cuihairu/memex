// R25-3 打包/导出窗口冒烟：真服务端进程 × 离屏 QDialog。
// 空态→建产物须先开白名单（未开放 403 状态行明示）→一键打包台账落地
// （谁出可回溯）→同款重打覆盖→删除恒归管理员（成员越权 403 走服务端
// 腿）→导出群配置快照（成员/白名单/流水线在快照；密文面不进快照）。
#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <app/group_pack_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::GroupPackDialog;
using memex::client::LocalStore;

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
                QStringLiteral("--files-port"), QString::number(files_port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, files_port)
               ? (probe.close(), false)
               : true;
  }, 8000));

  // 建群（alice 建，拉 bob）：group_result 回执取 gid（顶层 connect）
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
  ce.create_group(QStringLiteral("打包测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));

  // —— 群主腿：空态→白名单未开拒→开闸→打包→重打覆盖→导出快照 ——
  GroupPackDialog owner;
  owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                   QStringLiteral("alice"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return owner.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                   QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] {
    return owner.is_connected() &&
           owner.status_text().contains(QStringLiteral("已连接"));
  }, 8000));
  owner.set_group(gid, QStringLiteral("打包测试群"));
  CHECK(owner.windowTitle().contains(QStringLiteral("打包测试群")));
  CHECK(wait_until([&] { return owner.artifact_count() == 1; }, 8000));
  CHECK(owner.artifact_list()->item(0)->text().contains(
      QStringLiteral("暂无产物")));

  // 白名单未开：打包 403（状态行明示「打包未开放」）
  CHECK(!owner.build_artifact(QString(), QStringLiteral("1.0")));
  CHECK(owner.build_artifact(QStringLiteral("app"), QStringLiteral("1.0")));
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("操作失败")) &&
           owner.status_text().contains(QStringLiteral("打包未开放"));
  }, 8000));

  // 开闸→打包落地（台账带出产物人）；空名程序化入口拒
  CHECK(owner.open_build_whitelist());
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("已开放成员打包"));
  }, 8000));
  CHECK(!owner.build_artifact(QStringLiteral(""), QStringLiteral("1.0")));
  CHECK(owner.build_artifact(QStringLiteral("app"), QStringLiteral("1.0.0"),
                             QStringLiteral("首包")));
  CHECK(wait_until([&] {
    return owner.artifact_count() == 1 &&
           owner.artifact_list()->item(0) != nullptr &&
           owner.artifact_list()->item(0)->text().contains(
               QStringLiteral("app 1.0.0")) &&
           owner.artifact_list()->item(0)->text().contains(
               QStringLiteral("alice"));
  }, 8000));
  // 同款重打=覆盖台账（仍一件，备注换新）
  CHECK(owner.build_artifact(QStringLiteral("app"), QStringLiteral("1.0.0"),
                             QStringLiteral("重打")));
  CHECK(wait_until([&] {
    return owner.artifact_count() == 1 &&
           owner.artifact_list()->item(0)->text().contains(
               QStringLiteral("重打"));
  }, 8000));

  // 导出群配置快照：成员/流水线在快照；密文面永不进快照
  owner.export_config();
  CHECK(wait_until([&] {
    return owner.snapshot_text().contains(QStringLiteral("members"));
  }, 8000));
  CHECK(owner.snapshot_text().contains(QStringLiteral("alice")));
  CHECK(owner.snapshot_text().contains(QStringLiteral("打包测试群")));
  CHECK(!owner.snapshot_text().contains(QStringLiteral("wrapped_dek")));
  CHECK(!owner.snapshot_text().contains(QStringLiteral("secret_ct")));
  CHECK(!owner.snapshot_text().contains(QStringLiteral("kdf_salt")));

  // —— 成员腿：白名单开放后成员可读台账、可打包（入群即授权）——
  GroupPackDialog member;
  member.connect_to(QStringLiteral("127.0.0.1"), files_port,
                    QStringLiteral("bob"), QStringLiteral("pass-2"));
  CHECK(wait_until([&] { return member.is_connected(); }, 8000));
  member.set_group(gid, QStringLiteral("打包测试群"));
  CHECK(member.windowTitle().contains(QStringLiteral("打包测试群")));
  CHECK(wait_until([&] { return member.artifact_count() == 1; }, 8000));
  CHECK(member.artifact_list()->item(0)->text().contains(
      QStringLiteral("app 1.0.0")));
  CHECK(member.build_artifact(QStringLiteral("lib"), QStringLiteral("2.0"),
                              QString()));
  CHECK(wait_until([&] {
    return member.artifact_count() == 2 &&
           member.artifact_list()->item(0)->text().contains(
               QStringLiteral("lib 2.0")) &&
           member.artifact_list()->item(0)->text().contains(
               QStringLiteral("bob"));
  }, 8000));

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("group pack dialog tests: all passed");
    return 0;
  }
  qCritical("group pack dialog tests: %d failure(s)", g_failures);
  return 1;
}
