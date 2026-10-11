#include "discovery.hpp"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QDebug>

namespace memex::client {

namespace {
// 飞秋在线表识别标记：本端 IPMSG 头部 host 恒填此值——收到 host=memex
// 的 IPMSG 包即对端是另一 memex 实例（它已由 JSON 发现通道登记），IPMSG
// 侧不再建行，防同机互见重复行
constexpr const char* kIpmsgMemexHost = "memex";
} // namespace

DiscoveryService::DiscoveryService(std::string device_id, std::string device_name,
                                   QObject* parent)
    : QObject(parent), device_id_(std::move(device_id)),
      device_name_(std::move(device_name)) {
  announce_timer_.setTimerType(Qt::PreciseTimer);
  sweep_timer_.setTimerType(Qt::CoarseTimer);
  connect(&announce_timer_, &QTimer::timeout, this, &DiscoveryService::announce);
  connect(&sweep_timer_, &QTimer::timeout, this, &DiscoveryService::sweep);
}

DiscoveryService::~DiscoveryService() { stop(); }

bool DiscoveryService::start(const DiscoveryOptions& opts) {
  if (running_) return true;
  opts_ = opts;
  ipmsg_enabled_ = opts.ipmsg_enabled;
  refresh_ipmsg_user();

  // ShareAddress：同机多实例共享 2425（验收要求同机双实例互现）
  const auto bind_mode = QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint;
  if (!socket_.bind(QHostAddress::AnyIPv4, opts_.port, bind_mode)) {
    qWarning() << "[直连发现] 绑定 UDP" << opts_.port << "失败：" << socket_.errorString();
    return false;
  }
  connect(&socket_, &QUdpSocket::readyRead, this, &DiscoveryService::on_ready_read,
          Qt::UniqueConnection);

  running_ = true;
  announce(); // 立即宣告一轮
  announce_timer_.start(opts_.announce_interval_ms);
  sweep_timer_.start(opts_.sweep_interval_ms);
  qDebug() << "[直连发现] 启动，UDP" << opts_.port << "device=" << device_id_.c_str();
  return true;
}

void DiscoveryService::stop() {
  if (!running_) return;
  announce_timer_.stop();
  sweep_timer_.stop();
  socket_.close();
  peers_.clear();
  running_ = false;
  qDebug() << "[直连发现] 停止";
}

void DiscoveryService::set_tcp_port(quint16 port) { tcp_port_ = port; }

void DiscoveryService::set_account(const std::string& account) {
  if (account_ == account) return;
  account_ = account;
  refresh_ipmsg_user(); // 飞秋在线表名随登录态变（账号优先，退化设备名）
  // 登出／登录立即补一轮宣告——对端据此判定跨态（T4.2）
  if (running_) announce();
}

void DiscoveryService::set_ipmsg_enabled(bool on) {
  if (ipmsg_enabled_ == on) return;
  ipmsg_enabled_ = on;
  if (running_ && on) announce_ipmsg(); // 开即广播，飞秋在线表即时互见
}

void DiscoveryService::refresh_ipmsg_user() {
  const std::string name = account_.empty() ? device_name_ : account_;
  bool used_utf8 = false;
  ipmsg_user_ = ipmsg::encode_text(QString::fromStdString(name), used_utf8);
  if (ipmsg_user_.isEmpty()) ipmsg_user_ = kIpmsgMemexHost; // 空名兜底
  ipmsg_host_ = kIpmsgMemexHost;
}

QList<Peer> DiscoveryService::peers() const {
  QList<Peer> out;
  out.reserve(static_cast<int>(peers_.size()));
  for (const auto& [_, p] : peers_) out.push_back(p);
  return out;
}

void DiscoveryService::announce() {
  if (!running_) return;

  QJsonObject obj;
  obj.insert(QStringLiteral("magic"), QLatin1String(kDiscoveryMagic));
  obj.insert(QStringLiteral("v"), kDiscoveryVersion);
  obj.insert(QStringLiteral("device_id"), QString::fromStdString(device_id_));
  obj.insert(QStringLiteral("name"), QString::fromStdString(device_name_));
  obj.insert(QStringLiteral("tcp_port"), static_cast<int>(tcp_port_));
  if (!account_.empty()) {
    // T4.2：登录端携带账号标识——仅作对端显示与跨态判定，不参与路由
    obj.insert(QStringLiteral("account"), QString::fromStdString(account_));
  }
  obj.insert(QStringLiteral("ts"),
             QDateTime::currentMSecsSinceEpoch());

  const QByteArray datagram = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  socket_.writeDatagram(datagram, QHostAddress::Broadcast, opts_.port);
  announce_ipmsg();
}

// 飞秋在线表靠 BR_ENTRY 广播＋对 BR_ENTRY 的 ANSENTRY 应答成立，周期
// 重发维持在线（与 JSON 宣告同拍）；BR_ENTRY 协议禁 UTF8OPT，身份恒
// 本地编码（GBK/CP936）
void DiscoveryService::announce_ipmsg() {
  if (!running_ || !ipmsg_enabled_) return;
  const QByteArray raw =
      ipmsg::encode(ipmsg_next_no(), ipmsg::kBrEntry, ipmsg_user_,
                    ipmsg_host_, QByteArray());
  socket_.writeDatagram(raw, QHostAddress::Broadcast, opts_.port);
}

void DiscoveryService::set_filter(std::function<bool(const QHostAddress&)> f) {
  filter_ = std::move(f);
}

void DiscoveryService::on_ready_read() {
  while (socket_.hasPendingDatagrams()) {
    const QNetworkDatagram dgram = socket_.receiveDatagram();
    const QByteArray data = dgram.data();

    // 黑名单段命中＝不响应（用户令 2026-10-08 ⑤：段+掩码匹配拒连）
    if (filter_ && !filter_(dgram.senderAddress())) continue;

    // 畸形防御：长度越界直接丢弃
    if (data.isEmpty() || data.size() > 2048) continue;

    // 同口分流（飞秋兼容）：'{' 开头＝memex JSON 宣告；数字开头＝IPMSG
    // 报文（ver 区十进制数字起头）；其余形态丢弃
    const char first = data.at(0);
    if (first == '{') {
      QJsonParseError parse_error{};
      const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
      if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) continue;

      const QJsonObject obj = doc.object();
      if (obj.value(QStringLiteral("magic")).toString() !=
          QLatin1String(kDiscoveryMagic)) {
        continue;
      }
      if (obj.value(QStringLiteral("v")).toInt() != kDiscoveryVersion) continue;

      const std::string device_id =
          obj.value(QStringLiteral("device_id")).toString().toStdString();
      if (device_id.empty() || device_id == device_id_) continue; // 忽略自己

      Peer peer;
      peer.device_id = device_id;
      peer.name = obj.value(QStringLiteral("name")).toString().toStdString();
      peer.tcp_port = static_cast<quint16>(
          qBound(0, obj.value(QStringLiteral("tcp_port")).toInt(), 65535));
      peer.address = dgram.senderAddress();
      peer.last_seen_ms =
          static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
      peer.account = obj.value(QStringLiteral("account")).toString().toStdString();

      const auto it = peers_.find(device_id);
      if (it == peers_.end()) {
        peers_.emplace(device_id, peer);
        emit peerJoined(peer);
      } else {
        const bool account_changed = it->second.account != peer.account;
        it->second = peer;
        if (account_changed) emit peerUpdated(peer); // 登录态变化（T4.2 跨态判定）
      }
    } else if (first >= '0' && first <= '9') {
      handle_ipmsg(dgram.senderAddress(), dgram.senderPort(), data);
    }
  }
}

