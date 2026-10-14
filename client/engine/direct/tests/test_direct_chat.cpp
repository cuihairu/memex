// T1.2 验收：双实例互发文本，送达确认，本地 SQLite 落库（来源＝直连），
// 重开后历史仍在本机；全程无服务端进程参与（本进程外无 memex_server）。
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <functional>

#include <engine/direct/direct_engine.hpp>

using memex::client::DirectEngine;
using memex::client::LocalStore;
using memex::client::StoredMessage;

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

bool contains_text(const QList<StoredMessage>& list, const std::string& text) {
  for (const StoredMessage& m : list) {
    if (m.text == text) return true;
  }
  return false;
}

bool all_source_direct(const QList<StoredMessage>& list) {
  for (const StoredMessage& m : list) {
    if (m.source != "direct") return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  // 独立 UDP 发现口：并发测试进程互不串扰（生产默认口 2425 不受影响）
  {
    QUdpSocket probe;
    if (probe.bind(QHostAddress::AnyIPv4, 0,
                   QAbstractSocket::ShareAddress |
                       QAbstractSocket::ReuseAddressHint)) {
      qputenv("MEMEX_TEST_DISCOVERY_PORT",
              QByteArray::number(probe.localPort()));
      probe.close();
    }
  }
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("direct-chat-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db_a = tmp.filePath(QStringLiteral("a.db"));
  const QString db_b = tmp.filePath(QStringLiteral("b.db"));

  DirectEngine a("dev-A", db_a);
  DirectEngine b("dev-B", db_b);

  int b_received = 0;
  QString last_from;
  QString last_text;
  QObject::connect(&b, &DirectEngine::message_received, &b,
                   [&](const QString& from, const QString& text, qint64, quint64) {
                     ++b_received;
                     last_from = from;
                     last_text = text;
                   });

  quint64 delivered_seq = 0;
  bool delivered_ok = false;
  bool delivered_fired = false;
  QObject::connect(&a, &DirectEngine::text_delivered, &a,
                   [&](quint64 seq, bool ok) {
                     delivered_fired = true;
                     delivered_seq = seq;
                     delivered_ok = ok;
                   });

  CHECK(a.start());
  CHECK(b.start());

  // 发现互现（对端 TCP 端口须已随宣告携带）
  CHECK(wait_until(
      [&] { return a.has_peer("dev-B") && b.has_peer("dev-A"); }, 8000));
  CHECK(a.peer("dev-B").tcp_port != 0);
  CHECK(b.peer("dev-A").tcp_port != 0);

  // A → B 文本
  const quint64 seq = a.send_text("dev-B", "对账单已发你，归档里能查");
  CHECK(seq != 0);
  // 四态推进·已发送（需求批⑦·已读灰勾单）：受理落库即在途，receipt 空
  {
    bool sent_state = false;
    for (const StoredMessage& m : a.history(QStringLiteral("dev-B"))) {
      if (m.seq == seq) sent_state = m.receipt.empty();
    }
    CHECK(sent_state);
  }
  CHECK(wait_until([&] { return b_received >= 1 && delivered_fired; }, 6000));
  CHECK(last_from == QStringLiteral("dev-A"));
  CHECK(last_text == QStringLiteral("对账单已发你，归档里能查"));
  CHECK(delivered_ok);
  CHECK(delivered_seq == seq);
  // 四态推进·已送达：引擎在 text_delivered 信号前按 (收件人, 序号) 落库
  {
    bool delivered_stored = false;
    for (const StoredMessage& m : a.history(QStringLiteral("dev-B"))) {
      if (m.seq == seq) delivered_stored = m.receipt == "delivered";
    }
    CHECK(delivered_stored);
  }

  // —— 已读回执（需求批⑦·已读灰勾单 2026-10-14）：B 读后上报 READ，
  // A 按 (收件人, 序号) 命中自己发出的行落已读终态；重报去重；乱序
  // delivered 不倒退终态（只升不降，store 直写腿） ——
  bool a_read_fired = false;
  quint64 a_read_seq = 0;
  QString a_reader;
  QObject::connect(&a, &DirectEngine::message_read, &a,
                   [&](quint64 s, const QString& reader) {
                     a_read_fired = true;
                     a_read_seq = s;
                     a_reader = reader;
                   });
  CHECK(b.mark_read("dev-A", seq));
  CHECK(wait_until([&] { return a_read_fired; }, 6000));
  CHECK(a_read_seq == seq);
  CHECK(a_reader == QStringLiteral("dev-B"));
  // 去重：同序号重报拒绝（不重发帧；重启后簿记清空重报一次＝幂等补齐）
  CHECK(!b.mark_read("dev-A", seq));
  // 四态推进·已读：A 本地库该行 receipt=read（送达态被已读覆盖）
  {
    bool read_stored = false;
    for (const StoredMessage& m : a.history(QStringLiteral("dev-B"))) {
      if (m.seq == seq) read_stored = m.receipt == "read";
    }
    CHECK(read_stored);
  }
  // 只升不降：乱序后到的 delivered 不倒退已读终态
  CHECK(!a.store()->set_receipt_for_seq("dev-B", seq, "delivered"));
  // 键防串扰：收件人不符／序号不符均不命中
  CHECK(!a.store()->set_receipt_for_seq("dev-C", seq, "delivered"));
  CHECK(!a.store()->set_receipt_for_seq("dev-B", seq + 999, "delivered"));

  // B → A 回复
  const quint64 seq2 = b.send_text("dev-A", "收到，稍后检索确认");
  CHECK(seq2 != 0);
  int a_received = 0;
  QObject::connect(&a, &DirectEngine::message_received, &a,
                   [&](const QString&, const QString&, qint64, quint64) { ++a_received; });
  CHECK(wait_until([&] { return a_received >= 1; }, 6000));

  // 本地落库：来源＝直连
  const auto a_hist = a.history(QStringLiteral("dev-B"));
  const auto b_hist = b.history(QStringLiteral("dev-A"));
  CHECK(contains_text(a_hist, "对账单已发你，归档里能查"));
  CHECK(contains_text(b_hist, "对账单已发你，归档里能查"));
  CHECK(contains_text(b_hist, "收到，稍后检索确认"));
  CHECK(all_source_direct(a_hist));
  CHECK(all_source_direct(b_hist));

  a.stop();
  b.stop();

  // 重开（重新打开库文件）历史仍在本机
  LocalStore reopen;
  CHECK(reopen.open(db_b));
  const auto b_hist2 = reopen.history(QStringLiteral("dev-A"));
  CHECK(contains_text(b_hist2, "对账单已发你，归档里能查"));
  reopen.close();

  if (g_failures == 0) {
    qInfo("direct chat tests: all passed");
    return 0;
  }
  qCritical("direct chat tests: %d failure(s)", g_failures);
  return 1;
}
