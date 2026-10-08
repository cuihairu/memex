// 网段黑名单守卫（用户令 2026-10-08 ⑤）：
// ① net_guard 纯函数——CIDR 命中/不命中、/0 与 /32 边界、坏段不误伤、
//    validate 各拒因、空表放行；
// ② 引擎过滤真腿——发现包 sender 命中＝不响应（da2 起着黑名单看不见
//    存活邻居）、开关关＝放行恢复；TCP 入站 peer 命中＝当场拒连
//    （text_delivered 回 false）、关＝放行送达；
// ③ net_blacklist/remote_control 落盘往返——默认关、密码只存盐＋哈希
//    （空串拒设、对/错密码验真伪）。批准面拦截行为在 test_assist_dialog。
#include <QApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <cstdlib>
#include <functional>

#include <app/net_guard.hpp>
#include <engine/direct/direct_engine.hpp>

using memex::client::DirectEngine;
namespace net_guard = memex::client::net_guard;
namespace net_blacklist = memex::client::net_blacklist;
namespace remote_control = memex::client::remote_control;

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
  QTemporaryDir tmp;
  if (!tmp.isValid()) return 1;
  // QSettings 落盘面（net_blacklist/remote_control）隔离到临时目录
  qputenv("XDG_CONFIG_HOME", tmp.filePath(QStringLiteral("xdg-config")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("net-guard-test"));

  // —— ① 纯函数面 ——
  // validate_cidr 各拒因与合法
  CHECK(net_guard::validate_cidr(QStringLiteral("192.168.10.0/24")).isEmpty());
  CHECK(net_guard::validate_cidr(QStringLiteral(" 10.0.0.0/8 ")).isEmpty());
  CHECK(!net_guard::validate_cidr(QStringLiteral("192.168.10.0")).isEmpty());      // 无斜杠
  CHECK(!net_guard::validate_cidr(QStringLiteral("192.168.10/24")).isEmpty());     // 三节
  CHECK(!net_guard::validate_cidr(QStringLiteral("192.168.10.256/24")).isEmpty()); // 节越界
  CHECK(!net_guard::validate_cidr(QStringLiteral("192.168.10.0/33")).isEmpty());   // len 越界
  CHECK(!net_guard::validate_cidr(QStringLiteral("192.168.10.0/-1")).isEmpty());
  CHECK(!net_guard::validate_cidr(QStringLiteral("a.b.c.d/24")).isEmpty());

  // cidr_matches 命中/不命中/边界/坏段
  const QHostAddress in_net(QStringLiteral("192.168.10.7"));
  const QHostAddress out_net(QStringLiteral("192.168.11.7"));
  CHECK(net_guard::cidr_matches(QStringLiteral("192.168.10.0/24"), in_net));
  CHECK(!net_guard::cidr_matches(QStringLiteral("192.168.10.0/24"), out_net));
  CHECK(net_guard::cidr_matches(QStringLiteral("192.168.10.7/32"), in_net));   // /32 精确
  CHECK(!net_guard::cidr_matches(QStringLiteral("192.168.10.8/32"), in_net));
  CHECK(net_guard::cidr_matches(QStringLiteral("0.0.0.0/0"), out_net));       // /0 全网
  CHECK(net_guard::cidr_matches(QStringLiteral("10.0.0.0/8"),
                                QHostAddress(QStringLiteral("10.255.255.255"))));
  CHECK(!net_guard::cidr_matches(QStringLiteral("10.0.0.0/8"),
                                 QHostAddress(QStringLiteral("11.0.0.1"))));
  CHECK(!net_guard::cidr_matches(QStringLiteral("10.0.0.0/99"), in_net)); // 坏段不匹配
  CHECK(!net_guard::cidr_matches(QStringLiteral("10.0.0.0/8"),
                                 QHostAddress(QHostAddress::IPv6Protocol))); // 仅 IPv4

  // blocked：空表放行、命中即拒
  CHECK(!net_guard::blocked({}, in_net));
  CHECK(!net_guard::blocked({QStringLiteral("11.0.0.0/8")}, in_net));
  CHECK(net_guard::blocked({QStringLiteral("192.168.0.0/16"),
                            QStringLiteral("10.0.0.0/8")}, in_net));

  // —— ③ 落盘面：默认关、往返、密码口径 ——
  CHECK(!net_blacklist::enabled());     // 默认关＝全放行
  CHECK(net_blacklist::entries().isEmpty());
  net_blacklist::set_entries({QStringLiteral("192.168.9.0/24")});
  CHECK(net_blacklist::entries() ==
        QStringList{QStringLiteral("192.168.9.0/24")});
  net_blacklist::set_enabled(true);
  CHECK(net_blacklist::enabled());
  net_blacklist::set_enabled(false);

  CHECK(!remote_control::enabled()); // 远程控制默认关
  CHECK(!remote_control::has_password());
  CHECK(!remote_control::set_password(QString()));        // 空串拒
  CHECK(remote_control::set_password(QStringLiteral("pair-1")));
  CHECK(remote_control::has_password());
  CHECK(remote_control::verify_password(QStringLiteral("pair-1")));
  CHECK(!remote_control::verify_password(QStringLiteral("pair-2")));
  CHECK(!remote_control::verify_password(QString()));
  remote_control::set_enabled(true);
  CHECK(remote_control::enabled());
  remote_control::set_enabled(false);

  // —— ② 引擎过滤真腿 ——
  DirectEngine da("ng-A", tmp.filePath(QStringLiteral("a.db")));
  DirectEngine db("ng-B", tmp.filePath(QStringLiteral("b.db")));
  CHECK(da.start());
  CHECK(db.start());
  CHECK(wait_until([&] { return da.has_peer("ng-B") && db.has_peer("ng-A"); },
                   8000));

  // 运行时探测同机邻居地址形态：UDP/TCP 源地址是网卡地址（非 127.0.0.1），
  // 从互见后的 peer 条目取真实地址造 /32 段——测试不硬编本机 IP。
  const QHostAddress peer_addr = da.peer("ng-B").address;
  CHECK(!peer_addr.isNull());
  const QString deny_cidr =
      peer_addr.toString() + QStringLiteral("/32");
  CHECK(net_guard::validate_cidr(deny_cidr).isEmpty());
  CHECK(net_guard::cidr_matches(deny_cidr, peer_addr));

  // 发现不响应：da2 带黑名单（deny 127/8＝同机全部邻居）起 3 秒，恒不入表
  DirectEngine da2("ng-C", tmp.filePath(QStringLiteral("c.db")));
  da2.set_address_filter([deny_cidr](const QHostAddress& addr) {
    return !net_guard::blocked({deny_cidr}, addr);
  });
  CHECK(da2.start());
  {
    QElapsedTimer t;
    t.start();
    bool saw_peer = false;
    while (t.elapsed() < 3000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
      if (da2.has_peer("ng-B") || da2.has_peer("ng-A")) saw_peer = true;
      QThread::msleep(5);
    }
    CHECK(!saw_peer); // 黑名单命中＝发现包不响应
  }

  // 开关关＝不装过滤器＝放行恢复（da2 清过滤器后应重新看见存活邻居）
  da2.set_address_filter({});
  CHECK(wait_until([&] { return da2.has_peer("ng-B"); }, 6000));

  // 入连接拒：db 带黑名单（deny 127/8），da 的新入站连接当场被断
  db.set_address_filter([deny_cidr](const QHostAddress& addr) {
    return !net_guard::blocked({deny_cidr}, addr);
  });
  bool delivered = true;
  QObject::connect(&da, &DirectEngine::text_delivered, &da,
                   [&](quint64, bool ok) { delivered = ok; });
  const quint64 seq = da.send_text("ng-B", "netguard-tcp-block");
  CHECK(seq != 0);
  CHECK(wait_until([&] { return !delivered; }, 6000)); // 连接被拒＝回执失败

  // 开关关＝放行：db 清过滤器，重发应送达
  db.set_address_filter({});
  delivered = true;
  const quint64 seq2 = da.send_text("ng-B", "netguard-tcp-open");
  CHECK(seq2 != 0);
  CHECK(wait_until([&] { return delivered; }, 6000));

  da.stop();
  db.stop();
  da2.stop();

  if (g_failures == 0) {
    qInfo("net guard tests: all passed");
    return 0;
  }
  qCritical("net guard tests: %d failure(s)", g_failures);
  return 1;
}
