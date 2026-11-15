// session.cpp 拆分模块（LAY-015）：组织下发·跨态日志·回执三面·历史补齐·在线表；公共面见 common.hpp，声明在 session.hpp。
#include "common.hpp"

namespace memex::server {
void Session::h_org_query(const memex::protocol::Message& msg) { // 组织架构下发（T3.1/T4.6 可见性/策略随发/群透明可查）（LAY-015 自 handle_message switch 臂逐字提取）
    // 组织架构下发（T3.1）：登录后可查——部门树＋成员资料（含直属上级与角色）。
    // T4.6 通讯录可见性：按查询者过滤与脱敏（被隐藏者不可见不可搜——
    // 数据不出服务端，客户端建群/拉人数据源同受此口径约束）。
    if (!logged_in_) return;
    memex::protocol::Message out;
    out.set_type(v1::ORG_DATA);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* data = out.mutable_org_data();
    for (const auto& [id, path] : server_.store().visible_departments(account_)) {
      (void)id;
      data->add_departments()->set_path(path);
    }
    for (const auto& m : server_.store().visible_members(account_)) {
      auto* om = data->add_members();
      om->set_account(m.account);
      om->set_display_name(m.display_name);
      om->set_title(m.title);
      om->set_department_path(m.department_path);
      om->set_manager(m.manager);
      om->set_role(m.role);
      om->set_signature(m.signature);
      // 需求批⑩ 在线时长：滚动 24h/7d/30d 窗并集秒数随成员下发。组织
      // 架构=内网量级，查询时现算可接受（流水表只有登录/登出量级的行）。
      const auto& store = server_.store();
      const std::int64_t now = now_ms();
      om->set_online_day_s(
          store.online_ms_between(m.account, now - 24LL * 3600 * 1000, now) /
          1000);
      om->set_online_week_s(
          store.online_ms_between(m.account, now - 7LL * 24 * 3600 * 1000,
                                  now) /
          1000);
      om->set_online_month_s(
          store.online_ms_between(m.account, now - 30LL * 24 * 3600 * 1000,
                                  now) /
          1000);
      // 需求批⑫：头像版本戳随成员下发（0=未设置；字节走文件面按需取）
      om->set_avatar_ver(store.avatar_ver(m.account));
    }
    // T3.4 策略开关随组织架构一并下发（客户端按本人部门解析生效行）
    for (const auto& p : server_.store().policy_list()) {
      auto* op = data->add_policies();
      op->set_department_path(p.department_path);
      op->set_allow_anonymous(p.allow_anonymous);
      op->set_allow_cross_state(p.allow_cross_state);
      op->set_new_device_approval(p.new_device_approval);
      op->set_allow_cross_dept_file(p.allow_cross_dept_file);
      op->set_allow_forward_file(p.allow_forward_file);
    }
    // 权限模型「群在组织架构可见」（/etc/group 类比）：全量群透明可查，
    // 不按查看者裁剪（与成员/部门的 T4.6 可见性口径不同——群是权限载体，
    // 透明可查是模型明文规则）。
    for (const auto& g : server_.store().groups_list()) {
      auto* og = data->add_groups();
      og->set_group_id(g.group_id);
      og->set_name(g.name);
      og->set_owner(g.owner);
      for (const auto& m : g.members) og->add_members(m);
    }
    send(memex::protocol::encode(out));
}

void Session::h_cross_log(const memex::protocol::Message& msg) { // 跨态日志（需求批⑩）（LAY-015 自 handle_message switch 臂逐字提取）
    // 跨态会话日志（T4.2）：登录端上报与未登录设备的会话起止。
    // 载荷只有时间/双方/时长——无内容字段，服务端也只落这三类（留痕纪律）。
    if (!logged_in_ || !msg.has_cross_log()) return;
    const auto& c = msg.cross_log();
    if (c.op() == "start") {
      server_.store().cross_log_start(account_, c.peer_device(), c.peer_name(),
                                      c.started_ms());
      log("跨态会话建立：" + c.peer_device());
    } else if (c.op() == "end") {
      if (server_.store().cross_log_end(account_, c.peer_device(),
                                        c.started_ms(), c.ended_ms())) {
        log("跨态会话结束：" + c.peer_device() + "（时长 " +
            std::to_string(c.ended_ms() - c.started_ms()) + "ms）");
      }
    }
}

void Session::h_ack(const memex::protocol::Message& msg) { // ACK 投递确认（LAY-015 自 handle_message switch 臂逐字提取）
    // 接收方回执：消息已收取，清离线队列；平台-4 落 delivered 事件。
    // 需求批⑦ 送达级：在线即投与离线补投同走此口——首个该账号的
    // delivered 事件才记才推（重复 ACK 幂等），发送方在线即收
    // DELIVER_NOTICE（离线则仅留痕，回执态可经 RECEIPT_QUERY 补查）。
    if (logged_in_ && msg.has_ack() && !msg.ack().msg_id().empty()) {
      const std::string target = msg.ack().msg_id();
      server_.store().ack_offline(target, account_); // 清离线队列（幂等）
      const auto row = server_.store().message_by_id(target);
      if (!row) return; // 非归档消息（伪造 msg_id）不留痕
      if (!is_recipient(server_, row->to_account, account_)) {
        log("伪造回执被拒：非收件人 ACK " + target);
        return;
      }
      const std::string original_from = row->from_account;
      if (server_.store().delivered_recorded(target, account_)) return;
      const std::int64_t dms = now_ms(); // 留痕与通知共用同一时刻
      server_.store().append_message_event(target, "delivered", account_, "",
                                           dms);
      memex::protocol::Message out;
      out.set_type(v1::DELIVER_NOTICE);
      out.set_from("server");
      out.set_ts_ms(dms);
      auto* n = out.mutable_deliver_notice();
      n->set_msg_id(target);
      n->set_delivered_to(account_);
      n->set_delivered_ms(dms);
      // deliver_frame 入参为纯 Envelope 字节（其内部加长度前缀）
      const std::string blob = out.SerializeAsString();
      for (const auto& s : server_.online_sessions(original_from)) {
        s->deliver_frame(blob);
      }
    }
}

void Session::h_read(const memex::protocol::Message& msg) { // READ 已读回执（LAY-015 自 handle_message switch 臂逐字提取）
    // 已读上报（T4.3）：接收方已读某条归档消息。留痕后若原发送方在线，
    // 推送 READ_NOTICE（发送方离线则仅留痕，不补投——已读态下次可查）。
    if (!logged_in_ || !msg.has_read() || msg.read().msg_id().empty()) return;
    const std::string target = msg.read().msg_id();
    const auto row = server_.store().message_by_id(target);
    if (!row) return; // 非归档消息（伪造 msg_id）不留痕
    if (!is_recipient(server_, row->to_account, account_)) {
      log("伪造回执被拒：非收件人 READ " + target);
      return;
    }
    const std::string original_from = row->from_account;
    // 重复上报幂等：已记过该读者的不再重发 NOTICE（发送方已知已读）
    bool is_new = true;
    for (const auto& r : server_.store().readers_for(target)) {
      if (r.reader == account_) {
        is_new = false;
        break;
      }
    }
    const std::int64_t rms = now_ms(); // 留痕与通知共用同一时刻
    if (!server_.store().record_read(target, account_, rms)) return;
    if (!is_new) return;
    server_.store().append_message_event(target, "read", account_, "", rms);
    memex::protocol::Message out;
    out.set_type(v1::READ_NOTICE);
    out.set_from("server");
    out.set_ts_ms(rms);
    auto* n = out.mutable_read_notice();
    n->set_msg_id(target);
    n->set_reader(account_);
    n->set_read_ms(rms);
    // deliver_frame 入参为纯 Envelope 字节（其内部加长度前缀）
    const std::string blob = out.SerializeAsString();
    for (const auto& s : server_.online_sessions(original_from)) {
      s->deliver_frame(blob);
    }
}

void Session::h_receipt_query(const memex::protocol::Message& msg) { // 回执批量查询（LAY-015 自 handle_message switch 臂逐字提取）
    // 回执态查询（需求批⑦ 归档扩）：发送方补查自己消息的送达/已读
    // 名单——离线期错过的 DELIVER_NOTICE/READ_NOTICE 经此回看。
    // 仅 message_from == 请求者的条目回包，他人消息的回执不泄露。
    if (!logged_in_ || !msg.has_receipt_query()) return;
    memex::protocol::Message out;
    out.set_type(v1::RECEIPT_DATA);
    out.set_from("server");
    out.set_ts_ms(now_ms());
    auto* data = out.mutable_receipt_data();
    const int n = msg.receipt_query().msg_ids_size();
    for (int i = 0; i < n && i < 100; ++i) { // 上限 100（防滥用）
      const std::string& id = msg.receipt_query().msg_ids(i);
      if (server_.store().message_from(id) != account_) continue;
      auto* e = data->add_entries();
      e->set_msg_id(id);
      for (const auto& ev : server_.store().message_events(id)) {
        if (ev.event == "delivered") e->add_delivered_to(ev.by_account);
      }
      for (const auto& r : server_.store().readers_for(id)) {
        e->add_readers(r.reader);
      }
    }
    send(memex::protocol::encode(out));
}

void Session::h_message_query(const memex::protocol::Message& msg) { // MESSAGE_QUERY 历史补齐（LAY-015 自 handle_message switch 臂逐字提取）
    // 会话历史按日期查询（需求批⑧）：按请求者视角检索归档——search_messages
    // 的 account 口径保证只见自己参与的消息（单聊双向成对、群靠成员关系
    // 联入）；peer=对端账号或群键 group:N。已撤回条目不出回包（撤回即
    // 不可见）；时间窗含端点；正序回包（聊天渲染方向）。归档倒序取最近
    // N 条后反转，与本端 history 的「最近 N 条正序」口径同构。
    if (!logged_in_ || !msg.has_message_query()) return;
    const auto& q = msg.message_query();
    memex::protocol::Message out;
    out.set_type(v1::MESSAGE_PAGE);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* page = out.mutable_message_page();
    page->set_peer(q.peer());
    page->set_from_ms(q.from_ms());
    page->set_until_ms(q.until_ms());
    if (!q.peer().empty()) {
      memex::server::MessageSearch s;
      s.account = account_;
      s.peer = q.peer();
      s.since_ms = q.from_ms();
      s.until_ms = q.until_ms();
      s.limit = q.limit() > 0 ? std::min(q.limit(), 500) : 200;
      auto hits = server_.store().search_messages(s);
      // search_messages 按 id 倒序取「最近 N 条」（与 CLI/审计共面）；
      // 回包契约是时间正序——稳定排序按 ts 升（并列保持 id 序）。
      // 不直接按 ts 取最近 N：窗内截断口径沿用归档插入序，边界行为
      // 与既有检索面一致。
      std::reverse(hits.begin(), hits.end());
      std::stable_sort(hits.begin(), hits.end(),
                       [](const memex::server::ArchivedMessage& x,
                          const memex::server::ArchivedMessage& y) {
                         return x.ts_ms < y.ts_ms;
                       });
      for (const auto& h : hits) {
        if (h.recalled) continue;
        auto* e = page->add_messages();
        e->set_msg_id(h.msg_id);
        e->set_from(h.from_account);
        e->set_to(h.to_account);
        e->set_ts_ms(h.ts_ms);
        e->set_text(h.text);
      }
    }
    send(memex::protocol::encode(out));
}

void Session::h_presence_query(const memex::protocol::Message& msg) { // 在线表查询（LAY-015 自 handle_message switch 臂逐字提取）
    // 在线账号表查询（T4.3）：登录后可查；变更推送由服务端主动广播
    if (!logged_in_) return;
    memex::protocol::Message out;
    out.set_type(v1::PRESENCE_DATA);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* p = out.mutable_presence_data();
    for (const auto& a : server_.online_accounts()) p->add_accounts(a);
    send(memex::protocol::encode(out));
}

} // namespace memex::server
