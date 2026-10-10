#include "collab_engine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QSysInfo>

#include <algorithm>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>
#include <core/local_store.hpp>

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace memex::client {

using memex::protocol::Message;
using memex::protocol::MsgType;

namespace {

// machine-id 的两个常规落点（发行版差异）；都拿不到则退化为主机名。
QString read_machine_id() {
  for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
    QFile f(QString::fromLatin1(path));
    if (f.open(QIODevice::ReadOnly)) {
      const QString id = QString::fromUtf8(f.readAll()).trimmed();
      if (!id.isEmpty()) return id;
    }
  }
  return QString{};
}

} // namespace

QString CollabEngine::device_fingerprint() {
  const QString seed = read_machine_id() + QChar('|') + device_name();
  return QString::fromLatin1(
      QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256)
          .toHex());
}

QString CollabEngine::device_name() { return QSysInfo::machineHostName(); }

CollabEngine::CollabEngine(QObject* parent) : QObject(parent) {
  socket_ = new QTcpSocket(this);
  connect(socket_, &QTcpSocket::connected, this, [this] { send_login_frame(); });
  connect(socket_, &QTcpSocket::readyRead, this, [this] {
    const QByteArray data = socket_->readAll();
    std::vector<std::string> payloads;
    const auto st = decoder_.feed(
        std::string_view(data.constData(), static_cast<std::size_t>(data.size())),
        payloads);
    if (st == memex::protocol::DecodeStatus::kZeroLength ||
        st == memex::protocol::DecodeStatus::kTooLarge) {
      qWarning() << "[协作] 非法帧，断开";
      socket_->abort();
      return;
    }
    for (const auto& p : payloads) handle_frame(QByteArray(p.data(), static_cast<int>(p.size())));
  });
  connect(socket_, &QTcpSocket::disconnected, this, [this] {
    const bool was_logged_in = logged_in_;
    const bool expected = kicking_ || manual_logout_;
    teardown(was_logged_in && !expected);
    if (was_logged_in && !expected) {
      emit connection_lost();
      schedule_reconnect();
    }
  });
  connect(socket_, &QTcpSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) {
            if (logged_in_) return;
            if (reconnecting_) {
              schedule_reconnect();
            } else {
              emit login_failed(QStringLiteral("无法连接服务器"));
            }
          });

  // 定时器连接（QTimer 对象，非指针）
  connect(&reconnect_timer_, &QTimer::timeout, this, [this] {
    reconnect_timer_.stop();
    qInfo() << "[协作] 自动重连" << host_ << ":" << port_;
    socket_->connectToHost(host_, port_);
  });
  reconnect_timer_.setSingleShot(true);

  connect(&heartbeat_timer_, &QTimer::timeout, this, [this] {
    if (heartbeat_missed_ >= heartbeat_max_missed_) {
      qWarning() << "[协作] 心跳超时，主动断开并转入重连";
      socket_->abort();
      return;
    }
    ++heartbeat_missed_;
    Message ping;
    ping.set_type(MsgType::PING);
    ping.set_from(account_.toStdString());
    ping.set_to("server");
    ping.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
    send_frame(ping);
  });

  connect(&delivery_timer_, &QTimer::timeout, this, [this] { check_delivery_timeouts(); });
}

CollabEngine::~CollabEngine() = default;

void CollabEngine::set_heartbeat(int interval_ms, int max_missed) {
  heartbeat_interval_ms_ = interval_ms;
  heartbeat_max_missed_ = max_missed;
}

void CollabEngine::attach_store(LocalStore* store) { store_ = store; }

void CollabEngine::login(const QString& host, quint16 port,
                         const QString& account, const QString& password) {
  reconnect_timer_.stop();
  teardown(false);
  manual_logout_ = false;
  reconnecting_ = false;
  reconnect_backoff_ms_ = 1000;
  account_ = account;
  password_ = password;
  host_ = host;
  port_ = port;
  qInfo() << "[协作] 连接" << host << ":" << port << "（" << account << "）";
  socket_->connectToHost(host, port);
}

void CollabEngine::logout() {
  if (!logged_in_) return;
  manual_logout_ = true;
  Message m;
  m.set_type(MsgType::LOGOUT);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  send_frame(m);
  kicking_ = true;
  logged_in_ = false;
  socket_->disconnectFromHost();
}

namespace {
// 服务端 msg_id 派生式（session.cpp 同式）：sha256_hex(from + ":" + seq)。
// 发出即回填本地行，回执通知（DELIVER_NOTICE/READ_NOTICE）按它命中。
QString derive_msg_id(const QString& account, quint64 seq) {
  return CollabEngine::msg_id_for(account, seq);
}
// 需求批⑧ 补齐行（MESSAGE_PAGE 落库）的本地序号保留段：线上 seq 是
// 发送方小整数计数器，补齐行序号从 2^62 起计——两段永不相交，杜绝
// 补齐行占位把同 (from_id, seq) 的后续实时投递行静默 IGNORE 丢行。
constexpr std::uint64_t kGapSeqBase = 1ULL << 62;
} // namespace

QString CollabEngine::msg_id_for(const QString& account, quint64 seq) {
  return QString::fromUtf8(QCryptographicHash::hash(
      (account + QStringLiteral(":") + QString::number(seq)).toUtf8(),
      QCryptographicHash::Sha256).toHex());
}

