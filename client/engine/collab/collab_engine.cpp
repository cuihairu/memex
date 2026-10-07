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

void CollabEngine::query_presence() {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::PRESENCE_QUERY);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
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
        // T2.5：重连成功即补传中断期暂存消息（服务端按 msg_id 去重归档）
        flush_pending_sends();
      } else {
        emit logged_in(account_, display);
        fav_query(); // T4.5 常用联系人随登录自动拉取（换机保留即此体现）
      }
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
  case MsgType::NOTICE:
    if (msg.has_notice()) handle_notice(msg);
    break;
  case MsgType::ACK:
    handle_ack(msg);
    break;
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
                              {"role", m.role()}});
    }
    j["policies"] = nlohmann::json::array();
    for (const auto& p : msg.org_data().policies()) {
      j["policies"].push_back({{"department_path", p.department_path()},
                               {"allow_anonymous", p.allow_anonymous()},
                               {"allow_cross_state", p.allow_cross_state()},
                               {"new_device_approval", p.new_device_approval()}});
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
    emit message_read(QString::fromStdString(n.msg_id()),
                      QString::fromStdString(n.reader()), n.read_ms());
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
      // 超时未回执：不判失败，转待补传（服务端可能已受理——补传按
      // msg_id 去重，不会重复归档；真正送达以回执为准）
      PendingSend p = it.value();
      p.sent_at_ms = 0;
      pending_reconnect_.push_back(p);
      it = inflight_.erase(it);
    } else {
      ++it;
    }
  }
  if (inflight_.isEmpty()) delivery_timer_.stop();
}

// 重连成功后补传：暂存消息按原 seq 重发，服务端 sha256(from:seq) 幂等归档
void CollabEngine::flush_pending_sends() {
  if (pending_reconnect_.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const PendingSend& p : pending_reconnect_) {
    Message m;
    m.set_type(MsgType::TEXT);
    m.set_seq(p.seq);
    m.set_from(account_.toStdString());
    m.set_to(p.to);
    m.set_ts_ms(p.ts_ms);
    m.mutable_text()->set_text(p.text);
    send_frame(m);
    PendingSend inflight = p;
    inflight.sent_at_ms = now;
    inflight_.insert(p.seq, inflight);
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
    }
  } else {
    // 主动登出／被踢／重新登录：清队列并回报失败，不再补传
    for (auto it = inflight_.constBegin(); it != inflight_.constEnd(); ++it) {
      emit text_delivered(it.key(), false);
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