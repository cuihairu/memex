// 二期·群工具三件窗口冒烟：真服务端进程 × 离屏 QDialog。
// CollabEngine 建群（alice 建，拉 bob）→ GroupToolsDialog 连接文件面→
// 投票（建→选项越界 400→投→改票覆盖 counts 变化→成员关票 403→发起人
// 关票→关后投 409）→接龙（建→加入→重复提交 upsert 自己条目→关→关后
// 加入 409）→群任务（幽灵负责人 404→建待认领→认领→认领占位 409→完成
// 留痕 done_by）。判权矩阵细分（非成员 403/选项数 2~10/幂等关票/群主代
// 完成）走 test_files_api 协议腿，不在此重复。
// 注：列表每次成功动作后整表重建，选中不保留——每个动作前重选中；对端
// 数据拉式刷新（不推送），跨端断言前先 refresh。
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QListWidget>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/group_tools_dialog.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::CollabEngine;
using memex::client::GroupToolsDialog;
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

// 选中列表中含指定子串且可选中的行（接龙条目行 NoItemFlags 不可操作）
bool select_row_containing(QListWidget* list, const QString& needle) {
  for (int i = 0; i < list->count(); ++i) {
    auto* item = list->item(i);
    if ((item->flags() & Qt::ItemIsSelectable) &&
        item->text().contains(needle)) {
      list->setCurrentRow(i);
      return true;
    }
  }
  return false;
}