quint64 CollabEngine::send_text(const QString& to, const QString& text) {
  if (to.isEmpty()) return 0;
  const quint64 seq = next_seq_++;
  PendingSend p;
  p.to = to.toStdString();
  p.text = text.toStdString();
  p.seq = seq;
  p.ts_ms = QDateTime::currentMSecsSinceEpoch();

  if (logged_in_) {
    Message m;
    m.set_type(MsgType::TEXT);
    m.set_seq(seq);
    m.set_from(account_.toStdString());
    m.set_to(p.to);
    m.set_ts_ms(p.ts_ms);
    m.mutable_text()->set_text(p.text);
    send_frame(m);
    p.sent_at_ms = QDateTime::currentMSecsSinceEpoch();
    inflight_.insert(seq, p);
    if (!delivery_timer_.isActive()) delivery_timer_.start(500);
  } else if (reconnecting_) {
    // T2.5 断线中断期：本地暂存（不判失败），恢复后按原 seq 补传；
    // 服务端按 msg_id=sha256(from:seq) 去重——此前若已受理也不会重复归档。
    pending_reconnect_.push_back(p);
    qInfo() << "[协作] 断线中断期消息已暂存（待补传）：" << seq;
  } else {
    return 0; // 从未连接成功，不属于中断补传范围
  }

  if (store_) {
    memex::client::StoredMessage sm;
    sm.seq = seq;
    sm.peer = p.to;
    sm.from = account_.toStdString();
    sm.to = p.to;
    sm.ts_ms = p.ts_ms;
    sm.text = p.text;
    sm.source = "collab";
    // 需求批⑦：发出即回填 msg_id（与服务端同式派生 sha256(account:seq)，
    // 受理回执与重投去重键一致）——送达/已读通知按它命中本地行
    sm.msg_id = derive_msg_id(account_, seq).toStdString();
    // 平台-9 Sync State：入发送管线即记状态（在线直发=SENDING，
    // 断线暂存=PENDING）；受理/放弃由回执与 teardown 推移
    sm.sync_state = logged_in_ ? "SENDING" : "PENDING";
    store_->append(sm);
  }
  return seq;
}

// 振屏（需求批⑥，协作单聊）：空体 NUDGE——与文本同走至少一次管线
//（受理回执/超时重发/断线补传按 p.nudge 重建同型帧），本地落 "[振屏]"
// 标记行；服务端归档留痕＋在线即投，不进离线补投。
quint64 CollabEngine::send_nudge(const QString& to) {
  if (to.isEmpty()) return 0;
  const quint64 seq = next_seq_++;
  PendingSend p;
  p.to = to.toStdString();
  p.nudge = true;
  p.seq = seq;
  p.ts_ms = QDateTime::currentMSecsSinceEpoch();

  if (logged_in_) {
    Message m;
    m.set_type(MsgType::NUDGE);
    m.mutable_nudge(); // 空 message 进 oneof 须显式置位（需求批⑥）
    m.set_seq(seq);
    m.set_from(account_.toStdString());
    m.set_to(p.to);
    m.set_ts_ms(p.ts_ms);
    send_frame(m);
    p.sent_at_ms = QDateTime::currentMSecsSinceEpoch();
    inflight_.insert(seq, p);
    if (!delivery_timer_.isActive()) delivery_timer_.start(500);
  } else if (reconnecting_) {
    pending_reconnect_.push_back(p);
  } else {
    return 0;
  }

  if (store_) {
    memex::client::StoredMessage sm;
    sm.seq = seq;
    sm.peer = p.to;
    sm.from = account_.toStdString();
    sm.to = p.to;
    sm.ts_ms = p.ts_ms;
    sm.text = "[振屏]";
    sm.source = "collab";
    sm.sync_state = logged_in_ ? "SENDING" : "PENDING"; // 受理回执推移
    store_->append(sm);
  }
  return seq;
}

void CollabEngine::recall_text(const QString& to, const QString& msg_id) {
  if (!logged_in_ || to.isEmpty() || msg_id.isEmpty()) return;
  Message m;
  m.set_type(MsgType::RECALL);
  m.set_from(account_.toStdString());
  m.set_to(to.toStdString());
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  m.mutable_recall()->set_msg_id(msg_id.toStdString());
  send_frame(m);
}

void CollabEngine::query_org() {
  if (!logged_in_) {
    qWarning() << "[协作] 未登录，组织架构不可查";
    return;
  }
  Message m;
  m.set_type(MsgType::ORG_QUERY);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  send_frame(m);
}

void CollabEngine::fav_query() {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::FAV_QUERY);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  send_frame(m);
}

