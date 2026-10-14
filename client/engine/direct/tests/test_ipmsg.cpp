// 飞秋/IPMSG 兼容验收（2026-10-11）：编解码往返与 GB18030/UTF-8 兜底；
// mock 飞秋对拨——在线表 BR_ENTRY/ANSENTRY/BR_EXIT、单聊 SENDMSG/RECVMSG
// 回执、UTF-8 兜底带 UTF8OPT、关断静默、memex 标记不重复建行；引擎全链
// （send_text 分流→回执关单、收文落库上抛）。真飞秋客户端未实测（缺环境，
// 见 docs 飞秋兼容页）——本文件＝协议口径与状态机的机器验收。
//
// 同口投递口径（实测钉死，本机与 CI 同为 Linux 内核）：SO_REUSEADDR 下
// 广播进所有同口 socket、单播只进最后绑定者。mock 因此分两面：广播面先绑
// 同口（只收 memex BR_ENTRY 广播＝在线互见腿），单播面临时口（与 memex
// 收发对称——Peer::udp_port 记来源口）。memex 实例一律后绑同口，保单播
// 归 memex。
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QTemporaryDir>
#include <QThread>
#include <QUdpSocket>

#include <functional>

#include <engine/direct/direct_engine.hpp>
#include <engine/direct/ipmsg.hpp>

using memex::client::DirectEngine;
using memex::client::DiscoveryOptions;
using memex::client::DiscoveryService;
using memex::client::Peer;
using namespace memex::client::ipmsg;

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

bool has_peer(const DiscoveryService& svc, const std::string& device_id) {
  for (const Peer& p : svc.peers()) {
    if (p.device_id == device_id) return true;
  }
  return false;
}

// GB18030 编码（mock 身份/昵称/消息体：飞秋本地编码口径）
QByteArray gb(const QString& text) {
  QStringEncoder enc("GB18030");
  return enc.encode(text);
}

// —— mock 飞秋：host 恒 "FEIQPC"（非 memex 标记，memex 收到按飞秋对端
//    入列）。协议口径：BR_ENTRY→回 ANSENTRY；SENDMSG 带 SENDCHECKOPT→
//    回 RECVMSG（附加区＝原包号，走来源口单播）——
class MockFeiq {
public:
  MockFeiq() {
    QObject::connect(&uni_, &QUdpSocket::readyRead, &uni_, [this] {
      while (uni_.hasPendingDatagrams()) {
        const QNetworkDatagram d = uni_.receiveDatagram();
        Packet p;
        if (!parse(d.data(), p) || p.ver != kVersion) continue;
        if (p.user == user_ && p.host == host_) continue; // 自己回环
        switch (command_of(p.cmd)) {
          case kAnsEntry:
            if (p.host == "memex") ++ans_from_memex_;
            break;
          case kSendMsg:
            ++send_from_memex_;
            last_text_ = decode_text(p.additional, (p.cmd & kUtf8Opt) != 0);
            last_utf8opt_ = (p.cmd & kUtf8Opt) != 0;
            // 要回执则回 RECVMSG（原包号回填附加区，走来源口单播）
            if (p.cmd & kSendCheckOpt) {
              acked_no_ = p.no;
              uni_.writeDatagram(
                  encode(++no_, kRecvMsg, user_, host_,
                         QByteArray::number(p.no)),
                  d.senderAddress(), d.senderPort());
            }
            break;
          case kRecvMsg:
            ++recv_from_memex_;
            last_ack_no_ = p.additional.toUInt();
            break;
          default:
            break;
        }
      }
    });
    QObject::connect(&bcast_, &QUdpSocket::readyRead, &bcast_, [this] {
      while (bcast_.hasPendingDatagrams()) {
        const QNetworkDatagram d = bcast_.receiveDatagram();
        Packet p;
        if (!parse(d.data(), p) || p.ver != kVersion) continue;
        if (p.user == user_ && p.host == host_) continue; // 自己回环
        // 在线互见腿：memex BR_ENTRY 广播（host 恒 memex 标记）
        if (command_of(p.cmd) == kBrEntry && p.host == "memex")
          ++br_from_memex_;
      }
    });
  }

