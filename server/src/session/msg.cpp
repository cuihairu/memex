// session.cpp 拆分模块（LAY-015）：文本路由·振屏·撤回；公共面见 common.hpp，声明在 session.hpp。
#include "common.hpp"

namespace memex::server {
void Session::h_text(const memex::protocol::Message& msg) { // 文本消息路由（直连/协作/多端同步）（LAY-015 自 handle_message switch 臂逐字提取）
    // 协作态消息路由：先入离线队列（至少一次投递），在线即投；
    // 接收方 ACK(msg_id) 清队列，未 ACK 的下次登录重投（接收端按 msg_id 去重）。
    // 群消息（to="group:<群号>"，T4.1）：成员校验后全量归档一次、按成员扇出。
    if (!logged_in_ || !msg.has_text()) return;
    // 平台-1 伪造身份闸（授权盘点 P0-1 补漏）：归档与投递的发送者一律
    // 绑定登录会话身份，不信任客户端自填 from——与撤回同款拒收＋留痕
    if (msg.from() != account_) {
      log("伪造发送者被拒：" + account_ + " 报称 " + msg.from());
      return;
    }
    // 收件人集合：单聊一人；群聊＝群成员（不含发送者）
    std::vector<std::string> recipients;
    const bool is_group = msg.to().rfind("group:", 0) == 0;
    if (is_group) {
      const auto gid = static_cast<std::uint64_t>(
          std::strtoull(msg.to().c_str() + 6, nullptr, 10));
      if (!server_.store().is_group_member(gid, account_)) {
        log("非群成员发群消息被拒：" + msg.to());
        return; // 拒收不归档不入队（成员校验先于归档，原口径不变）
      }
      for (const auto& m : server_.store().group_members(gid)) {
        if (m != account_) recipients.push_back(m);
      }
    } else {
      recipients.push_back(msg.to());
    }
    // BUG-007：成员校验通过后派生 msg_id 落归档消歧（撞键内容异则换盐重排）
    const std::string derived =
        sha256_hex(msg.from() + ":" + std::to_string(msg.seq()));
    const auto msg_id =
        archive_and_resolve(server_, msg, msg.text().text(), derived);
    if (!msg_id) {
      // 评审 P0-2 归档失败不谎报成功：不入离线队列、不投在线、不回 ACK、
      // 不刷常用联系人——发送端在途行保持 SENDING，随断线补传重试
      //（重发按派生 id 幂等，恢复后重试恰一次入档）
      log("归档失败暂不受理（seq " + std::to_string(msg.seq()) + "）");
      return;
    }
    memex::protocol::Message out = msg;
    out.set_msg_id(*msg_id);
    const std::string blob = out.SerializeAsString();

    for (const auto& to : recipients) {
      server_.store().queue_offline(*msg_id, to, blob);
    }
    // 在线即投（桌面＋手机都在则都投，任一端 ACK 即清该端队列）
    for (const auto& to : recipients) {
      for (const auto& target : server_.online_sessions(to)) {
        target->deliver_frame(blob);
      }
    }
    // 发送方受理回执（原 seq＋最终 msg_id）：消息已被服务端接收并负责投递；
    // msg_id 回带最终 id（撞键重排时为重排新值），桌面 §4.3 校正本地行用
    memex::protocol::Message ack;
    ack.set_type(v1::ACK);
    ack.set_seq(msg.seq());
    ack.set_to(msg.from());
    ack.mutable_ack()->set_msg_id(*msg_id);
    send(memex::protocol::encode(ack));
    // T4.5 常用联系人最近刷新：发送方 ↔ 会话（单聊对端／群）
    const std::int64_t fav_ts = msg.ts_ms() > 0 ? msg.ts_ms() : now_ms();
    server_.store().fav_touch(msg.from(), msg.to(), fav_ts);
    if (is_group) {
      for (const auto& to : recipients) {
        server_.store().fav_touch(to, msg.to(), fav_ts);
      }
    } else {
      server_.store().fav_touch(msg.to(), msg.from(), fav_ts);
    }
    // 最近刷新后即时回推双方全量（在线会话持有最新排序；COALESCE 于下一次
    // 自然重推——客户端按整表替换处理，重复无妨）
    auto push_favs = [&](const std::string& acct) {
      for (const auto& s : server_.online_sessions(acct)) {
        memex::protocol::Message fo;
        fo.set_type(v1::FAV_DATA);
        fo.set_from("server");
        fo.set_to(acct);
        fo.set_ts_ms(now_ms());
        auto* d = fo.mutable_fav_data();
        for (const auto& f : server_.store().fav_list(acct)) {
          auto* e = d->add_entries();
          e->set_peer(f.peer);
          e->set_starred(f.starred);
          e->set_last_ms(f.last_ms);
        }
        s->deliver_frame(fo.SerializeAsString());
      }
    };
    push_favs(msg.from());
    for (const auto& to : recipients) push_favs(to);
    // 邮箱·通知入箱（mailbox.md §I：@我归通知类——正文即本条群消息
    //（已全量归档），邮箱只承载状态，不走新推送链；提及匹配 @账号/
    // @显示名（客户端 @ 选择器插显示名），边界判定防 @abc 误中 @ab）
    if (is_group) {
      const std::string& mention_text = msg.text().text();
      auto contains_mention = [&mention_text](const std::string& name) {
        if (name.empty()) return false;
        const std::string needle = "@" + name;
        std::size_t pos = mention_text.find(needle);
        while (pos != std::string::npos) {
          const std::size_t end = pos + needle.size();
          if (end >= mention_text.size()) return true;
          const unsigned char next =
              static_cast<unsigned char>(mention_text[end]);
          const bool cont = (next >= 'a' && next <= 'z') ||
                            (next >= 'A' && next <= 'Z') ||
                            (next >= '0' && next <= '9') || next == '_' ||
                            next == '.' || next == '-' || next >= 0x80;
          if (!cont) return true;
          pos = mention_text.find(needle, end);
        }
        return false;
      };
      const auto sender_acct = server_.store().find_account(account_);
      const std::string sender_name =
          sender_acct ? sender_acct->display_name : account_;
      const auto ginfo = server_.store().group_info(
          static_cast<std::uint64_t>(
              std::strtoull(msg.to().c_str() + 6, nullptr, 10)));
      if (ginfo.has_value()) {
        std::vector<std::string> mentioned;
        for (const auto& to : recipients) {
          const auto prof = server_.store().find_account(to);
          if (contains_mention(to) ||
              (prof && contains_mention(prof->display_name))) {
            mentioned.push_back(to);
          }
        }
        if (!mentioned.empty()) {
          (void)server_.store().mailbox_add(
              "notice",
              sender_name + " 在群「" + ginfo->name + "」中 @ 了你",
              "@我 · 回执只提醒一次，重复刷新不重复弹", "@我", msg.to(),
              "mention:" + *msg_id, std::move(mentioned), now_ms());
        }
      }
    }
}

void Session::h_nudge(const memex::protocol::Message& msg) { // NUDGE 振屏（限频）（LAY-015 自 handle_message switch 臂逐字提取）
    // 振屏（需求批⑥）：实时信号——归档留痕（"[振屏]" 标记文本，历史检索
    // 可见）＋在线即投；不进离线补投（补投的抖动失去时效且成骚扰）。
    // 防刷限频双层：服务端连接级 1s 最小间隔（静默丢弃），发送端 UI 级
    // 同一会话 10s 冷却（客户端面）。群扇出留余量：仅单聊。
    if (!logged_in_ || !msg.has_nudge()) return;
    // 平台-1 伪造身份闸：振屏同样绑定会话身份（NUDGE 归档含 from）
    if (msg.from() != account_) {
      log("伪造发送者被拒：" + account_ + " 报称 " + msg.from());
      return;
    }
    if (msg.to().rfind("group:", 0) == 0) {
      log("群振屏不支持被拒：" + msg.to());
      return;
    }
    const std::int64_t now = now_ms();
    if (now - last_nudge_ms_ < 1000) {
      log("振屏发送过频被拒（连接级 1s 限频）");
      return;
    }
    last_nudge_ms_ = now;
    // BUG-007：同 TEXT 口径先落归档消歧（撞键内容异则换盐重排）
    const std::string derived =
        sha256_hex(msg.from() + ":" + std::to_string(msg.seq()));
    const auto msg_id =
        archive_and_resolve(server_, msg, std::string("[振屏]"), derived);
    if (!msg_id) {
      // 评审 P0-2：归档失败不谎报成功——不投在线不回 ACK（限频已计，
      // 重试由发送端下一次振屏承担）
      log("振屏归档失败暂不受理（seq " + std::to_string(msg.seq()) + "）");
      return;
    }
    memex::protocol::Message out = msg;
    out.set_msg_id(*msg_id);
    log("振屏：" + account_ + " → " + msg.to());
    for (const auto& target : server_.online_sessions(msg.to())) {
      target->deliver_frame(out.SerializeAsString());
    }
    // 发送方受理回执（原 seq＋最终 msg_id）：同 TEXT 口径，客户端同步状态推移
    memex::protocol::Message ack;
    ack.set_type(v1::ACK);
    ack.set_seq(msg.seq());
    ack.set_to(msg.from());
    ack.mutable_ack()->set_msg_id(*msg_id);
    send(memex::protocol::encode(ack));
}

void Session::h_recall(const memex::protocol::Message& msg) { // 消息撤回（LAY-015 自 handle_message switch 臂逐字提取）
    // 撤回：仅置标记不清正文；事件独立留痕；转发给对端会话（本地展示标记）。
    if (!logged_in_ || !msg.has_recall()) return;
    const std::string target = msg.recall().msg_id();
    if (target.empty()) return;
    const std::string original_from = server_.store().message_from(target);
    if (original_from.empty()) {
      log("撤回目标不存在：" + target);
      return;
    }
    if (original_from != account_) {
      log("越权撤回被拒：目标发送方为 " + original_from);
      return;
    }
    server_.store().recall_message(target, account_, now_ms());
    log("撤回留痕：" + target);
    // 转发给消息会话双方的在线会话（不含本会话），客户端按 msg_id 标记本地副本
    for (const auto& s : server_.online_sessions(msg.to())) {
      memex::protocol::Message out = msg;
      out.set_msg_id(target);
      s->deliver_frame(out.SerializeAsString());
    }
    for (const auto& s : server_.online_sessions(account_)) {
      if (s.get() == this) continue;
      memex::protocol::Message out = msg;
      out.set_msg_id(target);
      s->deliver_frame(out.SerializeAsString());
    }
}

} // namespace memex::server