void CollabEngine::fav_cmd(const QString& op, const QString& peer) {
  if (!logged_in_ || peer.isEmpty() || op.isEmpty()) return;
  Message m;
  m.set_type(MsgType::FAV_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_fav_cmd();
  c->set_op(op.toStdString());
  c->set_peer(peer.toStdString());
  c->set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  send_frame(m);
}

// 个性签名设置/清除（需求批⑪）：服务端受理后 PROFILE_RESULT 回执
void CollabEngine::set_signature(const QString& signature) {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::PROFILE_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_profile_cmd();
  c->set_op("set_signature");
  c->set_signature(signature.toStdString());
  send_frame(m);
}

// —— T4.1 群聊 ——

void CollabEngine::create_group(const QString& name, const QStringList& members) {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::GROUP_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_group_cmd();
  c->set_op("create");
  c->set_name(name.toStdString());
  for (const QString& a : members) c->add_members(a.toStdString());
  send_frame(m);
}

void CollabEngine::invite_group(quint64 group_id, const QStringList& members) {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::GROUP_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_group_cmd();
  c->set_op("invite");
  c->set_group_id(group_id);
  for (const QString& a : members) c->add_members(a.toStdString());
  send_frame(m);
}

void CollabEngine::leave_group(quint64 group_id) {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::GROUP_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_group_cmd();
  c->set_op("leave");
  c->set_group_id(group_id);
  send_frame(m);
}

void CollabEngine::announce_group(quint64 group_id, const QString& announcement) {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::GROUP_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_group_cmd();
  c->set_op("announce");
  c->set_group_id(group_id);
  c->set_announcement(announcement.toStdString());
  send_frame(m);
}

void CollabEngine::announce_history(quint64 group_id) {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::GROUP_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_group_cmd();
  c->set_op("announce_history");
  c->set_group_id(group_id);
  send_frame(m);
}

void CollabEngine::query_groups() {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::GROUP_QUERY);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  send_frame(m);
}

// 跨态会话日志（T4.2）：登录端上报与未登录设备的会话起止（时间/双方/时长，
// 无内容字段）。start 不带 ended_ms，end 带两端时刻——服务端按 (账号, 设备,
// 建立时刻) 闭环最早一条未结束记录。
void CollabEngine::cross_log(const QString& op, const QString& peer_device,
                             const QString& peer_name, qint64 started_ms,
                             qint64 ended_ms) {
  if (!logged_in_ || peer_device.isEmpty() || started_ms <= 0) return;
  Message m;
  m.set_type(MsgType::CROSS_LOG);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_cross_log();
  c->set_op(op.toStdString());
  c->set_peer_device(peer_device.toStdString());
  c->set_peer_name(peer_name.toStdString());
  c->set_started_ms(started_ms);
  if (op == QStringLiteral("end")) c->set_ended_ms(ended_ms);
  send_frame(m);
}

// —— T4.3 已读回执与在线状态 ——

void CollabEngine::mark_read(const QString& msg_id) {
  if (!logged_in_ || msg_id.isEmpty()) return;
  Message m;
  m.set_type(MsgType::READ);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  m.mutable_read()->set_msg_id(msg_id.toStdString());
  send_frame(m);
}

void CollabEngine::query_receipts(const QStringList& msg_ids) {
  if (!logged_in_ || msg_ids.isEmpty()) return;
  Message m;
  m.set_type(MsgType::RECEIPT_QUERY);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* q = m.mutable_receipt_query();
  for (const QString& id : msg_ids) {
    if (id.isEmpty()) continue;
    q->add_msg_ids(id.toStdString());
  }
  send_frame(m);
}

// 会话历史按日期范围查询（需求批⑧）：回包落本地索引后经 history_received
// 通知——服务端归档比本地多的行（离线期/换机）就此补齐。
void CollabEngine::query_history(const QString& peer, qint64 from_ms,
                                 qint64 until_ms) {
  if (!logged_in_ || peer.isEmpty()) return;
  Message m;
  m.set_type(MsgType::MESSAGE_QUERY);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* q = m.mutable_message_query();
  q->set_peer(peer.toStdString());
  q->set_from_ms(from_ms);
  q->set_until_ms(until_ms);
  send_frame(m);
}

void CollabEngine::query_presence() {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::PRESENCE_QUERY);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  send_frame(m);
}

// 平台-10 直连文件旁路授权查询：文件不经服务器，判权必须经服务器。
// 未登录＝服务端不可达域，fail-closed 本地即拒（权限不能绕开服务器，
// 与文本降级可用不同——文本是归档面，文件是权限面）。
void CollabEngine::file_authz(quint64 req, const QString& to, quint64 size,
                              const QString& name, const QString& sha256,
                              bool forward) {
  if (!logged_in_) {
    emit file_authz_result(req, false,
                           QStringLiteral("deny:server-unreachable"), false);
    return;
  }
  Message m;
  m.set_type(MsgType::FILE_AUTHZ);
  m.set_seq(req); // 关联号原样回带
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* fa = m.mutable_file_authz();
  fa->set_to(to.toStdString());
  fa->set_size(size);
  fa->set_name(name.toStdString());
  fa->set_sha256(sha256.toStdString());
  fa->set_forward(forward);
  send_frame(m);
}

