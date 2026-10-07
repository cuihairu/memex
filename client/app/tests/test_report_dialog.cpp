// 二期·日报周报窗口冒烟：真服务端进程 × 离屏 QDialog。
// owner1 经汇报线为 member1 直属上级（org reporting set CLI 夹具）→
// ReportDialog 连接文件面→member1 写日报→当日重复提交=更新（同日期
// 仍一行、内容覆盖）→owner1 团队聚合可见（下属分组）→「仅本周」过滤
// （当周日期在窗内）→admin1 无下属=空列表（判权服务端裁）。判权矩阵
//（org-admin 不兜底/转岗断权/幽灵 403）走 test_files_api 协议腿，
// 不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/report_dialog.hpp>

using memex::client::ReportDialog;

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
       {std::pair<QString, QString>{QStringLiteral("member1"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("owner1"),
                                    QStringLiteral("pass-2")},
        std::pair<QString, QString>{QStringLiteral("admin1"),
                                    QStringLiteral("pass-3")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }

  // 汇报线夹具：owner1 ← member1（直属上级=看下属日报的唯一判权依据）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("reporting"),
             QStringLiteral("set"), QStringLiteral("member1"),
             QStringLiteral("owner1"), QStringLiteral("--db"),
             db}) == 0);

  const quint16 files_port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(free_port()),
                QStringLiteral("--webhook-port"), QStringLiteral("0"),
                QStringLiteral("--files-port"), QString::number(files_port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, files_port)
               ? (probe.close(), false)
               : true;
  }, 8000));

  ReportDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // 正口令：已连接并自动双拉（空列表）
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(dlg.report_count() == 0);

  // 空内容本地拒（不发网）
  CHECK(!dlg.write_report(QStringLiteral("2026-10-08"), QString()));

  // 写日报（三段自由文本）→列表一行；点行回填编辑器
  CHECK(dlg.write_report(QStringLiteral("2026-10-08"),
                         QStringLiteral("今日完成：日报链路\n明日计划：\nblockers：无")));
  CHECK(wait_until([&] { return dlg.report_count() == 1; }, 8000));
  CHECK(dlg.list()->item(0)->text().startsWith(QStringLiteral("2026-10-08")));
  // 点行回填编辑器（itemClicked=用户交互信号，程序化发同款）
  dlg.list()->setCurrentRow(0);
  emit dlg.list()->itemClicked(dlg.list()->item(0));
  QApplication::processEvents();
  CHECK(dlg.content_edit()->toPlainText().contains(QStringLiteral("日报链路")));

  // 当日重复提交=更新：同日期仍一行、首行内容换新
  CHECK(dlg.write_report(QStringLiteral("2026-10-08"),
                         QStringLiteral("今日完成：日报链路 v2")));
  CHECK(wait_until([&] {
    return dlg.report_count() == 1 &&
           dlg.list()->item(0)->text().contains(QStringLiteral("v2"));
  }, 8000));

  // 再写一篇过期日报（上周）：我的列表两篇
  CHECK(dlg.write_report(QStringLiteral("2026-09-01"),
                         QStringLiteral("今日完成：历史日报")));
  CHECK(wait_until([&] { return dlg.report_count() == 2; }, 8000));

  // owner1（直属上级）：团队聚合见 member1 分组；「仅本周」默认开——
  // 过期日报被滤、当周日报可见（周报=按周过滤的聚合视图）
  ReportDialog dlg_owner;
  dlg_owner.connect_to(QStringLiteral("127.0.0.1"), files_port,
                       QStringLiteral("owner1"), QStringLiteral("pass-2"));
  CHECK(wait_until(
      [&] {
        return dlg_owner.is_connected() &&
               dlg_owner.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until(
      [&] { return dlg_owner.team_count() == 1; }, 8000));
  CHECK(dlg_owner.team_list()->item(0)->text().contains(
      QStringLiteral("member1")));
  CHECK(dlg_owner.team_list()->item(0)->text().contains(QStringLiteral("v2")));
  CHECK(dlg_owner.report_count() == 0); // owner1 自己没写

  // 关「仅本周」（周报视图→日报全量）：过期日报也见（开关语义双向可走）
  dlg_owner.toggle_team_week();
  CHECK(wait_until([&] { return dlg_owner.team_count() == 2; }, 8000));
  int hist_row = -1;
  for (int i = 0; i < dlg_owner.team_list()->count(); ++i) {
    if (dlg_owner.team_list()->item(i)->text().contains(
            QStringLiteral("2026-09-01"))) {
      hist_row = i;
    }
  }
  CHECK(hist_row >= 0);

  // admin1（无下属；即便有角色也不兜底看别人）：团队聚合=空
  ReportDialog dlg_admin;
  dlg_admin.connect_to(QStringLiteral("127.0.0.1"), files_port,
                       QStringLiteral("admin1"), QStringLiteral("pass-3"));
  CHECK(wait_until(
      [&] {
        return dlg_admin.is_connected() &&
               dlg_admin.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until([&] { return dlg_admin.team_count() == 0; }, 8000));

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_report_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_report_dialog: " << g_failures << " check(s) FAILED\n";
  return 1;
}
