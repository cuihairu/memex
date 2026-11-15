// collab_engine 拆分模块（LAY-013）：发出侧 API：消息/撤回/组织/常用联系人/签名/群管理/跨态/已读/回执/历史/在线——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 collab_engine.hpp。
#include "common.hpp"

namespace memex::client {
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

void CollabEngine::create_group(const QString& name, const QStringList& members,
                                const QString& topic, const QString& purpose) {
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
  c->set_topic(topic.toStdString()); // 群组-1：空串=跳过（proto3 缺省同形）
  c->set_purpose(purpose.toStdString());
  send_frame(m);
}

// 群组-1 群资料（主题/目的）后期编辑：权沿群公告先例（群主/管理员，
// 服务端裁决），两字段整组覆盖＝留空即清除
void CollabEngine::set_group_profile(quint64 group_id, const QString& topic,
                                     const QString& purpose) {
  if (!logged_in_) return;
  Message m;
  m.set_type(MsgType::GROUP_CMD);
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* c = m.mutable_group_cmd();
  c->set_op("profile");
  c->set_group_id(group_id);
  c->set_topic(topic.toStdString());
  c->set_purpose(purpose.toStdString());
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
} // namespace memex::client
