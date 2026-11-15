// collab_engine 拆分模块（LAY-013）：心跳/重连/投递超时/补发/teardown/状态文本——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 collab_engine.hpp。
#include "common.hpp"

namespace memex::client {
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
