// 二期·办公室位置图窗口冒烟：真服务端进程 × 离屏 QDialog。
// member1 无工位=空图且编辑 403 状态行明示→owner1（CLI 授 org-admin）
// 建座视图随编辑走（3F→2F→切回 3F）→程序化点选＋拖拽落位保存回查
// （点 0.2,0.8 命中=坐标已持久）→绑定 member1 后其自楼层互见（工位即
// 楼层归属）→解绑复原空图。换座 409／越界夹回／幽灵绑定等判权与协议
// 腿走 test_files_api 位置图段，不在此重复。
#include <QApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/office_map_dialog.hpp>

using memex::client::OfficeMapDialog;

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
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }
  // owner1 授 org-admin（工位编辑判权在服务端；客户端只收 can_manage）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("owner1"),
             QStringLiteral("org-admin"), QStringLiteral("--by"),
             QStringLiteral("owner1"), QStringLiteral("--db"), db}) == 0);

  // 双端口 free_port() + 重试整个 server 启动（最多 3 次，与
  // test_approval_dialog 同构）：吸收 CI 共享 runner 竞态——free_port
  // 探活与 serve bind 之间端口可被抢占，文件面 bind 失败即全腿级联
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
                  QStringLiteral("--files-port"),
                  QString::number(files_port)});
    if (!server.waitForStarted(5000)) {
      qCritical("FAIL server 启动超时（尝试 %d/3）", attempt + 1);
      continue;
    }
    if (wait_until([&] {
          QTcpServer probe;
          return probe.listen(QHostAddress::LocalHost, files_port)
                     ? (probe.close(), false)
                     : true;
        }, 15000)) {
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

  OfficeMapDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态行明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // —— member1：无工位=空图（floor 空）＋编辑面 403 ——
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("member1"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until(
      [&] { return dlg.floor_text().contains(QStringLiteral("未分配工位")); },
      8000));
  CHECK(dlg.seat_count() == 0);
  CHECK(!dlg.can_manage());
  // 无权建座：服务端 403 状态行明示（客户端门面不拦，判权在服务端）
  CHECK(dlg.add_seat(QStringLiteral("3F"), QStringLiteral("A-01")));
  CHECK(wait_until(
      [&] {
        return dlg.status_text().contains(QStringLiteral("操作失败")) &&
               dlg.status_text().contains(QStringLiteral("org-admin"));
      },
      8000));
  // 无权拖拽：本地即拒（can_manage 门与鼠标路径同源，不发网）
  CHECK(!dlg.move_selected(0.2, 0.8));

  // —— owner1（org-admin）：建座、视图随编辑走、跨层查看 ——
  OfficeMapDialog dlg_admin;
  dlg_admin.connect_to(QStringLiteral("127.0.0.1"), files_port,
                       QStringLiteral("owner1"), QStringLiteral("pass-2"));
  CHECK(wait_until(
      [&] {
        return dlg_admin.is_connected() &&
               dlg_admin.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until([&] { return dlg_admin.can_manage(); }, 8000));

  CHECK(dlg_admin.add_seat(QStringLiteral("3F"), QStringLiteral("A-01")));
  CHECK(wait_until(
      [&] {
        return dlg_admin.floor_text().contains(QStringLiteral("3F")) &&
               dlg_admin.seat_count() == 1;
      },
      8000));
  CHECK(dlg_admin.add_seat(QStringLiteral("3F"), QStringLiteral("A-02")));
  CHECK(wait_until([&] { return dlg_admin.seat_count() == 2; }, 8000));
  CHECK(dlg_admin.add_seat(QStringLiteral("2F"), QStringLiteral("B-01")));
  CHECK(wait_until(
      [&] {
        return dlg_admin.floor_text().contains(QStringLiteral("2F")) &&
               dlg_admin.seat_count() == 1;
      },
      8000));
  // 跨层查看（org-admin ?floor=）
  dlg_admin.refresh(QStringLiteral("3F"));
  CHECK(wait_until(
      [&] {
        return dlg_admin.floor_text().contains(QStringLiteral("3F")) &&
               dlg_admin.seat_count() == 2;
      },
      8000));

  // 搜索过滤：命中亮其余淡（visible_count 与画笔同一谓词）
  dlg_admin.set_search(QStringLiteral("A-02"));
  CHECK(dlg_admin.visible_count() == 1);
  dlg_admin.set_search(QString());
  CHECK(dlg_admin.visible_count() == 2);

  // 拖拽落位：点选（0.5,0.5=A-01 起位）→移到 0.2,0.8→松手即保存→
  // 回执重拉后点新坐标命中=落位已持久
  CHECK(dlg_admin.select_at(0.5, 0.5));
  CHECK(dlg_admin.move_selected(0.2, 0.8));
  CHECK(wait_until(
      [&] {
        return dlg_admin.status_text().contains(QStringLiteral("工位已保存"));
      },
      8000));
  CHECK(wait_until(
      [&] {
        dlg_admin.select_at(0.2, 0.8);
        return dlg_admin.selected_id() >= 0;
      },
      8000));

  // 绑定 member1 占 A-01（选中态即上面点中的 0.2,0.8 座）
  CHECK(dlg_admin.bind_selected(QStringLiteral("member1")));
  CHECK(wait_until(
      [&] {
        return dlg_admin.status_text().contains(QStringLiteral("占用已更新"));
      },
      8000));

  // —— 工位即楼层归属：member1 被绑定后自楼层互见（3F 两座全见）——
  dlg.refresh();
  CHECK(wait_until(
      [&] {
        return dlg.floor_text().contains(QStringLiteral("3F")) &&
               dlg.seat_count() == 2;
      },
      8000));

  // 解绑复原：member1 回到无工位空图（绑定回执的 refresh 清了选中态，
  // 重选 A-01 再解绑）
  CHECK(dlg_admin.select_at(0.2, 0.8));
  CHECK(dlg_admin.bind_selected(QString()));
  CHECK(wait_until(
      [&] {
        return dlg_admin.status_text().contains(QStringLiteral("占用已更新"));
      },
      8000));
  CHECK(wait_until(
      [&] {
        dlg.refresh();
        return dlg.floor_text().contains(QStringLiteral("未分配工位"));
      },
      8000));

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_office_map_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_office_map_dialog: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
