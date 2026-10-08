// 二期·审批窗口冒烟：真服务端进程 × 离屏 QDialog。
// owner1 授 org-admin（member1 无直属上级→org-admin 兜底=可决夹具）→
// ApprovalDialog 连接文件面→member1 发起年假→member1 决自己的申请本地拒
//（待我决行才可决）→owner1 待决可见并批准（带批注）→member1 刷新见终态
//（已批准·审批人 owner1）→已决行撤回本地拒→member1 新发调休再撤回
//（申请人专属且仅 pending）→白名单外类型服务端 400 状态行明示。
// 判权矩阵（自审 403/无权 403/已决 409/直属上级优先）走 test_files_api
// 协议腿，不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/approval_dialog.hpp>

using memex::client::ApprovalDialog;

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
    if (timer.elapsed() > timeout_ms) {
      qDebug("wait_until timeout after %lldms (limit %dms)", timer.elapsed(), timeout_ms);
      return false;
    }
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return cond();
}

quint16 free_port() {
  for (int attempt = 0; attempt < 5; ++attempt) {
    QTcpServer probe;
    if (probe.listen(QHostAddress::LocalHost, 0)) {
      const quint16 port = probe.serverPort();
      probe.close();
      return port;
    }
    QThread::msleep(50);
  }
  // 极端竞态：返回 0 让上层报错
  return 0;
}