void CollabEngine::send_frame(const Message& msg) {
  const std::string frame = memex::protocol::encode(msg);
  socket_->write(QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
}

void CollabEngine::send_login_frame() {
  Message m;
  m.set_type(MsgType::LOGIN);
  m.set_from(device_name().toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* in = m.mutable_login();
  in->set_account(account_.toStdString());
  in->set_password(password_.toStdString());
  in->set_device_fingerprint(device_fingerprint().toStdString());
  in->set_device_kind("desktop");
  in->set_device_name(device_name().toStdString());
  in->set_client_version(MEMEX_VERSION);
  send_frame(m);
}

void CollabEngine::handle_frame(const QByteArray& payload) {
  Message msg;
  try {
    msg = memex::protocol::decode_payload(
        std::string_view(payload.constData(), static_cast<std::size_t>(payload.size())));
  } catch (const memex::protocol::ProtocolError& e) {
    qWarning() << "[协作] 载荷解析失败：" << e.what();
    return;
  }

  switch (msg.type()) {
  case MsgType::LOGIN_RESULT: {
    if (!msg.has_login_result()) return;
    const auto& r = msg.login_result();
    if (r.ok()) {
      logged_in_ = true;
      reconnect_timer_.stop();
      reconnect_backoff_ms_ = 1000;
      start_heartbeat();
      const QString display =
          QString::fromStdString(r.display_name().empty() ? account_.toStdString()
                                                          : r.display_name());
      qInfo() << "[协作] 登录成功：" << account_ << "（" << display << "）";
      if (reconnecting_) {
        reconnecting_ = false;
        emit reconnected();
      } else {
        emit logged_in(account_, display);
        fav_query(); // T4.5 常用联系人随登录自动拉取（换机保留即此体现）
      }
      // 平台-9 重启恢复：seq 计数从本地库续位——新进程若从 1 重数，既撞
      // 本地 UNIQUE(from_id,seq)，又因 msg_id=sha256(from:seq) 与旧消息
      // 同 ID 被服务端去重误吞（BUG-007 已修：服务端撞 id 按内容比对消歧
      // ——同内容幂等一行、异内容换盐重排两行齐；本地 UNIQUE 对重排新消息
      // 的拦截为 §4.2 升级窗口残差，本批未落）
      if (store_) {
        next_seq_ = static_cast<quint64>(
            store_->next_local_seq(account_.toStdString()));
      }
      // T2.5 重连补传 + 平台-9 重启恢复：每次登录成功都回收补传队列与库中
      // 遗留的 PENDING/SENDING 行（服务端按 msg_id 去重归档，重发幂等）
      flush_pending_sends();
    } else {
      reconnecting_ = false;
      reconnect_timer_.stop();
      qInfo() << "[协作] 登录失败：" << QString::fromStdString(r.reason());
      emit login_failed(QString::fromStdString(r.reason()));
      socket_->disconnectFromHost();
    }
    break;
  }
  case MsgType::KICK: {
    const auto& k = msg.has_kick() ? msg.kick()
                                   : memex::protocol::v1::Kick{};
    kicking_ = true;
    reconnect_timer_.stop();
    reconnecting_ = false;
    qInfo() << "[协作] 被顶替下线：" << QString::fromStdString(k.reason());
    emit kicked(QString::fromStdString(k.reason()),
                QString::fromStdString(k.replaced_by()));
    logged_in_ = false;
    socket_->disconnectFromHost();
    break;
  }
  case MsgType::PONG:
    heartbeat_missed_ = 0;
    break;
  case MsgType::TEXT:
    if (msg.has_text()) handle_text(msg);
    break;
  case MsgType::NUDGE: {
    // 振屏（需求批⑥）：本地落 "[振屏]" 标记行（服务端已归档）后上抛
    // 界面层抖窗；按 msg_id 去重（补投/重发幂等）。无离线补投——
    // 到达即在线，无需回 ACK 清队列。
    if (!msg.has_nudge()) break;
    if (msg.to().rfind("group:", 0) == 0) break; // 群振屏不支持（服务端同裁）
    bool inserted = true;
    if (store_) {
      memex::client::StoredMessage sm;
      sm.seq = msg.seq();
      sm.peer = msg.from();
      sm.from = msg.from();
      sm.to = msg.to();
      sm.ts_ms = msg.ts_ms() > 0 ? msg.ts_ms()
                                 : QDateTime::currentMSecsSinceEpoch();
      sm.text = "[振屏]";
      sm.source = "collab";
      sm.msg_id = msg.msg_id();
      sm.sync_state = "ARCHIVED"; // 服务端已归档（平台-9 接收方向）
      store_->append(sm, &inserted);
    }
    if (inserted) {
      emit nudge_received(QString::fromStdString(msg.from()),
                          msg.ts_ms() > 0 ? msg.ts_ms()
                                          : QDateTime::currentMSecsSinceEpoch());
    }
    break;
  }
  case MsgType::NOTICE:
    if (msg.has_notice()) handle_notice(msg);
    break;
  case MsgType::ACK:
    handle_ack(msg);
    break;
  case MsgType::FILE_AUTHZ_RESULT: {
    // 平台-10：授权裁决上抛，req 原样回带给发起面（直连引擎关单）
    if (!msg.has_file_authz_result()) return;
    const auto& r = msg.file_authz_result();
    emit file_authz_result(static_cast<quint64>(msg.seq()), r.allowed(),
                           QString::fromStdString(r.reason()),
                           r.forwardable());
    break;
  }
  case MsgType::RECALL: {
    const std::string target = msg.has_recall() ? msg.recall().msg_id() : std::string{};
    if (!target.empty() && store_) {
      store_->mark_recalled(target);
    }
    emit message_recalled(QString::fromStdString(msg.from()),
                          QString::fromStdString(target));
    break;
  }
  case MsgType::FAV_DATA: {
    // 常用联系人全量下发（T4.5）：JSON 交界面层
    if (!msg.has_fav_data()) return;
    nlohmann::json j = nlohmann::json::array();
    for (const auto& e : msg.fav_data().entries()) {
      j.push_back({{"peer", e.peer()},
                   {"starred", e.starred()},
                   {"last_ms", e.last_ms()}});
    }
    emit fav_received(QString::fromStdString(j.dump()));
    break;
  }
  case MsgType::PROFILE_RESULT: {
    // 资料命令回执（需求批⑪）：交界面层提示（保存成功/失败原因）
    if (!msg.has_profile_result()) return;
    const auto& r = msg.profile_result();
    emit profile_result(r.ok(), QString::fromStdString(r.reason()),
                        QString::fromStdString(r.op()));
    break;
  }
  case MsgType::ORG_DATA: {
    // 组织架构下发 → JSON 交给界面层（T3.1：管理端维护，客户端生效展示）
    if (!msg.has_org_data()) return;
    nlohmann::json j;
    j["departments"] = nlohmann::json::array();
    for (const auto& d : msg.org_data().departments()) {
      j["departments"].push_back({{"path", d.path()}});
    }
    j["members"] = nlohmann::json::array();
    for (const auto& m : msg.org_data().members()) {
      j["members"].push_back({{"account", m.account()},
                              {"display_name", m.display_name()},
                              {"title", m.title()},
                              {"department_path", m.department_path()},
                              {"manager", m.manager()},
                              {"role", m.role()},
                              {"signature", m.signature()},
                              // 需求批⑩ 在线时长：滚动窗并集秒数（界面侧展示）
                              {"online_day_s", m.online_day_s()},
                              {"online_week_s", m.online_week_s()},
                              {"online_month_s", m.online_month_s()},
                              // 需求批⑫ 头像：版本戳（0=未设置，字节走文件面）
                              {"avatar_ver", m.avatar_ver()}});
    }
    j["policies"] = nlohmann::json::array();
    for (const auto& p : msg.org_data().policies()) {
      j["policies"].push_back({{"department_path", p.department_path()},
                               {"allow_anonymous", p.allow_anonymous()},
                               {"allow_cross_state", p.allow_cross_state()},
                               {"new_device_approval", p.new_device_approval()},
                               {"allow_cross_dept_file", p.allow_cross_dept_file()},
                               {"allow_forward_file", p.allow_forward_file()}});
    }
    // 权限模型「群在组织架构可见」：群组清单随组织架构一并下发
    j["groups"] = nlohmann::json::array();
    for (const auto& g : msg.org_data().groups()) {
      nlohmann::json gj = {{"group_id", g.group_id()},
                           {"name", g.name()},
                           {"owner", g.owner()}};
      gj["members"] = nlohmann::json::array();
      for (const auto& m : g.members()) gj["members"].push_back(m);
      j["groups"].push_back(gj);
    }
    emit org_received(QString::fromStdString(j.dump()));
    break;
  }
  case MsgType::GROUP_RESULT: {
    // 群命令回执（T4.1）：失败理由上抛；成功操作顺带刷新群列表。
    // R24-1：announce_history 回执改走专用信号（不刷群列表）。
    if (!msg.has_group_result()) return;
    const auto& r = msg.group_result();
    if (r.op() == "announce_history") {
      nlohmann::json j = nlohmann::json::array();
      for (const auto& h : r.history()) {
        nlohmann::json hj;
        hj["editor"] = h.editor();
        hj["content"] = h.content();
        hj["ts_ms"] = h.ts_ms();
        j.push_back(std::move(hj));
      }
      emit announcement_history_received(
          r.group_id(), QString::fromStdString(j.dump()));
      break;
    }
    emit group_result(r.ok(), QString::fromStdString(r.reason()),
                      QString::fromStdString(r.op()), r.group_id());
    if (r.ok()) query_groups();
    break;
  }
  case MsgType::GROUP_DATA: {
    // 群列表（T4.1）：JSON 交给界面层，见 groups_received 注释
    if (!msg.has_group_data()) return;
    nlohmann::json j = nlohmann::json::array();
    for (const auto& g : msg.group_data().groups()) {
      nlohmann::json gj;
      gj["group_id"] = g.group_id();
      gj["name"] = g.name();
      gj["owner"] = g.owner();
      gj["announcement"] = g.announcement();
      gj["members"] = nlohmann::json::array();
      for (const auto& m : g.members()) gj["members"].push_back(m);
      j.push_back(std::move(gj));
    }
    emit groups_received(QString::fromStdString(j.dump()));
    break;
  }
  case MsgType::READ_NOTICE: {
    // 我发出的协作消息被已读（T4.3）：发送方视角的「对方已读」
    if (!msg.has_read_notice()) return;
    const auto& n = msg.read_notice();
    if (store_) store_->set_receipt(n.msg_id(), "read"); // 终态（只升不降）
    emit message_read(QString::fromStdString(n.msg_id()),
                      QString::fromStdString(n.reader()), n.read_ms());
    break;
  }
  case MsgType::DELIVER_NOTICE: {
    // 我发出的协作消息已送达对方客户端（需求批⑦ 送达级）
    if (!msg.has_deliver_notice()) return;
    const auto& n = msg.deliver_notice();
    if (store_) store_->set_receipt(n.msg_id(), "delivered");
    // 平台-9 发送侧归档推进：ACK=对方客户端已收取落库（接收方向
    // ARCHIVED 唯一入口之外，发送侧以服务端确认事件 DELIVER_NOTICE
    // 为触发面——SERVER_ACKED 留尾就此收口）
    if (store_) store_->set_sync_state_archived(n.msg_id());
    emit message_delivered(QString::fromStdString(n.msg_id()),
                           QString::fromStdString(n.delivered_to()),
                           n.delivered_ms());
    break;
  }
  case MsgType::RECEIPT_DATA: {
    // 回执态补查回包（需求批⑦）：打开会话时对齐离线期错过的通知
    if (!msg.has_receipt_data()) return;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : msg.receipt_data().entries()) {
      nlohmann::json o;
      o["msg_id"] = e.msg_id();
      o["delivered_to"] = nlohmann::json::array();
      for (const auto& d : e.delivered_to()) o["delivered_to"].push_back(d);
      o["readers"] = nlohmann::json::array();
      for (const auto& r : e.readers()) o["readers"].push_back(r);
      arr.push_back(std::move(o));
      // 台账对齐落本地（与实时通知同口径：已读压过送达；两项皆空不动；
      // 平台-9 发送侧归档推进同 DELIVER_NOTICE 触发面）
      if (store_ && e.readers_size() > 0) {
        store_->set_receipt(e.msg_id(), "read");
        store_->set_sync_state_archived(e.msg_id());
      } else if (store_ && e.delivered_to_size() > 0) {
        store_->set_receipt(e.msg_id(), "delivered");
        store_->set_sync_state_archived(e.msg_id());
      }
    }
    emit receipts_received(QString::fromStdString(arr.dump()));
    break;
  }
  case MsgType::MESSAGE_PAGE: {
    // 日期范围查询回包（需求批⑧）：逐条落本地索引——已在库的按 msg_id
    // 去重不计，缺行（离线期/换机漏收）就此补齐，信号带新增计数。
    if (!msg.has_message_page()) return;
    const auto& p = msg.message_page();
    const QString peer = QString::fromStdString(p.peer());
    int added = 0;
    if (store_) {
      for (const auto& e : p.messages()) {
        memex::client::StoredMessage sm;
        // 补齐行无线上 seq（服务端不存）：按发送方行空间取下一序号再
        // 平移到高位保留段——与线上小整数 seq 永不相交（撞 UNIQUE(from_id,
        // seq) 会把后续实时投递行静默 IGNORE 丢行）；去重靠 msg_id 唯一索引。
        sm.seq = kGapSeqBase + store_->next_local_seq(e.from());
        sm.peer = peer.toStdString();
        sm.from = e.from();
        sm.to = e.to();
        sm.ts_ms = e.ts_ms();
        sm.text = e.text();
        sm.source = "collab";
        sm.msg_id = e.msg_id();
        sm.sync_state = "ARCHIVED";
        bool inserted = false;
        store_->append(sm, &inserted);
        if (inserted) ++added;
      }
    }
    emit history_received(peer, p.from_ms(), p.until_ms(), added);
    break;
  }
  case MsgType::PRESENCE_DATA: {
    // 在线账号表（T4.3）：查询回执与登录/登出/互踢变更推送共用
    if (!msg.has_presence_data()) return;
    QStringList accounts;
    for (const auto& a : msg.presence_data().accounts()) {
      accounts.push_back(QString::fromStdString(a));
    }
    online_accounts_ = accounts;
    emit presence_changed(accounts);
    break;
  }
  default:
    break;
  }
}

void CollabEngine::handle_text(const Message& msg) {
  const qint64 ts =
      msg.ts_ms() > 0 ? msg.ts_ms() : QDateTime::currentMSecsSinceEpoch();
  // 群消息（to="group:N"，T4.1）：本地归到群会话（peer=群键），
  // 上抛 group_message_received；单聊维持原路径
  const bool is_group = msg.to().rfind("group:", 0) == 0;
  const std::string peer = is_group ? msg.to() : msg.from();
  bool inserted = true;
  if (store_) {
    memex::client::StoredMessage sm;
    sm.seq = msg.seq();
    sm.peer = peer;
    sm.from = msg.from();
    sm.to = msg.to();
    sm.ts_ms = ts;
    sm.text = msg.has_text() ? msg.text().text() : std::string{};
    sm.source = "collab";
    sm.msg_id = msg.msg_id();
    // 平台-9：接收方向唯一入口是服务端投递——落库即已归档（ARCHIVED）
    sm.sync_state = "ARCHIVED";
    store_->append(sm, &inserted);
  }
  if (!msg.msg_id().empty()) {
    Message ack;
    ack.set_type(MsgType::ACK);
    ack.set_from(account_.toStdString());
    ack.set_to("server");
    ack.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
    ack.mutable_ack()->set_msg_id(msg.msg_id());
    send_frame(ack);
  }
  if (inserted) {
    if (is_group) {
      emit group_message_received(
          QString::fromStdString(msg.to()), QString::fromStdString(msg.from()),
          QString::fromStdString(msg.has_text() ? msg.text().text()
                                                : std::string{}),
          ts, QString::fromStdString(msg.msg_id()));
    } else {
      emit message_received(QString::fromStdString(msg.from()),
                            QString::fromStdString(msg.has_text()
                                                       ? msg.text().text()
                                                       : std::string{}),
                            ts, QString::fromStdString(msg.msg_id()));
    }
  } else {
    qInfo() << "[协作] 重复补投，按 msg_id 去重：" << QString::fromStdString(msg.msg_id());
  }
}

void CollabEngine::handle_notice(const Message& msg) {
  if (!msg.has_notice()) return;
  const auto& n = msg.notice();
  const qint64 ts =
      msg.ts_ms() > 0 ? msg.ts_ms() : QDateTime::currentMSecsSinceEpoch();
  // 群通知（to="group:N"）与个人通知（to=账号，from="通知"）：归档形态与
  // 服务端同源（compose_notice_text），本地按 msg_id 去重（离线补投重复无害）
  const bool is_group = msg.to().rfind("group:", 0) == 0;
  const std::string peer = is_group ? msg.to() : msg.from();
  const std::string text = memex::protocol::compose_notice_text(
      n.title(), n.content(), n.jump_url());
  bool inserted = true;
  if (store_) {
    memex::client::StoredMessage sm;
    // 服务端通知无会话 seq（恒 0）：本地分配单调 seq，否则同 from 的第二条
    // 撞 UNIQUE(from_id, seq) 被 IGNORE 而丢信号；重投仍按 msg_id 去重
    sm.seq = msg.seq() > 0 ? msg.seq()
                           : store_->next_local_seq(msg.from());
    sm.peer = peer;
    sm.from = msg.from();
    sm.to = msg.to();
    sm.ts_ms = ts;
    sm.text = text;
    sm.source = "collab";
    sm.msg_id = msg.msg_id();
    sm.sync_state = "ARCHIVED"; // 服务端投递到达＝已归档（平台-9）
    store_->append(sm, &inserted);
  }
  // ACK 清服务端离线队列（与 TEXT 同语义；未回执下次登录重投，按 msg_id 去重）
  if (!msg.msg_id().empty()) {
    Message ack;
    ack.set_type(MsgType::ACK);
    ack.set_from(account_.toStdString());
    ack.set_to("server");
    ack.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
    ack.mutable_ack()->set_msg_id(msg.msg_id());
    send_frame(ack);
  }
  if (inserted) {
    const QString qtext = QString::fromStdString(text);
    if (is_group) {
      emit group_message_received(QString::fromStdString(msg.to()),
                                  QString::fromStdString(msg.from()), qtext,
                                  ts, QString::fromStdString(msg.msg_id()));
    } else {
      emit message_received(QString::fromStdString(msg.from()), qtext, ts,
                            QString::fromStdString(msg.msg_id()));
    }
    // 分级推送入口（T4.10）：普通＝站内消息已由上面的信号渲染，弹窗策略
    // 由 NotificationCenter 按个人偏好与紧急程度裁决
    emit notice_received(
        QString::fromStdString(msg.from()), QString::fromStdString(n.title()),
        QString::fromStdString(n.content()), static_cast<int>(n.urgency()),
        QString::fromStdString(n.jump_url()), ts,
        QString::fromStdString(msg.msg_id()));
  } else {
    qInfo() << "[协作] 重复通知，按 msg_id 去重："
            << QString::fromStdString(msg.msg_id());
  }
}

void CollabEngine::handle_ack(const Message& msg) {
  if (msg.seq() != 0 && inflight_.contains(msg.seq())) {
    const quint64 seq = msg.seq();
    inflight_.remove(seq);
    // 平台-9：服务端受理＝已归档待投递（SERVER_ACKED）
    if (store_) {
      store_->set_sync_state(account_.toStdString(), seq, "SERVER_ACKED");
    }
    emit text_delivered(seq, true);
  }
}

void CollabEngine::start_heartbeat() {
  heartbeat_missed_ = 0;
  // 未配置心跳参数（interval=0）则不启用——QTimer::start(0) 会退化为
  // 连发，误触超时重连。
  if (heartbeat_interval_ms_ > 0) {
    heartbeat_timer_.start(heartbeat_interval_ms_);
  }
}

void CollabEngine::stop_heartbeat() {
  heartbeat_timer_.stop();
}

void CollabEngine::schedule_reconnect() {
  if (manual_logout_ || host_.isEmpty() || account_.isEmpty()) return;
  reconnecting_ = true;
  qInfo() << "[协作] " << reconnect_backoff_ms_ << "ms 后重连";
  reconnect_timer_.start(reconnect_backoff_ms_);
  reconnect_backoff_ms_ = std::min(reconnect_backoff_ms_ * 2, CollabEngine::kMaxBackoffMs);
}

void CollabEngine::check_delivery_timeouts() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (auto it = inflight_.begin(); it != inflight_.end();) {
    if (it.value().sent_at_ms > 0 &&
        now - it.value().sent_at_ms > CollabEngine::kDeliveryTimeoutMs) {
      // 平台-9 至少一次：回执超时先在连接内按原 seq 重发（服务端按
      // msg_id 去重，重发幂等，不判失败）；连接已亡才转待补传。
      if (logged_in_) {
        PendingSend p = it.value();
        Message m;
        m.set_seq(p.seq);
        m.set_from(account_.toStdString());
        m.set_to(p.to);
        m.set_ts_ms(p.ts_ms);
        if (p.nudge) { // 振屏重发：按原类型空体重建（需求批⑥）
          m.set_type(MsgType::NUDGE);
          m.mutable_nudge();
        } else {
          m.set_type(MsgType::TEXT);
          m.mutable_text()->set_text(p.text);
        }
        send_frame(m);
        p.sent_at_ms = now;
        it.value() = p;
        qInfo() << "[协作] 回执超时，连接内重发（seq" << p.seq << "）";
        ++it;
      } else {
        // 服务端可能已受理——补传按 msg_id 去重，不会重复归档；
        // 真正送达以回执为准
        PendingSend p = it.value();
        p.sent_at_ms = 0;
        pending_reconnect_.push_back(p);
        if (store_) {
          store_->set_sync_state(account_.toStdString(), p.seq, "PENDING");
        }
        it = inflight_.erase(it);
      }
    } else {
      ++it;
    }
  }
  if (inflight_.isEmpty()) delivery_timer_.stop();
}