// 行文本安全取（空位返回空串——等待谓词里不解引用空指针）
QString row_text(QListWidget* list, int row) {
  const auto* item = list->item(row);
  return item ? item->text() : QString();
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

  // 建群需特权（权限模型「建群需授权」）：alice 授 group_creator
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
  ce.create_group(QStringLiteral("群工具测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));

  GroupToolsDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("wrong"), gid);
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // 正口令：已连接并自动拉三页空列表
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("pass-1"), gid);
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(dlg.poll_list()->count() == 0);
  CHECK(dlg.chain_list()->count() == 0);
  CHECK(dlg.task_list()->count() == 0);

  // 选项不足 2：本地门拒发（与审批白名单同口径——客户端只挡明显误操作）
  CHECK(!dlg.add_poll(QStringLiteral("午餐"),
                      {QStringLiteral("面馆")}));
  CHECK(dlg.status_text().contains(QStringLiteral("选项至少 2 个")));

  // —— 投票 ——
  // 建投票 200：列表落地（进行中＋选项 0 票）
  CHECK(dlg.add_poll(QStringLiteral("午餐"),
                     {QStringLiteral("面馆"), QStringLiteral("食堂")}));
  CHECK(wait_until([&] {
    return dlg.poll_list()->count() == 1 &&
           row_text(dlg.poll_list(), 0).contains(QStringLiteral("进行中")) &&
           row_text(dlg.poll_list(), 0).contains(QStringLiteral("午餐"));
  }, 8000));
  CHECK(row_text(dlg.poll_list(), 0).contains(QStringLiteral("面馆×0")));

  // 选项越界：服务端 400，状态行明示（客户端不重复选项数裁决）
  CHECK(select_row_containing(dlg.poll_list(), QStringLiteral("午餐")));
  CHECK(dlg.vote_selected(5));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("400")); },
      8000));

  // 投票：counts 变化＋记名台账
  CHECK(select_row_containing(dlg.poll_list(), QStringLiteral("午餐")));
  CHECK(dlg.vote_selected(1));
  CHECK(wait_until([&] {
    const auto text = row_text(dlg.poll_list(), 0);
    return text.contains(QStringLiteral("面馆×1")) &&
           text.contains(QStringLiteral("alice→1"));
  }, 8000));

  // 改票=覆盖：counts 此消彼长，旧选择不留（votes 只剩一条 alice→2）
  CHECK(select_row_containing(dlg.poll_list(), QStringLiteral("午餐")));
  CHECK(dlg.vote_selected(2));
  CHECK(wait_until([&] {
    const auto text = row_text(dlg.poll_list(), 0);
    return text.contains(QStringLiteral("食堂×1")) &&
           !text.contains(QStringLiteral("面馆×1")) &&
           text.contains(QStringLiteral("alice→2")) &&
           !text.contains(QStringLiteral("alice→1"));
  }, 8000));

  // bob 侧：成员可见投票；非发起人关票 403
  GroupToolsDialog dlg_bob;
  dlg_bob.connect_to(QStringLiteral("127.0.0.1"), files_port,
                     QStringLiteral("bob"), QStringLiteral("pass-2"), gid);
  CHECK(wait_until(
      [&] {
        return dlg_bob.is_connected() &&
               dlg_bob.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until(
      [&] { return dlg_bob.poll_list()->count() == 1; }, 8000));
  CHECK(select_row_containing(dlg_bob.poll_list(), QStringLiteral("午餐")));
  CHECK(dlg_bob.close_selected_poll());
  CHECK(wait_until(
      [&] { return dlg_bob.status_text().contains(QStringLiteral("403")); },
      8000));

  // bob 投票 200：两选项各 1 票（alice 改票后投 2）
  CHECK(select_row_containing(dlg_bob.poll_list(), QStringLiteral("午餐")));
  CHECK(dlg_bob.vote_selected(1));
  CHECK(wait_until([&] {
    const auto text = row_text(dlg_bob.poll_list(), 0);
    return text.contains(QStringLiteral("面馆×1")) &&
           text.contains(QStringLiteral("食堂×1")) &&
           text.contains(QStringLiteral("bob→1"));
  }, 8000));

  // 发起人关票 200：行转已截止
  CHECK(select_row_containing(dlg.poll_list(), QStringLiteral("午餐")));
  CHECK(dlg.close_selected_poll());
  CHECK(wait_until([&] {
    return row_text(dlg.poll_list(), 0).contains(QStringLiteral("已截止"));
  }, 8000));

  // 关后投票 409：状态行明示
  CHECK(select_row_containing(dlg_bob.poll_list(), QStringLiteral("午餐")));
  CHECK(dlg_bob.vote_selected(2));
  CHECK(wait_until(
      [&] { return dlg_bob.status_text().contains(QStringLiteral("409")); },
      8000));

  // —— 截止自动关票（惰性判定，服务端权威）：发起区勾截止提交→未到点
  // 可投→到点行渲染已截止→投票拒（客户端渲染不误导＝第一层，服务端
  // 409 权威拒＝第二层兜底；客户端不重复判定故无本地截止门）
  const qint64 dl = QDateTime::currentMSecsSinceEpoch() + 2000;
  CHECK(dlg.add_poll(QStringLiteral("快截票"),
                     {QStringLiteral("甲"), QStringLiteral("乙")}, dl));
  CHECK(wait_until([&] { return dlg.poll_list()->count() == 2; }, 8000));
  CHECK(row_text(dlg.poll_list(), 0).contains(QStringLiteral("进行中")));
  // 未到点可投
  CHECK(select_row_containing(dlg.poll_list(), QStringLiteral("快截票")));
  CHECK(dlg.vote_selected(1));
  CHECK(wait_until([&] {
    const auto text = row_text(dlg.poll_list(), 0);
    return text.contains(QStringLiteral("甲×1")) &&
           text.contains(QStringLiteral("alice→1"));
  }, 8000));
  // 到点：两侧刷新后行转已截止（服务端 status 现算回带）
  CHECK(wait_until(
      [&] { return QDateTime::currentMSecsSinceEpoch() >= dl + 100; }, 8000));
  dlg.refresh();
  CHECK(wait_until([&] {
    return select_row_containing(dlg.poll_list(),
                                 QStringLiteral("快截票")) &&
           row_text(dlg.poll_list(), 0).contains(QStringLiteral("已截止"));
  }, 8000));
  dlg_bob.refresh();
  CHECK(wait_until([&] {
    return select_row_containing(dlg_bob.poll_list(),
                                 QStringLiteral("快截票")) &&
           row_text(dlg_bob.poll_list(), 0).contains(QStringLiteral("已截止"));
  }, 8000));
  // 到点投票：服务端 409 状态行明示（改票同拒路径）
  CHECK(dlg_bob.vote_selected(2));
  CHECK(wait_until(
      [&] { return dlg_bob.status_text().contains(QStringLiteral("409")); },
      8000));

  // —— 匿名＋多选：发起区勾选提交→投位集（1+3=5）→行渲染匿名不展示
  // 投票人＋counts 位展开→改投=整集覆盖
  CHECK(dlg.add_poll(QStringLiteral("匿名多选票"),
                     {QStringLiteral("甲"), QStringLiteral("乙"),
                      QStringLiteral("丙")},
                     0, true, true));
  CHECK(wait_until([&] { return dlg.poll_list()->count() == 3; }, 8000));
  CHECK(row_text(dlg.poll_list(), 0).contains(QStringLiteral("[匿名]")));
  CHECK(row_text(dlg.poll_list(), 0).contains(QStringLiteral("[多选]")));
  dlg_bob.refresh();
  CHECK(wait_until(
      [&] { return dlg_bob.poll_list()->count() == 3; }, 8000));
  CHECK(select_row_containing(dlg_bob.poll_list(),
                              QStringLiteral("匿名多选票")));
  CHECK(dlg_bob.vote_selected(5));  // 位集 5=选 1+3
  CHECK(wait_until([&] {
    const auto text = row_text(dlg_bob.poll_list(), 0);
    return text.contains(QStringLiteral("甲×1")) &&
           text.contains(QStringLiteral("丙×1")) &&
           text.contains(QStringLiteral("匿名投票不展示投票人"));
  }, 8000));
  // 改投=整集覆盖：位集 2（只选乙），甲清零
  CHECK(select_row_containing(dlg_bob.poll_list(),
                              QStringLiteral("匿名多选票")));
  CHECK(dlg_bob.vote_selected(2));
  CHECK(wait_until([&] {
    const auto text = row_text(dlg_bob.poll_list(), 0);
    return text.contains(QStringLiteral("乙×1")) &&
           !text.contains(QStringLiteral("甲×1")) &&
           !text.contains(QStringLiteral("丙×1"));
  }, 8000));

  // —— 接龙 ——
  CHECK(dlg.add_chain(QStringLiteral("周五聚餐"),
                      QStringLiteral("姓名+菜")));
  CHECK(wait_until([&] {
    return dlg.chain_list()->count() == 1 &&
           row_text(dlg.chain_list(), 0).contains(
               QStringLiteral("周五聚餐"));
  }, 8000));

  // bob 加入：条目平铺主题行下（ts ASC）；alice 建的数据对端拉式刷新
  dlg_bob.refresh();
  CHECK(wait_until(
      [&] { return dlg_bob.chain_list()->count() == 1; }, 8000));
  // 格式本地门：hint「姓名+菜」=2 段，单段本地拒（不发网，条目不落地）
  CHECK(select_row_containing(dlg_bob.chain_list(),
                              QStringLiteral("周五聚餐")));
  CHECK(!dlg_bob.join_selected(QStringLiteral("水煮鱼")));
  CHECK(dlg_bob.status_text().contains(QStringLiteral("分 2 段")));
  CHECK(dlg_bob.chain_list()->count() == 1);
  // 合规两段 200
  CHECK(dlg_bob.join_selected(QStringLiteral("本人+水煮鱼")));
  CHECK(wait_until([&] {
    return dlg_bob.chain_list()->count() == 2 &&
           row_text(dlg_bob.chain_list(), 1).contains(
               QStringLiteral("bob：本人+水煮鱼"));
  }, 8000));

  // 重复提交=upsert 自己条目：仍一人一条，内容更新
  CHECK(select_row_containing(dlg_bob.chain_list(),
                              QStringLiteral("周五聚餐")));
  CHECK(dlg_bob.join_selected(QStringLiteral("本人+烤鸭")));
  CHECK(wait_until([&] {
    const auto text = row_text(dlg_bob.chain_list(), 1);
    return dlg_bob.chain_list()->count() == 2 &&
           text.contains(QStringLiteral("bob：本人+烤鸭")) &&
           !text.contains(QStringLiteral("本人+水煮鱼"));
  }, 8000));

  // 发起人关接龙 200；关后加入 409
  CHECK(select_row_containing(dlg.chain_list(), QStringLiteral("周五聚餐")));
  CHECK(dlg.close_selected_chain());
  CHECK(wait_until([&] {
    return row_text(dlg.chain_list(), 0).contains(QStringLiteral("已截止"));
  }, 8000));
  CHECK(select_row_containing(dlg_bob.chain_list(),
                              QStringLiteral("周五聚餐")));
  // 两段合规内容过本地门，服务端关后 409 兜底
  CHECK(dlg_bob.join_selected(QStringLiteral("本人+火锅")));
  CHECK(wait_until(
      [&] { return dlg_bob.status_text().contains(QStringLiteral("409")); },
      8000));

  // —— 群任务 ——
  // 幽灵负责人：服务端 404，状态行明示
  CHECK(dlg.add_task(QStringLiteral("周报汇总"),
                     QStringLiteral("ghost")));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("404")); },
      8000));
  CHECK(dlg.task_list()->count() == 0); // 幽灵拒，未落地

  // 建待认领任务：列表落地（待认领占位）
  CHECK(dlg.add_task(QStringLiteral("周报汇总"), QString()));
  CHECK(wait_until([&] {
    return dlg.task_list()->count() == 1 &&
           row_text(dlg.task_list(), 0).contains(QStringLiteral("待认领"));
  }, 8000));

  // bob 认领：负责人落地（alice 建的数据对端拉式刷新）
  dlg_bob.refresh();
  CHECK(wait_until(
      [&] { return dlg_bob.task_list()->count() == 1; }, 8000));
  CHECK(select_row_containing(dlg_bob.task_list(),
                              QStringLiteral("周报汇总")));
  CHECK(dlg_bob.claim_selected());
  CHECK(wait_until([&] {
    return row_text(dlg_bob.task_list(), 0).contains(
        QStringLiteral("负责人 bob"));
  }, 8000));

  // 认领占位 409：alice 后到被拒
  CHECK(select_row_containing(dlg.task_list(), QStringLiteral("周报汇总")));
  CHECK(dlg.claim_selected());
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("409")); },
      8000));

  // 负责人完成：终态留痕 done_by=bob（行不删）
  CHECK(select_row_containing(dlg_bob.task_list(),
                              QStringLiteral("周报汇总")));
  CHECK(dlg_bob.done_selected());
  CHECK(wait_until([&] {
    const auto text = row_text(dlg_bob.task_list(), 0);
    return text.contains(QStringLiteral("bob 完成")) &&
           text.contains(QStringLiteral("周报汇总"));
  }, 8000));

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_group_tools_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_group_tools_dialog: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
