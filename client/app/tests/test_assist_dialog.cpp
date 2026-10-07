// 二期·远程协助窗口冒烟：真服务端进程 × 离屏 QDialog 双实例（发起方
// owner1 × 受控方 member1）。CLI 开部门放行（默认禁白名单口径）→发起
// →受控方批准缩权（只 view+mouse，consent 可缩不可扩）→启动→受控方
// 合成帧推送（离屏绕真抓屏）→发起方 500ms 轮询拉帧渲染→程序化点画面
// （真 eventFilter 路径）发输入→受控方轮询取走→受控方结束=撤权→台账
// 终态＋审计链四迁移。判权协议腿（401/默认禁 403/consent 403/实批超集
// 409/权限位门 403/终态即断）走 test_files_api 协议段，不在此重复。
#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/assist_dialog.hpp>

using memex::client::AssistDialog;

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

// 合成帧：纯色 JPEG → b64（离屏环境绕 QScreen 抓屏）
QString make_frame_b64() {
  QImage img(64, 48, QImage::Format_RGB32);
  img.fill(QColor(30, 90, 160));
  QByteArray bytes;
  QBuffer buf(&bytes);
  buf.open(QIODevice::WriteOnly);
  img.save(&buf, "JPEG");
  return QString::fromUtf8(bytes.toBase64());
}

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("owner1"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("member1"),
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }
  // 部门放行开关开全局行（默认禁——白名单口径；协议段验默认拒腿）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("assist"), QStringLiteral("policy"),
             QStringLiteral("set"), QStringLiteral("on"),
             QStringLiteral("--by"), QStringLiteral("owner1"),
             QStringLiteral("--db"), db}) == 0);

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

  // 双实例：发起方（owner1）× 受控方（member1）
  AssistDialog helper;
  AssistDialog subject;
  CHECK(!helper.is_connected());

  // 错口令：状态行明示
  helper.connect_to(QStringLiteral("127.0.0.1"), files_port,
                    QStringLiteral("owner1"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return helper.status_text().contains(QStringLiteral("连接失败")); },
      8000));

  helper.connect_to(QStringLiteral("127.0.0.1"), files_port,
                    QStringLiteral("owner1"), QStringLiteral("pass-1"));
  subject.connect_to(QStringLiteral("127.0.0.1"), files_port,
                     QStringLiteral("member1"), QStringLiteral("pass-2"));
  CHECK(wait_until(
      [&] {
        return helper.is_connected() &&
               helper.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until(
      [&] {
        return subject.is_connected() &&
               subject.status_text().contains(QStringLiteral("已连接"));
      },
      8000));

  // 发起（显式三权限——勾选框默认只勾查看，程序化入口直传同款请求体）
  helper.request_assist(QStringLiteral("member1"),
                        {QStringLiteral("view"), QStringLiteral("mouse"),
                         QStringLiteral("keyboard")});
  CHECK(wait_until(
      [&] {
        return helper.status_text().contains(QStringLiteral("已发起")) &&
               helper.session_count() == 1;
      },
      8000));
  // 受控方待批行（consent 只属受控方）
  CHECK(wait_until([&] { return subject.pending_count() == 1; }, 8000));

  // 批准缩权：只给 view+mouse（实批 ⊆ 申请，keyboard 不给）
  subject.approve_pending(true,
                          {QStringLiteral("view"), QStringLiteral("mouse")});
  CHECK(wait_until(
      [&] {
        return helper.session_text(0).contains(QStringLiteral("已批待启动"));
      },
      8000));

  // 启动（发起方；选中行→启动）
  helper.select_session(0);
  helper.start_selected();
  CHECK(wait_until(
      [&] { return helper.session_text(0).contains(QStringLiteral("进行中")); },
      8000));

  // 受控方开始共享（轮询起效后共享指示持续可见）＋推合成帧
  CHECK(wait_until(
      [&] {
        if (!subject.share_text().contains(QStringLiteral("屏幕共享中"))) {
          subject.start_sharing();
          return false;
        }
        return true;
      },
      8000));
  subject.push_frame_once(make_frame_b64());
  // 发起方 500ms 轮询拉帧渲染
  CHECK(wait_until([&] { return helper.frame_seq_seen() >= 1; }, 8000));

  // 程序化点画面（真 eventFilter 路径）→受控方轮询取走输入
  helper.click_frame(0.5, 0.25);
  CHECK(wait_until(
      [&] { return subject.input_log_count() == 1; }, 8000));
  CHECK(subject.status_text().contains(QStringLiteral("已发点击")) ||
        helper.status_text().contains(QStringLiteral("已发点击")));

  // 受控方结束=撤权即时生效：发起方台账见终态
  subject.stop_sharing();
  CHECK(wait_until(
      [&] { return helper.session_text(0).contains(QStringLiteral("已结束")); },
      8000));

  // 审计链（当事方可读）：request/approve/start/end 四迁移。台账列表
  // 每 500ms 重建会清选中态——选中与拉取须同拍完成
  CHECK(wait_until(
      [&] {
        helper.select_session(0);
        helper.show_audit();
        return helper.audit_count() >= 4;
      },
      8000));

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_assist_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_assist_dialog: " << g_failures << " check(s) FAILED\n";
  return 1;
}
