// 管理台·归档检索台冒烟（原型 desktop-search）：真服务端进程 × 离屏
// ArchiveConsole。member1→owner1 造三条归档（中间一条撤回）→无角色检索
// 403 状态行明示→授 owner1 auditor→关键词检索命中两行（已撤回标记胶囊＋
// 撤回事件钮）→查看上下文回会话窗口（前后行在、命中行加粗）→撤回事件
// 回撤回人 member1→导出留证落证据文件（头注/行/计数）→查阅日志台账见
// search/context/recall/export 四类留痕（被拒留痕腿走 test_audit_dialog）。
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

#include <app/archive_console.hpp>
#include <app/theme.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

using memex::client::ArchiveConsole;
using memex::client::CollabEngine;
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

}  // namespace

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

  // 双端口 free_port() + 重试整个 server 启动（与 test_audit_dialog 同构）：
  // 吸收 CI 共享 runner 的探活/bind 竞态
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
    if (!server.waitForStarted(5000)) continue;
    if (wait_until([&] {
          QTcpServer probe;
          return probe.listen(QHostAddress::LocalHost, files_port)
                     ? (probe.close(), false)
                     : true;
        }, 15000)) {
      server_ok = true;
      break;
    }
    server.kill();
    server.waitForFinished(3000);
  }
  if (!server_ok) {
    qCritical("FAIL server 启动重试耗尽");
    return 1;
  }

  // 归档夹具：member1→owner1 三条文本（中间一条撤回；前后行供上下文窗口）
  LocalStore store_ce;
  CHECK(store_ce.open(tmp.filePath(QStringLiteral("ce.db"))));
  CollabEngine ce;
  ce.attach_store(&store_ce);
  bool ce_in = false;
  quint64 ack_a = 0, ack_b = 0, ack_c = 0;
  bool ok_a = false, ok_b = false, ok_c = false;
  QObject::connect(&ce, &CollabEngine::logged_in, &ce,
                   [&](const QString&, const QString&) { ce_in = true; });
  QObject::connect(&ce, &CollabEngine::text_delivered, &ce,
                   [&](quint64 seq, bool ok) {
                     if (seq == ack_a) ok_a = ok;
                     if (seq == ack_b) ok_b = ok;
                     if (seq == ack_c) ok_c = ok;
                   });
  ce.login(QStringLiteral("127.0.0.1"), collab_port, QStringLiteral("member1"),
           QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return ce_in; }, 8000));
  ack_a = ce.send_text(QStringLiteral("owner1"),
                       QStringLiteral("第一条常规消息 暗号Q7 开头"));
  ack_b = ce.send_text(QStringLiteral("owner1"),
                       QStringLiteral("第二条要撤回的 暗号Q7 消息"));
  ack_c = ce.send_text(QStringLiteral("owner1"),
                       QStringLiteral("第三条收尾常规消息"));
  CHECK(ack_a != 0 && ack_b != 0 && ack_c != 0);
  CHECK(wait_until([&] { return ok_a && ok_b && ok_c; }, 8000));
  // 撤回中间一条（发送方本人；服务端置标记留原文）
  ce.recall_text(QStringLiteral("owner1"),
                 CollabEngine::msg_id_for(QStringLiteral("member1"), ack_b));
  QThread::msleep(300);  // 撤回投影同事务落库，稍候即成

  // 无角色腿：member1 检索被 403 拒（状态行明示；留痕腿在 test_audit_dialog）
  ArchiveConsole denied;
  denied.connect_to(QStringLiteral("127.0.0.1"), files_port,
                    QStringLiteral("member1"), QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return denied.is_connected(); }, 8000));
  CHECK(denied.run_search(QString(), QStringLiteral("暗号Q7"), 0, 0));
  CHECK(wait_until(
      [&] {
        return denied.status_text().contains(QStringLiteral("auditor")) &&
               denied.status_text().contains(QStringLiteral("操作失败"));
      },
      8000));
  CHECK(denied.result_count() == 0);

  // 授 owner1 auditor（CLI；与 test_audit_dialog 同款）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("owner1"),
             QStringLiteral("auditor"), QStringLiteral("--by"),
             QStringLiteral("owner1"), QStringLiteral("--db"),
             db}) == 0);

  ArchiveConsole console;
  console.connect_to(QStringLiteral("127.0.0.1"), files_port,
                     QStringLiteral("owner1"), QStringLiteral("pass-2"));
  CHECK(wait_until([&] { return console.is_connected(); }, 8000));
  CHECK(console.run_search(QString(), QStringLiteral("暗号Q7"), 0, 0));
  CHECK(wait_until([&] { return console.result_count() == 2; }, 8000));

  // 回包语义：一条已撤回（原文保留）、一条正常
  bool saw_recalled = false, saw_plain = false;
  QString recalled_id, plain_id;
  for (const auto& v : console.results()) {
    const auto m = v.toObject();
    if (m.value(QStringLiteral("recalled")).toBool()) {
      saw_recalled = true;
      recalled_id = m.value(QStringLiteral("msg_id")).toString();
      CHECK(m.value(QStringLiteral("text")).toString().contains(
          QStringLiteral("暗号Q7")));  // 撤回原文保留（留痕纪律）
    } else {
      saw_plain = true;
      plain_id = m.value(QStringLiteral("msg_id")).toString();
    }
  }
  CHECK(saw_recalled && saw_plain);

  // 五列命中行：两行 QFrame#hit_row；已撤回行带标记胶囊＋撤回事件钮
  CHECK(console.findChildren<QFrame*>(QStringLiteral("hit_row")).size() == 2);
  bool flag_pill = false;
  int ctx_btns = 0, ev_btns = 0;
  for (QLabel* l : console.findChildren<QLabel*>()) {
    if (l->text() == QStringLiteral("已撤回 · 原文留存")) flag_pill = true;
  }
  for (QPushButton* b : console.findChildren<QPushButton*>()) {
    if (b->text() == QStringLiteral("查看上下文")) ++ctx_btns;
    if (b->text() == QStringLiteral("撤回事件")) ++ev_btns;
  }
  CHECK(flag_pill);
  CHECK(ctx_btns == 2);
  CHECK(ev_btns == 1);

  // 查看上下文：会话窗口弹出，前后常规行在、命中行品牌加粗
  console.open_context(recalled_id);
  QDialog* ctx = nullptr;
  CHECK(wait_until([&] {
    ctx = console.findChild<QDialog*>(QStringLiteral("context_view"));
    return ctx != nullptr;
  }, 8000));
  if (ctx) {
    bool has_before = false, has_after = false, hit_bold = false;
    for (QLabel* l : ctx->findChildren<QLabel*>()) {
      if (l->text().contains(QStringLiteral("第一条常规消息"))) has_before = true;
      if (l->text().contains(QStringLiteral("第三条收尾常规消息"))) has_after = true;
      if (l->text().contains(QStringLiteral("暗号Q7")) &&
          l->styleSheet().contains(QStringLiteral("font-weight:700")))
        hit_bold = true;
    }
    CHECK(has_before);
    CHECK(has_after);
    CHECK(hit_bold);
    ctx->close();
    // WA_DeleteOnClose → 删除走 deleteLater；排空延迟删除防下一 findChild
    // 命中陈旧视图（撤回事件/上下文视图复用同一 objectName）
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
  }

  // 撤回事件：撤回人 member1＋时刻；原型「单独可查撤回事件」面
  console.open_recall_event(recalled_id);
  QDialog* ev = nullptr;
  CHECK(wait_until([&] {
    ev = console.findChild<QDialog*>(QStringLiteral("recall_view"));
    return ev != nullptr;
  }, 8000));
  if (ev) {
    bool by_member = false;
    for (QLabel* l : ev->findChildren<QLabel*>()) {
      if (l->text().contains(QStringLiteral("撤回人：member1"))) by_member = true;
    }
    CHECK(by_member);
    ev->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
  }
  // 未撤回消息：exists=false 如实呈现
  console.open_recall_event(plain_id);
  QDialog* ev2 = nullptr;
  CHECK(wait_until([&] {
    ev2 = console.findChild<QDialog*>(QStringLiteral("recall_view"));
    return ev2 != nullptr;
  }, 8000));
  if (ev2) {
    bool not_recalled = false;
    for (QLabel* l : ev2->findChildren<QLabel*>()) {
      if (l->text().contains(QStringLiteral("未撤回"))) not_recalled = true;
    }
    CHECK(not_recalled);
    ev2->close();
  }

  // 导出留证：证据文件落盘（头注/行/计数）＋留证行回显
  const QString export_path = tmp.filePath(QStringLiteral("export.txt"));
  console.export_to(export_path);
  CHECK(wait_until(
      [&] { return console.evidence_text().contains(QStringLiteral("证据包")); },
      8000));
  CHECK(QFile::exists(export_path));
  {
    QFile f(export_path);
    CHECK(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();
    CHECK(content.contains(QStringLiteral("# Memex 归档导出")));
    CHECK(content.contains(QStringLiteral("# 操作者：owner1")));
    CHECK(content.contains(QStringLiteral("暗号Q7")));
    CHECK(content.contains(QStringLiteral("已撤回")));
    CHECK(content.contains(QStringLiteral("共 2 条")));
  }

  // 查阅日志台账：search/context/recall/export 四类留痕都在（audit 自身
  // 也被审计）；被拒留痕（audit.denied）腿在 test_audit_dialog 不重复
  const auto reads_has = [&](const QString& needle) {
    for (int i = 0; i < console.reads_list()->count(); ++i) {
      if (console.reads_list()->item(i)->text().contains(needle)) return true;
    }
    return false;
  };
  CHECK(wait_until([&] { return reads_has(QStringLiteral("audit.message.search")); },
                   8000));
  CHECK(wait_until([&] { return reads_has(QStringLiteral("audit.message.context")); },
                   8000));
  CHECK(wait_until([&] { return reads_has(QStringLiteral("audit.recall.event")); },
                   8000));
  CHECK(wait_until([&] { return reads_has(QStringLiteral("audit.message.export")); },
                   8000));

  // 归档检索台实况进 docs 截图目录（浅＋深；随拍随回亮色基线）——
  // 与 mode_switch 的 live-org 同法：主题令牌重染后回 light
  {
    const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
    auto& tm = memex::client::ThemeManager::instance();
    CHECK(console.grab().save(dir + QStringLiteral("/live-search-light.png")));
    tm.apply(qApp);
    tm.set_mode(QStringLiteral("dark"));
    CHECK(console.grab().save(dir + QStringLiteral("/live-search-dark.png")));
    tm.set_mode(QStringLiteral("light"));
  }

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_archive_console: all checks passed\n";
    return 0;
  }
  std::cout << "test_archive_console: " << g_failures << " check(s) FAILED\n";
  return 1;
}