// 重连成功后补传：暂存消息按原 seq 重发，服务端 sha256(from:seq) 幂等归档
void CollabEngine::flush_pending_sends() {
  // 平台-9 重启恢复：进程崩溃会丢内存暂存队列——库里仍 PENDING/SENDING
  // 的消息灌回补传（按 seq 去重；已 SERVER_ACKED/FAILED 的不会回来）
  if (store_) {
    const QList<memex::client::StoredMessage> stuck =
        store_->pending_sync(account_.toStdString());
    for (const auto& m : stuck) {
      const quint64 seq = m.seq;
      const bool known =
          inflight_.contains(seq) ||
          std::any_of(pending_reconnect_.cbegin(), pending_reconnect_.cend(),
                      [seq](const PendingSend& p) { return p.seq == seq; });
      if (known) continue;
      PendingSend p;
      p.to = m.to;
      p.text = m.text;
      p.seq = seq;
      p.ts_ms = m.ts_ms;
      // 振屏标记行恢复为振屏帧（用户字面输入同名文本的极端情形下重建为
      // TEXT/NUDGE 渲染面等价——都是 "[振屏]" 系统行，如实口径）
      p.nudge = m.text == "[振屏]";
      pending_reconnect_.push_back(p);
      qInfo() << "[协作] 重启恢复：库中待同步消息重新入队（seq" << seq << "）";
    }
  }
  if (pending_reconnect_.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const PendingSend& p : pending_reconnect_) {
    Message m;
    m.set_seq(p.seq);
    m.set_from(account_.toStdString());
    m.set_to(p.to);
    m.set_ts_ms(p.ts_ms);
    if (p.nudge) { // 振屏补传：按原类型空体重建（需求批⑥）
      m.set_type(MsgType::NUDGE);
      m.mutable_nudge();
    } else {
      m.set_type(MsgType::TEXT);
      m.mutable_text()->set_text(p.text);
    }
    send_frame(m);
    PendingSend inflight = p;
    inflight.sent_at_ms = now;
    inflight_.insert(p.seq, inflight);
    if (store_) {
      store_->set_sync_state(account_.toStdString(), p.seq, "SENDING");
    }
    qInfo() << "[协作] 补传中断期消息（seq" << p.seq << "）";
  }
  pending_reconnect_.clear();
  if (!delivery_timer_.isActive()) delivery_timer_.start(500);
}

