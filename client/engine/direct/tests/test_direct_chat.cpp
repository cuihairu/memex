// T1.2 验收：双实例互发文本，送达确认，本地 SQLite 落库（来源＝直连），
// 重开后历史仍在本机；全程无服务端进程参与（本进程外无 memex_server）。
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

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
                   [&](const QString& from, const QString& text, qint64) {
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
  CHECK(wait_until([&] { return b_received >= 1 && delivered_fired; }, 6000));
  CHECK(last_from == QStringLiteral("dev-A"));
  CHECK(last_text == QStringLiteral("对账单已发你，归档里能查"));
  CHECK(delivered_ok);
  CHECK(delivered_seq == seq);

  // B → A 回复
  const quint64 seq2 = b.send_text("dev-A", "收到，稍后检索确认");
  CHECK(seq2 != 0);
  int a_received = 0;
  QObject::connect(&a, &DirectEngine::message_received, &a,
                   [&](const QString&, const QString&, qint64) { ++a_received; });
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