// 飞秋/IPMSG 收包（开关关＝静默丢；host=memex＝另一 memex，交 JSON 通道）。
// 握手口径：BR_ENTRY 必回 ANSENTRY（不回对方在线表就没有我）；SENDMSG 必
// 回 RECVMSG（原包号回填附加区，对方据此判送达）
void DiscoveryService::handle_ipmsg(const QHostAddress& sender,
                                    quint16 sender_port,
                                    const QByteArray& data) {
  if (!ipmsg_enabled_) return;
  ipmsg::Packet p;
  if (!ipmsg::parse(data, p) || p.ver != ipmsg::kVersion) return;
  const bool is_memex = p.host == kIpmsgMemexHost;
  const quint32 cmd = ipmsg::command_of(p.cmd);

  switch (cmd) {
    case ipmsg::kBrEntry: {
      if (is_memex) return;
      ipmsg_upsert(sender, sender_port, p.user, p.host, p.additional);
      ipmsg_send(sender, sender_port,
                 ipmsg::encode(ipmsg_next_no(), ipmsg::kAnsEntry, ipmsg_user_,
                               ipmsg_host_, QByteArray()));
      break;
    }
    case ipmsg::kAnsEntry:
    case ipmsg::kBrAbsence:
      if (is_memex) return;
      ipmsg_upsert(sender, sender_port, p.user, p.host, p.additional);
      break;
    case ipmsg::kBrExit: {
      if (is_memex) return;
      const std::string id = ipmsg_id(p.user, p.host);
      const auto it = peers_.find(id);
      if (it != peers_.end()) {
        peers_.erase(it);
        emit peerLeft(id);
      }
      break;
    }
    case ipmsg::kSendMsg: {
      if (is_memex) return;
      ipmsg_upsert(sender, sender_port, p.user, p.host, p.user);
      // 回执先行（原包号回填）：对端界面的送达勾点靠这条
      ipmsg_send(sender, sender_port,
                 ipmsg::encode(ipmsg_next_no(), ipmsg::kRecvMsg, ipmsg_user_,
                               ipmsg_host_, QByteArray::number(p.no)));
      const QString text = ipmsg::decode_text(
          p.additional, (p.cmd & ipmsg::kUtf8Opt) != 0);
      emit ipmsg_received(ipmsg_id(p.user, p.host), text,
                          static_cast<qint64>(QDateTime::currentMSecsSinceEpoch()));
      break;
    }
    case ipmsg::kRecvMsg: {
      bool ok = false;
      const quint32 no = p.additional.toUInt(&ok);
      if (ok) emit ipmsg_acked(no);
      break;
    }
    default:
      break; // 未知命令（新版特性）静默忽略，不回包不崩
  }
}