void CollabEngine::teardown(bool unexpected) {
  logged_in_ = false;
  kicking_ = false;
  online_accounts_.clear(); // 在线表随会话失效（T4.3；重登后推送刷新）
  stop_heartbeat();
  if (unexpected) {
    // 意外断开：在途消息转待补传（可能已达服务端——重发按 msg_id 幂等）
    for (auto it = inflight_.constBegin(); it != inflight_.constEnd(); ++it) {
      PendingSend p = it.value();
      p.sent_at_ms = 0;
      pending_reconnect_.push_back(p);
      if (store_) {
        store_->set_sync_state(account_.toStdString(), p.seq, "PENDING");
      }
    }
  } else {
    // 主动登出／被踢／重新登录：清队列并回报失败，不再补传
    // （平台-9：暂存队列同样终态 FAILED——此前被静默丢弃）
    for (auto it = inflight_.constBegin(); it != inflight_.constEnd(); ++it) {
      emit text_delivered(it.key(), false);
      if (store_) {
        store_->set_sync_state(account_.toStdString(), it.value().seq,
                               "FAILED");
      }
    }
    for (const PendingSend& p : pending_reconnect_) {
      emit text_delivered(p.seq, false);
      if (store_) {
        store_->set_sync_state(account_.toStdString(), p.seq, "FAILED");
      }
    }
    pending_reconnect_.clear();
  }
  inflight_.clear();
  delivery_timer_.stop();
  decoder_.reset();
  if (socket_->state() != QAbstractSocket::UnconnectedState) {
    socket_->abort();
  }
}

std::string CollabEngine::status_text() const {
  return logged_in_ ? "协作态 · 已登录（" + account_.toStdString() + "）"
                    : "协作态 · 未登录";
}

} // namespace memex::client