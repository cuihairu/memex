// 平台-8 验收：直连安全（Ed25519 身份认证握手 + X25519 会话密钥 + AEAD
// 密文 + TOFU 定针；发现宣告只做发现，身份只在 TCP 握手认证）。
// 腿：
// ① 信道单元对拨：握手往返、应用密文线路不可见、篡改 GCM 拒、重放计数拒、
//    未握手 protect 拒、首触定针落库；
// ② 真引擎 raw-socket 协议腿：裸信道连 B（发现端口）握手 → 密文 TEXT →
//    ACK 解封回流；同库裸载身份与引擎一致；
// ③ 明文直插拒：未握手发 TEXT → 连接被断且无消息；
// ④ TOFU 定针：预埋异针 → delivered false（无明文回退）；清针恢复 → true；
// ⑤ 重启同库同公钥（对端定针跨会话稳定的前提）。
#include <QAbstractSocket>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QTcpSocket>
#include <QThread>
#include <QUdpSocket>

#include <functional>

#include <engine/direct/direct_engine.hpp>
#include <engine/direct/secure_channel.hpp>
#include <memex/protocol/messages.hpp>

using memex::client::DeviceIdentity;
using memex::client::DirectEngine;
using memex::client::LocalStore;
using memex::client::SecureChannel;
using memex::protocol::Message;
using memex::protocol::MsgType;

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