// 收到即入列/刷新（新见=peerJoined，昵称变化=peerUpdated）；显示名按
// 飞秋本地编码解（GB18030——BR_ENTRY 身份禁 UTF8OPT）
void DiscoveryService::ipmsg_upsert(const QHostAddress& sender,
                                    quint16 sender_port, const QByteArray& user,
                                    const QByteArray& host,
                                    const QByteArray& nick) {
  const std::string id = ipmsg_id(user, host);
  if (id.empty()) return;
  const QString name = ipmsg::decode_text(nick.isEmpty() ? user : nick, false);
  const auto now = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
  const auto it = peers_.find(id);
  if (it == peers_.end()) {
    Peer peer;
    peer.device_id = id;
    peer.name = name.toStdString();
    peer.address = sender;
    peer.udp_port = sender_port;
    peer.last_seen_ms = now;
    peers_.emplace(id, peer);
    emit peerJoined(peer);
    return;
  }
  it->second.last_seen_ms = now;
  it->second.address = sender;
  it->second.udp_port = sender_port;
  if (it->second.name != name.toStdString()) {
    it->second.name = name.toStdString();
    emit peerUpdated(it->second);
  }
}

std::string DiscoveryService::ipmsg_id(const QByteArray& user,
                                       const QByteArray& host) const {
  if (user.isEmpty()) return {};
  QByteArray id;
  id += "ipmsg:";
  id += user;
  id += '@';
  id += host;
  return id.toStdString();
}

void DiscoveryService::ipmsg_send(const QHostAddress& addr, quint16 port,
                                  const QByteArray& raw) {
  if (!socket_.isValid() || addr.isNull()) return;
  socket_.writeDatagram(raw, addr, port);
}

quint32 DiscoveryService::send_ipmsg_text(const std::string& device_id,
                                          const QString& text, quint32 no) {
  if (!running_ || !ipmsg_enabled_ || no == 0) return 0;
  const auto it = peers_.find(device_id);
  if (it == peers_.end() || it->second.address.isNull()) return 0;
  // 与调用方发送序号共用一条单调序列（回执按同号回填，界面送达态对齐）
  ipmsg_no_ = qMax(ipmsg_no_, no);
  bool used_utf8 = false;
  const QByteArray body = ipmsg::encode_text(text, used_utf8);
  quint32 cmd = ipmsg::kSendMsg | ipmsg::kSendCheckOpt;
  if (used_utf8) cmd |= ipmsg::kUtf8Opt;
  // 真飞秋收发同口 2425；mock/测试来源口即收口（upsert 时已记 udp_port）
  const quint16 port = it->second.udp_port != 0 ? it->second.udp_port : opts_.port;
  ipmsg_send(it->second.address, port,
             ipmsg::encode(no, cmd, ipmsg_user_, ipmsg_host_, body));
  return no;
}

void DiscoveryService::sweep() {
  if (!running_) return;
  const auto now = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
  const auto timeout = static_cast<quint64>(opts_.peer_timeout_ms);
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (now - it->second.last_seen_ms > timeout) {
      const std::string id = it->first;
      it = peers_.erase(it);
      emit peerLeft(id);
    } else {
      ++it;
    }
  }
}

} // namespace memex::client