  // 绑定次序契约：广播面与单播面都先于任何 memex 实例绑定同口
  bool bind(quint16 port) {
    if (!bcast_.bind(QHostAddress::AnyIPv4, port,
                     QAbstractSocket::ShareAddress |
                         QAbstractSocket::ReuseAddressHint))
      return false;
    if (!uni_.bind(QHostAddress::AnyIPv4, 0,
                   QAbstractSocket::ShareAddress |
                       QAbstractSocket::ReuseAddressHint))
      return false;
    port_ = port;
    return true;
  }

  // BR_ENTRY/SENDMSG/BR_EXIT 都从单播面发往 memex 同口（回环地址定向，
  // memex 侧按来源口记 udp_port，后续收发对称）
  void send_br_entry() {
    uni_.writeDatagram(encode(++no_, kBrEntry, user_, host_, nick_),
                       QHostAddress::LocalHost, port_);
  }
  void send_sendmsg(quint32 msg_no, const QByteArray& body) {
    uni_.writeDatagram(encode(msg_no, kSendMsg | kSendCheckOpt, user_, host_,
                              body),
                       QHostAddress::LocalHost, port_);
  }
  void send_br_exit() {
    uni_.writeDatagram(encode(++no_, kBrExit, user_, host_, QByteArray()),
                       QHostAddress::LocalHost, port_);
  }

  QByteArray user_ = "feiquser";
  QByteArray host_ = "FEIQPC";
  QByteArray nick_ = gb(QStringLiteral("飞秋用户"));
  quint32 no_ = 100;
  int br_from_memex_{0};   // memex BR_ENTRY 广播（在线互见）
  int ans_from_memex_{0};  // memex 对 mock BR_ENTRY 的 ANSENTRY 应答
  int send_from_memex_{0}; // memex SENDMSG
  int recv_from_memex_{0}; // memex 对 mock SENDMSG 的 RECVMSG 回执
  quint32 last_ack_no_{0};
  quint32 acked_no_{0};
  bool last_utf8opt_{false};
  QString last_text_;

private:
  QUdpSocket bcast_; // 广播面（同口，只收广播）
  QUdpSocket uni_;   // 单播面（临时口，收发对称）
  quint16 port_ = 0;
};

} // namespace

