// R25-2 群 CI/CD 窗口冒烟：真服务端进程 × 离屏 QDialog。
// 管理员建流水线（灰→触发后翻绿/翻红）→开工具白名单（成员触发前 403
// 状态行明示）→触发 stub 即时终态→run 历史谁触发可回溯→成员腿入群即
// 授权（白名单开放后成员可触发可读）。结果卡片回群与权限矩阵（非成员
// 403/幽灵流水线 404/审计管理面）走 test_files_api 服务端腿，不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

#include <app/group_ci_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::GroupCiDialog;
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
                QStringLiteral("--files-port"), QString::number(files_port),
                // R25-4 凭据面：配主密钥（缺省空=凭据路由 503）
                QStringLiteral("--tool-cred-secret"),
                QStringLiteral("test-secret")});
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
  ce.create_group(QStringLiteral("流水线测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));

  // —— 管理员腿：建流水线→开白名单→触发翻绿/翻红→历史可回溯 ——
  GroupCiDialog owner;
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
  owner.set_group(gid, QStringLiteral("流水线测试群"));
  CHECK(owner.windowTitle().contains(QStringLiteral("流水线测试群")));
  CHECK(wait_until([&] { return owner.pipeline_count() == 1; }, 8000));
  CHECK(owner.pipeline_list()->item(0)->text().contains(
      QStringLiteral("暂无流水线")));

  // 建流水线：灰灯（未跑过）；空名程序化入口拒
  CHECK(!owner.submit_pipeline(QString(), QString()));
  CHECK(owner.submit_pipeline(QStringLiteral("dev"), QStringLiteral("主构建")));
  CHECK(wait_until([&] {
    return owner.pipeline_count() == 1 &&
           owner.pipeline_list()->item(0) != nullptr &&
           owner.pipeline_list()->item(0)->text().contains(
               QStringLiteral("dev")) &&
           owner.pipeline_list()->item(0)->text().contains(
               QStringLiteral("未跑过"));
  }, 8000));

  // 白名单未开：触发 403（状态行明示「触发未开放」）
  owner.pipeline_list()->setCurrentRow(0);
  CHECK(owner.trigger_selected());
  CHECK(wait_until([&] {
    return owner.status_text().contains(QStringLiteral("操作失败")) &&
           owner.status_text().contains(QStringLiteral("触发未开放"));
  }, 8000));

  // 开白名单（R25-1 框架面：tool=ci actions=[trigger]）→触发成功翻绿
  CHECK(owner.open_trigger_whitelist());
  CHECK(wait_until([&] {
    return owner.status_text().contains(
        QStringLiteral("已开放成员触发"));
  }, 8000));
  CHECK(owner.trigger_selected());
  // 红绿灯翻绿＋run 历史落账（谁触发可回溯：actor=alice）——只等持久
  // 面（列表/历史条目文），不等状态行：「已触发」提示会被刷新回包的
  //「共 N 条流水线」覆盖（CI 慢回包下首拍即被冲掉＝5630f0b 的假红教训）
  CHECK(wait_until([&] {
    return owner.pipeline_list()->item(0) != nullptr &&
           owner.pipeline_list()->item(0)->text().contains(
               QStringLiteral("成功")) &&
           owner.runs_count() == 1 &&
           owner.runs_list()->item(0)->text().contains(
               QStringLiteral("alice"));
  }, 8000));

  // 演练失败（params {"fail":true}）→翻红；历史倒序（失败在前）
  owner.pipeline_list()->setCurrentRow(0);
  for (QLineEdit* e : owner.findChildren<QLineEdit*>()) {
    if (e->placeholderText().contains(QStringLiteral("参数"))) {
      e->setText(QStringLiteral("{\"fail\":true}"));
    }
  }
  CHECK(owner.trigger_selected());
  CHECK(wait_until([&] {
    return owner.pipeline_list()->item(0) != nullptr &&
           owner.runs_count() == 2 &&
           owner.runs_list()->item(0)->text().contains(
               QStringLiteral("失败")) &&
           owner.runs_list()->item(1)->text().contains(
               QStringLiteral("成功"));
  }, 8000));

  // —— R25-4 凭据腿（alice=群主）：设置→掩码状态「已配置」→删除→「未配置」。
  //     只等持久面（cred_state_ 标签），不等状态行（会被刷新回包覆盖）——
  CHECK(owner.credential_state_text().contains(QStringLiteral("未配置")));
  CHECK(owner.set_credential(QStringLiteral("sk-owner-secret")));
  CHECK(wait_until([&] {
    return owner.credential_state_text().contains(QStringLiteral("已配置")) &&
           owner.credential_state_text().contains(QStringLiteral("alice"));
  }, 8000));
  // 掩码面：状态文本永不含凭据值
  CHECK(!owner.credential_state_text().contains(QStringLiteral("sk-owner-secret")));
  CHECK(owner.delete_credential());
  CHECK(wait_until([&] {
    return owner.credential_state_text().contains(QStringLiteral("未配置"));
  }, 8000));

  // —— 成员腿：白名单开放后成员可读可触发（入群即授权）——
  GroupCiDialog member;
  member.connect_to(QStringLiteral("127.0.0.1"), files_port,
                    QStringLiteral("bob"), QStringLiteral("pass-2"));
  CHECK(wait_until([&] { return member.is_connected(); }, 8000));
  member.set_group(gid, QStringLiteral("流水线测试群"));
  CHECK(member.windowTitle().contains(QStringLiteral("流水线测试群")));
  CHECK(wait_until([&] { return member.pipeline_count() == 1; }, 8000));
  CHECK(member.pipeline_list()->item(0)->text().contains(
      QStringLiteral("失败"))); // 红灯对成员同样可见
  member.pipeline_list()->setCurrentRow(0);
  CHECK(member.trigger_selected());
  CHECK(wait_until([&] {
    return member.runs_count() == 3 &&
           member.runs_list()->item(0)->text().contains(
               QStringLiteral("bob")) &&
           member.runs_list()->item(0)->text().contains(
               QStringLiteral("成功"));
  }, 8000));

  server.terminate();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("group ci dialog tests: all passed");
    return 0;
  }
  qCritical("group ci dialog tests: %d failure(s)", g_failures);
  return 1;
}
