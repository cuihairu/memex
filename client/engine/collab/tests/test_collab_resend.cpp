// T2.5 断线补传归档验收（进程级，真实服务端）：
// 协作态双方在线 → 服务端被杀 → 发送方中断期消息本地暂存（不判失败）→
// 服务端恢复 → 自动重连后补传 → 归档无重复无缺失（CLI messages 检索面核对），
// 接收端按 msg_id 去重只收一次（A16）。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>

#include <csignal>

#include <functional>
#include <vector>

#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>

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
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
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

QString run_capture(const QString& bin, const QStringList& args) {
  QProcess p;
  p.start(bin, args);
  // 启动与完成分开等：负载下进程孵化偶发超 5s，只等完成会把启动失败
  // 误报成「CLI 没归档」（本文件历史上的偶发红——整段断言全空即此因）
  if (!p.waitForStarted(5000)) {
    qWarning("run_capture: 进程未启动 %s", qPrintable(bin));
    return QString();
  }
  if (!p.waitForFinished(15000)) {
    qWarning("run_capture: 进程未退出 %s", qPrintable(bin));
    return QString();
  }
  return QString::fromUtf8(p.readAllStandardOutput());
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("collab-resend-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-a")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-b")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"), QStringLiteral("add"),
                             row.first, row.second, QStringLiteral("--db"),
                             db}) == 0);
  }

  const quint16 port = free_port();
  const QStringList server_args = {QStringLiteral("serve"),
                                   QStringLiteral("--db"), db,
                                   QStringLiteral("--port"),
                                   QString::number(port)};
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin, server_args);
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));

  LocalStore store_a, store_b;
  CHECK(store_a.open(tmp.filePath(QStringLiteral("a.db"))));
  CHECK(store_b.open(tmp.filePath(QStringLiteral("b.db"))));

  CollabEngine a, b;
  a.attach_store(&store_a);
  b.attach_store(&store_b);
  a.set_heartbeat(300, 2);
  b.set_heartbeat(300, 2);

  std::vector<QString> b_got;
  QObject::connect(&b, &CollabEngine::message_received, &b,
                   [&](const QString&, const QString& text, qint64,
                       const QString&) { b_got.push_back(text); });
  std::vector<std::pair<quint64, bool>> a_receipts;
  QObject::connect(&a, &CollabEngine::text_delivered, &a,
                   [&](quint64 seq, bool ok) { a_receipts.push_back({seq, ok}); });

  bool a_in = false, b_in = false;
  QObject::connect(&a, &CollabEngine::logged_in, &a,
                   [&](const QString&, const QString&) { a_in = true; });
  QObject::connect(&b, &CollabEngine::logged_in, &b,
                   [&](const QString&, const QString&) { b_in = true; });
  bool a_lost = false, b_lost = false, a_re = false, b_re = false;
  QObject::connect(&a, &CollabEngine::connection_lost, &a, [&] { a_lost = true; });
  QObject::connect(&b, &CollabEngine::connection_lost, &b, [&] { b_lost = true; });
  QObject::connect(&a, &CollabEngine::reconnected, &a, [&] { a_re = true; });
  QObject::connect(&b, &CollabEngine::reconnected, &b, [&] { b_re = true; });

  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-a"));
  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
          QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return a_in && b_in; }, 8000));

  // 在线基线消息（归档）
  const quint64 s1 = a.send_text(QStringLiteral("bob"), QStringLiteral("在线基线消息"));
  CHECK(wait_until([&] { return b_got.size() == 1; }, 8000));
  CHECK(wait_until(
      [&] {
        return !a_receipts.empty() && a_receipts[0].first == s1 &&
               a_receipts[0].second;
      },
      8000));

  // —— 服务端中断：中断期消息本地暂存（不判失败）——
  server.kill();
  CHECK(server.waitForFinished(5000));
  CHECK(wait_until([&] { return a_lost && b_lost; }, 8000));

  const quint64 s2 = a.send_text(QStringLiteral("bob"), QStringLiteral("中断期消息甲"));
  const quint64 s3 = a.send_text(QStringLiteral("bob"), QStringLiteral("中断期消息乙"));
  CHECK(s2 != 0); // 暂存受理（待补传），非失败
  CHECK(s3 != 0);

  // —— 服务端恢复：自动重连 → 补传 → 归档无重复无缺失 ——
  QProcess server2;
  server2.setProcessChannelMode(QProcess::ForwardedChannels);
  server2.start(server_bin, server_args);
  CHECK(server2.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));
  CHECK(wait_until([&] { return a_re && b_re; }, 20000));

  // 接收端收到补传消息（重复投递由 msg_id 去重）
  CHECK(wait_until([&] { return b_got.size() == 3; }, 10000));
  CHECK(b_got[1] == QStringLiteral("中断期消息甲"));
  CHECK(b_got[2] == QStringLiteral("中断期消息乙"));

  // 发送方回执：补传消息均被服务端受理
  CHECK(wait_until(
      [&] {
        return a_receipts.size() == 3 && a_receipts[1].first == s2 &&
               a_receipts[1].second && a_receipts[2].first == s3 &&
               a_receipts[2].second;
      },
      10000));

  // 接收端本地库：每条只落一次
  CHECK(wait_until(
      [&] {
        int copies_jia = 0, copies_yi = 0;
        for (const auto& m : store_b.history(QStringLiteral("alice"))) {
          if (m.text == "中断期消息甲") ++copies_jia;
          if (m.text == "中断期消息乙") ++copies_yi;
        }
        return copies_jia == 1 && copies_yi == 1;
      },
      5000));

  // 归档核对（CLI messages 检索面）：三条全在、无重复、无缺失
  //（messages 为只读检索，短窗重试等服务端归档落盘可见——不改变语义）
  {
    QString out;
    CHECK(wait_until([&] {
            out = run_capture(server_bin,
                              {QStringLiteral("messages"), QStringLiteral("--db"),
                               db, QStringLiteral("--limit"),
                               QStringLiteral("50")});
            return out.contains(QStringLiteral("共 3 条"));
          }, 5000));
    CHECK(out.contains(QStringLiteral("在线基线消息")));
    CHECK(out.count(QStringLiteral("中断期消息甲")) == 1);
    CHECK(out.count(QStringLiteral("中断期消息乙")) == 1);
    CHECK(out.contains(QStringLiteral("共 3 条")));
    CHECK(out.contains(QStringLiteral("alice")) &&
          out.contains(QStringLiteral("bob")));
  }

  // 恢复后正常通道仍可用
  const quint64 s4 = a.send_text(QStringLiteral("bob"), QStringLiteral("恢复后新消息"));
  CHECK(wait_until([&] { return b_got.size() == 4; }, 8000));
  CHECK(wait_until(
      [&] {
        return !a_receipts.empty() && a_receipts.back().first == s4 &&
               a_receipts.back().second;
      },
      8000));
  {
    QString out;
    CHECK(wait_until([&] {
            out = run_capture(server_bin,
                              {QStringLiteral("messages"), QStringLiteral("--db"),
                               db, QStringLiteral("--limit"),
                               QStringLiteral("50")});
            return out.contains(QStringLiteral("共 4 条"));
          }, 5000));
    CHECK(out.contains(QStringLiteral("共 4 条")));
  }

  // —— 平台-9 Sync State：六态状态机验收 ——
  // 按文本取某对端历史行的同步状态（找不到＝<missing>）
  auto sync_of = [](LocalStore& st, const QString& peer,
                    const QString& text) -> QString {
    for (const auto& m : st.history(peer, 200)) {
      if (QString::fromStdString(m.text) == text) {
        return QString::fromStdString(m.sync_state);
      }
    }
    return QStringLiteral("<missing>");
  };

  // 发送侧：服务端受理（ACK 到达）＝SERVER_ACKED；接收方客户端落库发
  // ACK→服务端 DELIVER_NOTICE 回达→发送侧推进 ARCHIVED（发送侧触发面，
  // 等持久库面状态到位）；接收侧：服务端投递落库即 ARCHIVED（接收方向
  // 唯一入口）
  CHECK(wait_until([&] {
    return sync_of(store_a, QStringLiteral("bob"),
                   QStringLiteral("在线基线消息")) ==
           QStringLiteral("ARCHIVED");
  }, 8000));
  CHECK(wait_until([&] {
    return sync_of(store_a, QStringLiteral("bob"),
                   QStringLiteral("中断期消息甲")) ==
           QStringLiteral("ARCHIVED");
  }, 8000));
  CHECK(wait_until([&] {
    return sync_of(store_a, QStringLiteral("bob"),
                   QStringLiteral("中断期消息乙")) ==
           QStringLiteral("ARCHIVED");
  }, 8000));
  CHECK(wait_until([&] {
    return sync_of(store_a, QStringLiteral("bob"),
                   QStringLiteral("恢复后新消息")) ==
           QStringLiteral("ARCHIVED");
  }, 8000));
  CHECK(sync_of(store_b, QStringLiteral("alice"), QStringLiteral("在线基线消息")) ==
        QStringLiteral("ARCHIVED"));
  CHECK(sync_of(store_b, QStringLiteral("alice"), QStringLiteral("中断期消息甲")) ==
        QStringLiteral("ARCHIVED"));
  CHECK(sync_of(store_b, QStringLiteral("alice"), QStringLiteral("中断期消息乙")) ==
        QStringLiteral("ARCHIVED"));
  CHECK(sync_of(store_b, QStringLiteral("alice"), QStringLiteral("恢复后新消息")) ==
        QStringLiteral("ARCHIVED"));

  // —— 平台-9 纯库层：set_sync_state / pending_sync（独立临时库）——
  {
    LocalStore sp;
    CHECK(sp.open(tmp.filePath(QStringLiteral("p9.db"))));
    memex::client::StoredMessage m;
    m.peer = "carol";
    m.from = "alice";
    m.to = "carol";
    m.ts_ms = 1;
    m.source = "collab";
    m.seq = 1;
    m.text = "待发一";
    m.sync_state = "PENDING";
    CHECK(sp.append(m));
    m.seq = 2;
    m.text = "在途二";
    m.sync_state = "SENDING";
    CHECK(sp.append(m));
    m.seq = 3;
    m.text = "已受理三";
    m.sync_state = "SERVER_ACKED";
    CHECK(sp.append(m));
    m.seq = 4;
    m.text = "历史行";
    m.sync_state = ""; // 空串＝状态机启用前的历史行，不参与恢复
    CHECK(sp.append(m));

    auto pends = sp.pending_sync("alice");
    CHECK(pends.size() == 2);
    CHECK(pends[0].seq == 1 && pends[1].seq == 2); // seq 升序

    CHECK(sp.set_sync_state("alice", 1, "FAILED"));
    pends = sp.pending_sync("alice");
    CHECK(pends.size() == 1 && pends[0].seq == 2);
    CHECK(sp.set_sync_state("alice", 2, "SERVER_ACKED"));
    CHECK(sp.pending_sync("alice").isEmpty());
    CHECK(!sp.set_sync_state("alice", 999, "FAILED")); // 不存在的行＝false
    CHECK(!sp.set_sync_state("nobody", 1, "FAILED"));

    CHECK(sync_of(sp, QStringLiteral("carol"), QStringLiteral("待发一")) ==
          QStringLiteral("FAILED"));
    CHECK(sync_of(sp, QStringLiteral("carol"), QStringLiteral("历史行")).isEmpty());

    // 发送侧归档推进（按 msg_id）：只从 SERVER_ACKED 走——其余态
    //（PENDING/SENDING/ARCHIVED/空历史）不倒退不越级，未知 id=false
    CHECK(sp.set_sync_state("alice", 2, "SERVER_ACKED")); // 复位回受理态
    CHECK(sp.set_msg_id("alice", 2, "sha-a2"));
    CHECK(sp.set_sync_state_archived("sha-a2"));
    CHECK(sync_of(sp, QStringLiteral("carol"), QStringLiteral("在途二")) ==
          QStringLiteral("ARCHIVED"));
    CHECK(!sp.set_sync_state_archived("sha-a2")); // 已 ARCHIVED＝不再命中
    CHECK(!sp.set_sync_state_archived("sha-none")); // 未知 msg_id
    m.seq = 5;
    m.text = "在途五";
    m.sync_state = "SENDING";
    CHECK(sp.append(m));
    CHECK(sp.set_msg_id("alice", 5, "sha-a5"));
    CHECK(!sp.set_sync_state_archived("sha-a5")); // SENDING 不越级
    CHECK(sync_of(sp, QStringLiteral("carol"), QStringLiteral("在途五")) ==
          QStringLiteral("SENDING"));
    sp.close();
  }

  // —— 平台-9 登出放弃：在线在途（回执未达）→ 显式登出终态 FAILED ——
  // 服务端进程冻结（SIGSTOP）：TCP 连接未断（logged_in_ 保持），但回执
  // 永不到达——比杀进程更早的心跳判死之前完成登出，路径确定
  {
    ::kill(server2.processId(), SIGSTOP);

    bool gave_up = false;
    quint64 gave_up_seq = 0;
    QObject::connect(&a, &CollabEngine::text_delivered, &a,
                     [&](quint64 seq, bool ok) {
                       if (!ok) {
                         gave_up = true;
                         gave_up_seq = seq;
                       }
                     });

    const quint64 s5 =
        a.send_text(QStringLiteral("bob"), QStringLiteral("登出放弃消息"));
    CHECK(s5 != 0);
    CHECK(wait_until(
        [&] {
          return sync_of(store_a, QStringLiteral("bob"),
                         QStringLiteral("登出放弃消息")) == "SENDING";
        },
        3000));

    a.logout(); // 在线显式登出：在途消息不再补传 → delivered(false) + FAILED
    CHECK(wait_until([&] { return gave_up && gave_up_seq == s5; }, 3000));
    CHECK(sync_of(store_a, QStringLiteral("bob"), QStringLiteral("登出放弃消息")) ==
          QStringLiteral("FAILED"));
    CHECK(store_a.pending_sync("alice").isEmpty()); // 终态行不参与恢复

    ::kill(server2.processId(), SIGCONT);
    server2.kill();
    CHECK(server2.waitForFinished(5000));
  }

  // —— 平台-9 重启恢复：进程崩溃丢内存队列 → 库面 PENDING 行重新入队 ——
  {
    // 服务端复活：bob 自动重连；alice 已手动登出不再自动连
    QProcess server3;
    server3.setProcessChannelMode(QProcess::ForwardedChannels);
    server3.start(server_bin, server_args);
    CHECK(server3.waitForStarted(5000));
    CHECK(wait_until([&] { return port_listening(port); }, 8000));
    bool b_re2 = false;
    QObject::connect(&b, &CollabEngine::reconnected, &b, [&] { b_re2 = true; });
    CHECK(wait_until([&] { return b_re2; }, 20000));

    // 模拟崩溃：新引擎登录后随服务端中断发送（暂存 PENDING），不登出
    // 直接析构——析构不做 teardown，内存队列丢弃、库里行留在 PENDING
    {
      CollabEngine crashed;
      crashed.attach_store(&store_a);
      crashed.set_heartbeat(300, 2);
      bool c_in = false;
      QObject::connect(&crashed, &CollabEngine::logged_in, &crashed,
                       [&](const QString&, const QString&) { c_in = true; });
      crashed.login(QStringLiteral("127.0.0.1"), port,
                    QStringLiteral("alice"), QStringLiteral("pass-a"));
      CHECK(wait_until([&] { return c_in; }, 8000));

      server3.kill();
      CHECK(server3.waitForFinished(5000));
      bool c_lost = false;
      QObject::connect(&crashed, &CollabEngine::connection_lost, &crashed,
                       [&] { c_lost = true; });
      CHECK(wait_until([&] { return c_lost; }, 8000));

      CHECK(crashed.send_text(QStringLiteral("bob"),
                              QStringLiteral("重启恢复消息")) != 0);
      CHECK(wait_until(
          [&] {
            return sync_of(store_a, QStringLiteral("bob"),
                           QStringLiteral("重启恢复消息")) == "PENDING";
          },
          3000));
    } // crashed 析构＝崩溃同型（内存队列没了，库面行还在）

    // 服务端复活 + 全新引擎登录：flush_pending_sends 从库回收 PENDING 行
    QProcess server4;
    server4.setProcessChannelMode(QProcess::ForwardedChannels);
    server4.start(server_bin, server_args);
    CHECK(server4.waitForStarted(5000));
    CHECK(wait_until([&] { return port_listening(port); }, 8000));

    CollabEngine a3;
    a3.attach_store(&store_a);
    a3.set_heartbeat(300, 2);
    bool a3_in = false;
    QObject::connect(&a3, &CollabEngine::logged_in, &a3,
                     [&](const QString&, const QString&) { a3_in = true; });
    a3.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
             QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return a3_in; }, 8000));

    // 补传到位：内存队列已死，恢复只能靠库面行——bob 恰收到一条
    CHECK(wait_until([&] { return b_got.size() == 5; }, 20000));
    CHECK(b_got[4] == QStringLiteral("重启恢复消息"));
    CHECK(wait_until(
        [&] {
          return sync_of(store_a, QStringLiteral("bob"),
                         QStringLiteral("重启恢复消息")) == "SERVER_ACKED";
        },
        8000));
    CHECK(sync_of(store_b, QStringLiteral("alice"),
                  QStringLiteral("重启恢复消息")) == QStringLiteral("ARCHIVED"));
    CHECK(store_a.pending_sync("alice").isEmpty());

    // 归档恰一条（CLI messages 对账）
    {
      QString out;
      CHECK(wait_until([&] {
              out = run_capture(server_bin,
                                {QStringLiteral("messages"), QStringLiteral("--db"),
                                 db, QStringLiteral("--limit"),
                                 QStringLiteral("50")});
              return out.contains(QStringLiteral("共 5 条"));
            }, 5000));
      CHECK(out.count(QStringLiteral("重启恢复消息")) == 1);
    }

    a3.logout();
  }

  b.logout();
  server2.terminate();
  server2.waitForFinished(3000);

  if (g_failures == 0) {
    qInfo("collab resend tests: all passed");
    return 0;
  }
  qCritical("collab resend tests: %d failure(s)", g_failures);
  return 1;
}