// 选中列表中含指定子串的行（找到并选中返回 true）
bool select_row_containing(ApprovalDialog& dlg, const QString& needle) {
  for (int i = 0; i < dlg.list()->count(); ++i) {
    if (dlg.list()->item(i)->text().contains(needle)) {
      dlg.list()->setCurrentRow(i);
      return true;
    }
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("member1"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("owner1"),
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }

  // owner1 授 org-admin：member1 未设直属上级（汇报线空）→org-admin 兜底
  // 可决（approval-decide 规则的无上级腿）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("owner1"),
             QStringLiteral("org-admin"), QStringLiteral("--by"),
             QStringLiteral("owner1"), QStringLiteral("--db"),
             db}) == 0);

  // 双端口显式 free_port() + 重试整个 server 启动（最多 3 次），
  // 吸收 CI 共享 runner 竞态；与 test_group_tools_dialog 同构但带重试。
  quint16 collab_port = 0;
  quint16 files_port = 0;
  QProcess server;
  bool server_ok = false;
  for (int attempt = 0; attempt < 3 && !server_ok; ++attempt) {
    collab_port = free_port();
    files_port = free_port();
    if (collab_port == 0 || files_port == 0) {
      QThread::msleep(100);
      continue;
    }
    server.setProcessChannelMode(QProcess::ForwardedChannels);
    server.start(server_bin,
                 {QStringLiteral("serve"), QStringLiteral("--db"), db,
                  QStringLiteral("--port"), QString::number(collab_port),
                  QStringLiteral("--webhook-port"), QStringLiteral("0"),
                  QStringLiteral("--files-port"), QString::number(files_port)});
    if (!server.waitForStarted(5000)) {
      qCritical("FAIL server 启动超时（尝试 %d/3）", attempt + 1);
      continue;
    }
    // 探活 files_port
    if (wait_until([&] {
          QTcpServer probe;
          return probe.listen(QHostAddress::LocalHost, files_port)
                     ? (probe.close(), false)
                     : true;
        }, 15000)) {
      server_ok = true;
      break;
    }
    qCritical("FAIL files_port %u 探活超时（尝试 %d/3），重试", files_port, attempt + 1);
    server.kill();
    server.waitForFinished(3000);
  }
  if (!server_ok) {
    qCritical("FAIL server 启动重试耗尽");
    return 1;
  }

  ApprovalDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      15000));
  CHECK(!dlg.is_connected());

  // 正口令：已连接并自动拉空列表
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      15000));
  CHECK(dlg.list()->count() == 0);

  // 发起年假（类型下拉默认第一项；起止＋事由）
  CHECK(dlg.add_approval(QStringLiteral("年假"),
                         QStringLiteral("2026-10-12"),
                         QStringLiteral("2026-10-13"),
                         QStringLiteral("家里有事")));
  CHECK(wait_until([&] { return dlg.list()->count() == 1; }, 15000));
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("待决")));
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("年假")));
  CHECK(dlg.selected_kind() == QString()); // 未选中行无种类

  // 决自己的申请：本地拒（只有「待我决」行可决；自审不成立在服务端也拒）
  dlg.list()->setCurrentRow(0);
  CHECK(dlg.selected_kind() == QStringLiteral("mine"));
  const int before_net = dlg.list()->count();
  CHECK(!dlg.decide_selected(true, QStringLiteral("自批")));
  CHECK(dlg.status_text().contains(QStringLiteral("待我决")));
  CHECK(dlg.list()->count() == before_net);

  // 白名单外类型：服务端 400，状态行明示（客户端不重复白名单裁决）
  CHECK(dlg.add_approval(QStringLiteral("探亲"), QString(), QString(),
                         QString()));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("操作失败")); },
      15000));
  dlg.refresh();
  CHECK(wait_until([&] { return dlg.list()->count() == 1; }, 15000));

  // owner1 侧：待决可见（member1 无直属上级→org-admin 兜底判权通过）
  ApprovalDialog dlg_owner;
  dlg_owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                       QStringLiteral("owner1"), QStringLiteral("pass-2"));
  CHECK(wait_until(
      [&] {
        return dlg_owner.is_connected() &&
               dlg_owner.status_text().contains(QStringLiteral("已连接"));
      },
      15000));
  CHECK(wait_until([&] { return dlg_owner.list()->count() == 1; }, 15000));
  CHECK(dlg_owner.list()->item(0)->text().contains(
      QStringLiteral("member1")));
  CHECK(dlg_owner.list()->item(0)->text().contains(QStringLiteral("年假")));
  CHECK(dlg_owner.selected_kind() == QString());

  // owner1 批准（带批注）：待决清空
  dlg_owner.list()->setCurrentRow(0);
  CHECK(dlg_owner.selected_kind() == QStringLiteral("pending"));
  CHECK(dlg_owner.decide_selected(true, QStringLiteral("同意")));
  CHECK(wait_until([&] { return dlg_owner.list()->count() == 0; }, 15000));

  // member1 刷新：终态「已批准·审批人 owner1」；已决行撤回本地拒
  dlg.refresh();
  CHECK(wait_until([&] {
    return select_row_containing(dlg, QStringLiteral("已批准")) &&
           dlg.status_text().contains(QStringLiteral("待我决 0 项"));
  }, 15000));
  CHECK(dlg.list()->currentItem()->text().contains(
      QStringLiteral("同意")));
  CHECK(dlg.list()->currentItem()->text().contains(
      QStringLiteral("审批人 owner1")));
  CHECK(!dlg.withdraw_selected()); // 非 pending 撤回拒（客户端门＋服务端 409）
  CHECK(dlg.status_text().contains(QStringLiteral("待决的申请")));

  // 新发调休再撤回：申请人专属且仅 pending（终态行不动）
  CHECK(dlg.add_approval(QStringLiteral("调休"), QString(), QString(),
                         QString()));
  CHECK(wait_until([&] { return dlg.list()->count() == 2; }, 15000));
  CHECK(select_row_containing(dlg, QStringLiteral("调休")));
  CHECK(dlg.selected_kind() == QStringLiteral("mine"));
  CHECK(dlg.withdraw_selected());
  CHECK(wait_until([&] {
    return select_row_containing(dlg, QStringLiteral("已撤回")) &&
           dlg.list()->count() == 2; // 撤回不删行——终态留痕在列表
  }, 15000));
  // 年假终态行仍在（全程留痕不删改）
  CHECK(select_row_containing(dlg, QStringLiteral("已批准")));

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_approval_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_approval_dialog: " << g_failures << " check(s) FAILED\n";
  return 1;
}