// 喂入 socket 已到字节；failed 时经 fed 返回（默认未建立、未失败）
SecureChannel::Fed feed_available(SecureChannel* ch, QTcpSocket* sock) {
  SecureChannel::Fed fed;
  if (sock->bytesAvailable() <= 0) return fed;
  const QByteArray d = sock->readAll();
  fed = ch->feed(
      std::string_view(d.constData(), static_cast<std::size_t>(d.size())));
  return fed;
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
  QCoreApplication::setApplicationName(QStringLiteral("direct-secure-test"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());

  // ---------- ① 信道单元对拨（不经 socket，同步拉式 API） ----------
  {
    const QString db_a = tmp.filePath(QStringLiteral("u-a.db"));
    const QString db_b = tmp.filePath(QStringLiteral("u-b.db"));
    LocalStore store_a;
    LocalStore store_b;
    CHECK(store_a.open(db_a));
    CHECK(store_b.open(db_b));

    DeviceIdentity id_a = DeviceIdentity::load(&store_a);
    DeviceIdentity id_b = DeviceIdentity::load(&store_b);
    CHECK(id_a.valid());
    CHECK(id_b.valid());
    CHECK(id_a.pub_hex().size() == 64);
    CHECK(id_a.pub_hex() != id_b.pub_hex());

    SecureChannel a(SecureChannel::Role::kInitiator, &id_a, &store_a, "dev-A",
                    "dev-B");
    SecureChannel b(SecureChannel::Role::kResponder, &id_b, &store_b, "dev-B",
                    "");
    // 握手前拒发应用数据
    CHECK(a.protect("too-early").empty());

    const std::string hello1 = a.start();
    CHECK(!hello1.empty());
    const auto f1 = b.feed(hello1);
    CHECK(!f1.failed);
    CHECK(f1.established);
    CHECK(!f1.reply.empty());
    const auto f2 = a.feed(f1.reply);
    CHECK(!f2.failed);
    CHECK(f2.established);
    CHECK(a.established());
    CHECK(b.established());
    // 首触定针：B 记下 A 公钥
    CHECK(store_b.peer_identity_pub("dev-A") == id_a.pub_hex());
    CHECK(store_a.peer_identity_pub("dev-B") == id_b.pub_hex());

    // 密文线路不可见 + 双向往返
    const std::string canary = "plaintext-canary-0123456789";
    const std::string wire1 = a.protect(canary);
    CHECK(!wire1.empty());
    CHECK(wire1.find(canary) == std::string::npos);
    const auto f3 = b.feed(wire1);
    CHECK(!f3.failed);
    CHECK(f3.payloads.size() == 1);
    CHECK(!f3.payloads.empty() && f3.payloads[0] == canary);

    const std::string back = "reply-canary-xyz";
    const std::string wire2 = b.protect(back);
    CHECK(!wire2.empty());
    CHECK(wire2.find(back) == std::string::npos);
    const auto f4 = a.feed(wire2);
    CHECK(!f4.failed);
    CHECK(f4.payloads.size() == 1);
    CHECK(!f4.payloads.empty() && f4.payloads[0] == back);

    // 重放拒：同一帧第二次（TCP 保序下计数必须严格递增）
    const auto f5 = b.feed(wire1);
    CHECK(f5.failed);
    CHECK(f5.reason.contains(QStringLiteral("计数错序")));

    // 篡改拒：新信道对，翻密文首字节（4B len + 8B ctr 之后）
    SecureChannel a2(SecureChannel::Role::kInitiator, &id_a, &store_a, "dev-A",
                     "dev-B");
    SecureChannel b2(SecureChannel::Role::kResponder, &id_b, &store_b, "dev-B",
                     "");
    const auto g1 = b2.feed(a2.start());
    CHECK(g1.established);
    const auto g2 = a2.feed(g1.reply);
    CHECK(g2.established);
    std::string wire_t = a2.protect("tamper-target");
    CHECK(wire_t.size() > 12);
    wire_t[12] ^= 0xFF;
    const auto g3 = b2.feed(wire_t);
    CHECK(g3.failed);
    CHECK(g3.reason.contains(QStringLiteral("认证失败")));

    store_a.close();
    store_b.close();
  }

  // ---------- ② 真引擎 raw-socket 协议腿 ----------
  const QString db_ea = tmp.filePath(QStringLiteral("eng-a.db"));
  const QString db_eb = tmp.filePath(QStringLiteral("eng-b.db"));
  DirectEngine a("dev-A", db_ea);
  DirectEngine b("dev-B", db_eb);

  int b_received = 0;
  QString last_from;
  QString last_text;
  QObject::connect(&b, &DirectEngine::message_received, &b,
                   [&](const QString& from, const QString& text, qint64) {
                     ++b_received;
                     last_from = from;
                     last_text = text;
                   });
  bool got_delivered = false;
  bool last_ok = false;
  QObject::connect(&a, &DirectEngine::text_delivered, &a,
                   [&](quint64, bool ok) {
                     got_delivered = true;
                     last_ok = ok;
                   });

  CHECK(a.start());
  CHECK(b.start());
  CHECK(!a.identity_pub_hex().empty());
  CHECK(a.identity_pub_hex().size() == 64);
  CHECK(a.identity_pub_hex() != b.identity_pub_hex());
  // 发现互现（对端 TCP 端口须已随宣告携带）
  CHECK(wait_until(
      [&] { return a.has_peer("dev-B") && b.has_peer("dev-A"); }, 8000));
  const quint16 port_b = a.peer("dev-B").tcp_port;
  CHECK(port_b != 0);
  // 定针尚未存在（首触前为空）
  CHECK(b.store()->peer_identity_pub("dev-A").empty());

  // 裸信道（不经引擎发送面）：同库载入与引擎同一身份
  DeviceIdentity raw_id = DeviceIdentity::load(a.store());
  CHECK(raw_id.valid());
  CHECK(raw_id.pub_hex() == a.identity_pub_hex());

  SecureChannel raw(SecureChannel::Role::kInitiator, &raw_id, a.store(),
                    "dev-A", "dev-B");
  QTcpSocket sock;
  sock.connectToHost(QHostAddress::LocalHost, port_b);
  CHECK(sock.waitForConnected(3000));
  const std::string raw_hello = raw.start();
  CHECK(!raw_hello.empty());
  sock.write(QByteArray(raw_hello.data(),
                        static_cast<qsizetype>(raw_hello.size())));
  CHECK(wait_until(
      [&] {
        const auto fed = feed_available(&raw, &sock);
        if (fed.failed) return false;
        return raw.established();
      },
      3000));
  CHECK(raw.established());
  // 首触定针：B 记下的 A 公钥＝引擎公钥
  CHECK(b.store()->peer_identity_pub("dev-A") == a.identity_pub_hex());

  // 密文 TEXT（线路不可见）→ B 落消息 → ACK 密文回流解封
  Message text;
  text.set_type(MsgType::TEXT);
  text.set_seq(42);
  text.set_from("dev-A");
  text.set_to("dev-B");
  text.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  text.mutable_text()->set_text("raw-腿密文消息");
  const std::string wire_text =
      raw.protect(memex::protocol::encode_payload(text));
  CHECK(!wire_text.empty());
  CHECK(wire_text.find("raw-腿密文消息") == std::string::npos);
  const int before_raw = b_received;
  sock.write(QByteArray(wire_text.data(),
                        static_cast<qsizetype>(wire_text.size())));
  CHECK(wait_until([&] { return b_received > before_raw; }, 4000));
  CHECK(last_from == QStringLiteral("dev-A"));
  CHECK(last_text == QStringLiteral("raw-腿密文消息"));

  bool ack_ok = false;
  CHECK(wait_until(
      [&] {
        const auto fed = feed_available(&raw, &sock);
        if (fed.failed) return false;
        for (const auto& payload : fed.payloads) {
          try {
            const Message m = memex::protocol::decode_payload(payload);
            if (m.type() == MsgType::ACK) ack_ok = (m.seq() == 42);
          } catch (const memex::protocol::ProtocolError&) {
            return false;
          }
        }
        return ack_ok;
      },
      4000));
  CHECK(ack_ok);
  sock.disconnectFromHost();

  // ---------- ③ 明文直插拒：未握手发 TEXT → 断开且无消息 ----------
  {
    const int before_plain = b_received;
    QTcpSocket plain;
    plain.connectToHost(QHostAddress::LocalHost, port_b);
    CHECK(plain.waitForConnected(3000));
    Message pt;
    pt.set_type(MsgType::TEXT);
    pt.set_seq(99);
    pt.set_from("dev-A");
    pt.set_to("dev-B");
    pt.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
    pt.mutable_text()->set_text("明文直插应被拒");
    const std::string pt_frame = memex::protocol::encode(pt);
    plain.write(QByteArray(pt_frame.data(),
                           static_cast<qsizetype>(pt_frame.size())));
    CHECK(wait_until(
        [&] { return plain.state() != QAbstractSocket::ConnectedState; },
        3000));
    CHECK(b_received == before_plain);
    plain.close();
  }

  // ---------- ④ TOFU 定针：异针拒 + 清针恢复 ----------
  {
    // 预埋异针：B 侧把 dev-A 钉到另一把公钥（先清旧针，pin 不覆盖既有行）
    CHECK(b.store()->clear_peer_identity("dev-A"));
    const QString db_other = tmp.filePath(QStringLiteral("other.db"));
    LocalStore store_other;
    CHECK(store_other.open(db_other));
    DeviceIdentity id_other = DeviceIdentity::load(&store_other);
    CHECK(id_other.valid());
    CHECK(id_other.pub_hex() != a.identity_pub_hex());
    CHECK(b.store()->pin_peer_identity("dev-A", id_other.pub_hex()));
    store_other.close();

    const int before_bad = b_received;
    got_delivered = false;
    last_ok = true;
    const quint64 seq_bad = a.send_text("dev-B", "错针应被拒");
    CHECK(seq_bad != 0);
    CHECK(wait_until([&] { return got_delivered; }, 6000));
    CHECK(!last_ok); // 握手被拒 → 送达失败，无明文回退
    CHECK(b_received == before_bad); // 且对端没收到任何消息

    // 清针恢复：显式清针后重新首触，定针回到 A 真公钥
    CHECK(b.store()->clear_peer_identity("dev-A"));
    got_delivered = false;
    last_ok = false;
    const quint64 seq_ok = a.send_text("dev-B", "清针后恢复");
    CHECK(seq_ok != 0);
    CHECK(wait_until([&] { return got_delivered; }, 6000));
    CHECK(last_ok);
    CHECK(wait_until([&] { return b_received == before_bad + 1; }, 4000));
    CHECK(last_text == QStringLiteral("清针后恢复"));
    CHECK(b.store()->peer_identity_pub("dev-A") == a.identity_pub_hex());
  }

  // ---------- ⑤ 重启同库同公钥 ----------
  {
    const std::string pub_before = a.identity_pub_hex();
    CHECK(pub_before.size() == 64);
    a.stop();
    DirectEngine a2("dev-A", db_ea);
    CHECK(a2.start());
    CHECK(a2.identity_pub_hex() == pub_before);
    a2.stop();
    b.stop();
  }

  if (g_failures == 0) {
    qInfo("direct secure tests: all passed");
    return 0;
  }
  qCritical("direct secure tests: %d failure(s)", g_failures);
  return 1;
}