int main(int argc, char** argv) {
  // 独立 UDP 发现口：与其余测试进程互不串扰（直连引擎经环境变量取同口）
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
  const quint16 port = static_cast<quint16>(
      qEnvironmentVariableIntValue("MEMEX_TEST_DISCOVERY_PORT"));
  QCoreApplication app(argc, argv);

  // —— A. 编解码（纯函数面） ——
  {
    const QByteArray raw = encode(77, kSendMsg | kSendCheckOpt, "zhang", "pc",
                                  gb(QStringLiteral("你好:飞秋")));
    Packet p;
    CHECK(parse(raw, p));
    CHECK(p.ver == kVersion);
    CHECK(p.no == 77);
    CHECK(p.cmd == (kSendMsg | kSendCheckOpt));
    CHECK(command_of(p.cmd) == kSendMsg);
    CHECK(p.user == "zhang");
    CHECK(p.host == "pc");
    CHECK(decode_text(p.additional, false) == QStringLiteral("你好:飞秋"));

    Packet bad;
    CHECK(!parse("", bad));                    // 空
    CHECK(!parse("1:2:3", bad));               // 段数不足
    CHECK(!parse("a:b:c:d:e", bad));           // ver 非十进制
    CHECK(!parse("1:2:u:h:zz:", bad));         // 命令非十六进制
    CHECK(!parse("{\"magic\":\"x\"}", bad));   // memex JSON 不是 IPMSG 报文

    // GB18030 往返；emoji 编不出→UTF-8 兜底（调用方带 UTF8OPT）
    bool used_utf8 = true;
    const QByteArray body = encode_text(QStringLiteral("中文消息"), used_utf8);
    CHECK(!used_utf8);
    CHECK(decode_text(body, false) == QStringLiteral("中文消息"));
    encode_text(QString::fromUtf8("emoji \xF0\x9F\x98\x80"), used_utf8);
    CHECK(used_utf8);
  }

  // —— B. mock 飞秋对拨（发现服务层；绑定次序：mock 先、memex 后） ——
  MockFeiq feiq;
  CHECK(feiq.bind(port));

  DiscoveryOptions opts;
  opts.port = port;
  opts.announce_interval_ms = 300;
  opts.peer_timeout_ms = 60000; // 在线表退出只走 BR_EXIT 腿，排除超时巡检干扰
  opts.sweep_interval_ms = 1000;
  opts.ipmsg_enabled = true;

  DiscoveryService a("dev-A", "memex-host");
  CHECK(a.start(opts));

  const std::string feiq_id = "ipmsg:feiquser@FEIQPC";
  const QString emoji_text = QString::fromUtf8("emoji \xF0\x9F\x98\x80");

  // B1 在线互见：memex BR_ENTRY 广播＝飞秋侧可见（host 恒 memex 标记）
  CHECK(wait_until([&] { return feiq.br_from_memex_ > 0; }, 8000));

  // B2 飞秋 BR_ENTRY 入列：memex 回 ANSENTRY（协议握手）＋建行（昵称
  //    GB18030 解出；飞秋对端无 TCP 口）
  feiq.send_br_entry();
  CHECK(wait_until([&] { return feiq.ans_from_memex_ > 0; }, 8000));
  CHECK(wait_until([&] { return has_peer(a, feiq_id); }, 8000));
  {
    bool name_ok = false;
    for (const Peer& p : a.peers()) {
      if (p.device_id == feiq_id) {
        name_ok = p.name == QStringLiteral("飞秋用户").toStdString() &&
                  p.tcp_port == 0 && !p.address.isNull();
      }
    }
    CHECK(name_ok);
  }

  // B3 飞秋→memex 单聊：GB18030 消息体解出；memex 回 RECVMSG（原包号）
  bool recv_seen = false;
  QString recv_text;
  QObject::connect(&a, &DiscoveryService::ipmsg_received, &a,
                   [&](const std::string&, const QString& text, qint64) {
                     recv_seen = true;
                     recv_text = text;
                   });
  const quint32 mock_msg_no = ++feiq.no_;
  feiq.send_sendmsg(mock_msg_no, gb(QStringLiteral("你好，来自飞秋")));
  CHECK(wait_until([&] { return recv_seen; }, 8000));
  CHECK(recv_text == QStringLiteral("你好，来自飞秋"));
  CHECK(wait_until([&] { return feiq.recv_from_memex_ > 0; }, 8000));
  CHECK(feiq.last_ack_no_ == mock_msg_no); // 回执＝原包号

  // B4 memex→飞秋 单聊：SENDMSG（GB18030）＋SEND_CHECKOPT；mock 回
  //    RECVMSG 后 ipmsg_acked 按同号上抛（no＝调用方序号原样返回）
  bool acked_seen = false;
  quint32 acked_no = 0;
  QObject::connect(&a, &DiscoveryService::ipmsg_acked, &a,
                   [&](quint32 no) {
                     acked_seen = true;
                     acked_no = no;
                   });
  const quint32 sent_no =
      a.send_ipmsg_text(feiq_id, QStringLiteral("来自 memex"), 777);
  CHECK(sent_no == 777);
  CHECK(wait_until([&] { return feiq.send_from_memex_ > 0; }, 8000));
  CHECK(feiq.last_text_ == QStringLiteral("来自 memex"));
  CHECK(!feiq.last_utf8opt_); // 中文走 GB18030，不带 UTF8OPT
  CHECK(wait_until([&] { return acked_seen; }, 8000));
  CHECK(acked_no == 777);

  // B5 emoji：GB18030 编不出→UTF-8 兜底且置 UTF8OPT，对端按位解出
  a.send_ipmsg_text(feiq_id, emoji_text, 778);
  CHECK(wait_until([&] { return feiq.last_text_ == emoji_text; }, 8000));
  CHECK(feiq.last_utf8opt_);

  // B6 退出广播：BR_EXIT 后对端消失（mock 不再主动 BR_ENTRY，memex 宣告
  //    无应答不会把对端拉回在线表）
  bool left_seen = false;
  QObject::connect(&a, &DiscoveryService::peerLeft, &a,
                   [&](const std::string& id) {
                     if (id == feiq_id) left_seen = true;
                   });
  feiq.send_br_exit();
  CHECK(wait_until([&] { return left_seen && !has_peer(a, feiq_id); }, 8000));

  // B7 关断静默：开关关＝IPMSG 报文不再收发（BR_ENTRY 无应答、不入列）
  a.set_ipmsg_enabled(false);
  const int ans_before = feiq.ans_from_memex_;
  feiq.send_br_entry();
  QElapsedTimer settle;
  settle.start();
  while (settle.elapsed() < 1200) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(10);
  }
  CHECK(feiq.ans_from_memex_ == ans_before);
  CHECK(!has_peer(a, feiq_id));

  // B8 memex 标记不重复建行（重开开关再测）：两 memex 实例互见仍走 JSON
  //    通道（设备号行），IPMSG 侧不因 host=memex 的 BR_ENTRY 建第二行
  //    （双实例均为广播腿，单播归属不受绑定次序影响）
  a.set_ipmsg_enabled(true);
  {
    DiscoveryService b("dev-B", "memex-host2");
    CHECK(b.start(opts));
    CHECK(wait_until(
        [&] { return has_peer(a, "dev-B") && has_peer(b, "dev-A"); }, 8000));
    bool no_ipmsg_row = true;
    for (const Peer& p : a.peers()) {
      if (p.device_id.rfind("ipmsg:", 0) == 0) no_ipmsg_row = false;
    }
    CHECK(no_ipmsg_row);
    b.stop();
  }
  a.stop();

  // —— C. 引擎全链：send_text 分流→GB18030 下行→回执关单；收文落库上抛
  //      （引擎在 dev-A/dev-B 停止后启动＝同口最后绑定者，单播归引擎） ——
  {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    DirectEngine engine("dev-C", tmp.filePath(QStringLiteral("c.db")));
    CHECK(engine.start());
    engine.set_ipmsg_enabled(true);

    bool peers_changed = false;
    QObject::connect(&engine, &DirectEngine::peers_changed, &engine,
                     [&] { peers_changed = true; });
    feiq.send_br_entry();
    CHECK(wait_until([&] { return engine.has_peer(feiq_id); }, 8000));
    CHECK(wait_until([&] { return peers_changed; }, 8000));

    bool delivered_ok = false;
    quint64 delivered_seq = 0;
    QObject::connect(&engine, &DirectEngine::text_delivered, &engine,
                     [&](quint64 seq, bool ok) {
                       delivered_seq = seq;
                       delivered_ok = ok;
                     });
    const quint64 seq = engine.send_text(feiq_id, "引擎链路");
    CHECK(seq > 0);
    CHECK(wait_until(
        [&] { return feiq.last_text_ == QStringLiteral("引擎链路"); }, 8000));
    CHECK(wait_until([&] { return delivered_ok && delivered_seq == seq; },
                     8000));

    bool msg_seen = false;
    QString msg_text;
    QObject::connect(&engine, &DirectEngine::message_received, &engine,
                     [&](const QString& from, const QString& text, qint64, quint64) {
                       if (from == QString::fromStdString(feiq_id)) {
                         msg_seen = true;
                         msg_text = text;
                       }
                     });
    feiq.send_sendmsg(++feiq.no_, gb(QStringLiteral("mock 到引擎")));
    CHECK(wait_until([&] { return msg_seen; }, 8000));
    CHECK(msg_text == QStringLiteral("mock 到引擎"));
    // 收发都落本地库（source=direct，与 memex 直连消息同径）
    const auto history = engine.history(QString::fromStdString(feiq_id));
    bool has_out = false, has_in = false, all_direct = true;
    for (const auto& m : history) {
      if (m.text == "引擎链路") has_out = true;
      if (m.text == "mock 到引擎") has_in = true;
      if (m.source != "direct") all_direct = false;
    }
    CHECK(has_out && has_in && all_direct);

    engine.stop();
  }

  if (g_failures == 0) {
    qInfo("ipmsg tests: all passed");
    return 0;
  }
  qCritical("ipmsg tests: %d failure(s)", g_failures);
  return 1;
}
