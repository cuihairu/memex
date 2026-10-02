// T1.1 验收：同机双实例互现；停掉一端后另一端在超时内置为离线。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <functional>

#include <engine/direct/discovery.hpp>

using memex::client::DiscoveryOptions;
using memex::client::DiscoveryService;
using memex::client::Peer;

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

bool has_peer(const DiscoveryService& svc, const char* device_id) {
  const auto list = svc.peers();
  for (const Peer& p : list) {
    if (p.device_id == device_id) return true;
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  DiscoveryOptions opts;
  opts.announce_interval_ms = 400;
  opts.peer_timeout_ms = 1500;
  opts.sweep_interval_ms = 150;

  DiscoveryService a("dev-A", "host-A");
  DiscoveryService b("dev-B", "host-B");

  bool b_left_seen = false;
  QObject::connect(&a, &DiscoveryService::peerLeft, &a,
                   [&](const std::string& id) {
                     if (id == "dev-B") b_left_seen = true;
                   });

  CHECK(a.start(opts));
  CHECK(b.start(opts));

  // 同机双实例互现
  CHECK(wait_until([&] { return has_peer(a, "dev-B") && has_peer(b, "dev-A"); },
                   8000));
  // 不应看到自己
  CHECK(!has_peer(a, "dev-A"));
  CHECK(!has_peer(b, "dev-B"));

  // 停掉一端 → 另一端超时置离线
  b.stop();
  CHECK(wait_until([&] { return !has_peer(a, "dev-B"); }, 6000));
  CHECK(b_left_seen);

  a.stop();

  if (g_failures == 0) {
    qInfo("discovery tests: all passed");
    return 0;
  }
  qCritical("discovery tests: %d failure(s)", g_failures);
  return 1;
}
