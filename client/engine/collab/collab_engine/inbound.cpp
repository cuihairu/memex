// collab_engine 拆分模块（LAY-013）：入站消息处理：text/notice/ack——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 collab_engine.hpp。
#include "common.hpp"

namespace memex::client {
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
    // BUG-007 §4.2：(from_id, seq) 槽位被占且既有行 msg_id 不同＝服务端
    // 换盐重排后的新消息（同 seq 不同内容）——本地改分配单调 seq 落库
    // （与 handle_notice 对无 seq 通知同构），否则 UNIQUE 约束静默丢行；
    // msg_id 相同＝真补投照旧去重。重投经 msg_id 唯一索引仍幂等。
    if (msg.seq() > 0 && !msg.msg_id().empty()) {
      const std::string prev = store_->msg_id_at(msg.from(), msg.seq());
      if (!prev.empty() && prev != msg.msg_id()) {
        sm.seq = store_->next_local_seq(msg.from());
      }
    }
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
      // BUG-007 §4.3：回执回带服务端最终 msg_id（撞 id 换盐重排后的新值）
      // 时覆盖本地推定值——此后送达/已读回执、撤回按新 id 命中；老服务端
      // 不回带（空串）不动作
      if (msg.has_ack() && !msg.ack().msg_id().empty()) {
        store_->set_msg_id_final(account_.toStdString(), seq,
                                 msg.ack().msg_id());
      }
    }
    emit text_delivered(seq, true);
  }
}

} // namespace memex::client
