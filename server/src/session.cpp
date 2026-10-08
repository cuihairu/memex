#include "session.hpp"

#include <chrono>
#include <iostream>

#include "cred.hpp"
#include "webhook.hpp" // R24-1 公告联动三级推送（deliver_notice）

namespace memex::server {

namespace v1 = memex::protocol::v1;

namespace {
std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// UTF-8 码点计数（需求批⑪签名「120 字」门：按字数不按字节——
// 一个汉字 3 字节，字节口径会把 40 字汉字签名误判超限）
std::size_t utf8_len(const std::string& s) {
  std::size_t n = 0;
  for (const unsigned char ch : s) {
    if ((ch & 0xC0) != 0x80) ++n; // 非续字节＝一个码点起点
  }
  return n;
}
} // namespace

Session::Session(asio::ip::tcp::socket socket, CollabServer& server)
    : socket_(std::move(socket)), server_(server),
      remote_([&] {
        try {
          return socket_.remote_endpoint().address().to_string();
        } catch (...) {
          return std::string{"?"};
        }
      }()) {}

void Session::start() {
  log("接入");
  do_read();
}

void Session::kick(const std::string& reason, const std::string& replaced_by) {
  memex::protocol::Message k;
  k.set_type(memex::protocol::v1::KICK);
  k.set_from("server");
  k.set_to(account_);
  k.set_ts_ms(now_ms());
  k.mutable_kick()->set_reason(reason);
  k.mutable_kick()->set_replaced_by(replaced_by);
  log("互踢下发（" + reason + "）");
  close_after_flush_ = true;
  logged_in_ = false;
  send(memex::protocol::encode(k));
}

// 发送统一入口：写队列单链驱动（async_write 不可并发，队空闲才起链）
void Session::send(std::string frame) {
  const bool idle = write_queue_.empty();
  write_queue_.push_back(std::move(frame));
  if (idle && !closed_) do_write();
}

void Session::deliver_frame(const std::string& envelope_blob) {
  send(memex::protocol::encode_frame(envelope_blob));
}

void Session::do_read() {
  auto self = shared_from_this();
  socket_.async_read_some(
      asio::buffer(read_buf_),
      [this, self](std::error_code ec, std::size_t n) {
        if (ec) {
          log(std::string{"断开："} + ec.message());
          if (logged_in_) server_.unregister_online(account_, kind_, this);
          return;
        }
        handle_bytes(n);
        if (!closed_) do_read();
      });
}

void Session::handle_bytes(std::size_t n) {
  std::vector<std::string> frames;
  const auto st = decoder_.feed(std::string_view(read_buf_.data(), n), frames);
  if (st == memex::protocol::DecodeStatus::kZeroLength ||
      st == memex::protocol::DecodeStatus::kTooLarge) {
    log(std::string{"非法帧："} + memex::protocol::decode_status_name(st));
    close();
    return;
  }
  for (const auto& f : frames) {
    if (closed_) return;
    try {
      handle_message(memex::protocol::decode_payload(f));
    } catch (const memex::protocol::ProtocolError& e) {
      log(std::string{"协议错误："} + e.what());
    }
  }
}

void Session::handle_message(const memex::protocol::Message& msg) {
  if (logged_in_) {
    log(std::string{"收到 "} + memex::protocol::msg_type_name(msg.type()) +
        "（" + account_ + "）");
  } else {
    log(std::string{"收到 "} + memex::protocol::msg_type_name(msg.type()));
  }

  switch (msg.type()) {
  case v1::PING: {
    memex::protocol::Message pong;
    pong.set_type(v1::PONG);
    pong.set_seq(msg.seq());
    pong.set_from("server");
    pong.set_to(msg.from());
    pong.set_ts_ms(now_ms());
    send(memex::protocol::encode(pong));
    break;
  }
  case v1::LOGIN:
    if (!logged_in_) handle_login(msg);
    break;
  case v1::LOGOUT:
    if (logged_in_) handle_logout();
    break;
  case v1::ORG_QUERY: {
    // 组织架构下发（T3.1）：登录后可查——部门树＋成员资料（含直属上级与角色）。
    // T4.6 通讯录可见性：按查询者过滤与脱敏（被隐藏者不可见不可搜——
    // 数据不出服务端，客户端建群/拉人数据源同受此口径约束）。
    if (!logged_in_) break;
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
    break;
  }
  case v1::TEXT: {
    // 协作态消息路由：先入离线队列（至少一次投递），在线即投；
    // 接收方 ACK(msg_id) 清队列，未 ACK 的下次登录重投（接收端按 msg_id 去重）。
    // 群消息（to="group:<群号>"，T4.1）：成员校验后全量归档一次、按成员扇出。
    if (!logged_in_ || !msg.has_text()) break;
    const std::string msg_id =
        sha256_hex(msg.from() + ":" + std::to_string(msg.seq()));
    memex::protocol::Message out = msg;
    out.set_msg_id(msg_id);
    const std::string blob = out.SerializeAsString();

    // 收件人集合：单聊一人；群聊＝群成员（不含发送者）
    std::vector<std::string> recipients;
    const bool is_group = msg.to().rfind("group:", 0) == 0;
    if (is_group) {
      const auto gid = static_cast<std::uint64_t>(
          std::strtoull(msg.to().c_str() + 6, nullptr, 10));
      if (!server_.store().is_group_member(gid, account_)) {
        log("非群成员发群消息被拒：" + msg.to());
        break;
      }
      for (const auto& m : server_.store().group_members(gid)) {
        if (m != account_) recipients.push_back(m);
      }
    } else {
      recipients.push_back(msg.to());
    }

    for (const auto& to : recipients) {
      server_.store().queue_offline(msg_id, to, blob);
    }
    // T2.3 全量归档：协作态消息原样落服务端归档库（群消息 to=群标识，一次）
    server_.store().store_message(msg_id, msg.from(), msg.to(),
                                  static_cast<int>(msg.type()),
                                  msg.text().text(), msg.ts_ms());
    // 在线即投（桌面＋手机都在则都投，任一端 ACK 即清该端队列）
    for (const auto& to : recipients) {
      for (const auto& target : server_.online_sessions(to)) {
        target->deliver_frame(blob);
      }
    }
    // 发送方受理回执（原 seq）：消息已被服务端接收并负责投递
    memex::protocol::Message ack;
    ack.set_type(v1::ACK);
    ack.set_seq(msg.seq());
    ack.set_to(msg.from());
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
    break;
  }
  case v1::GROUP_CMD: {
    // 群管理（T4.1）：建群／拉人／退群／群公告；结果经 GROUP_RESULT 回执
    if (!logged_in_ || !msg.has_group_cmd()) break;
    const auto& cmd = msg.group_cmd();
    bool ok = false;
    bool want_history = false; // R24-1：announce_history 回执带编辑历史
    std::string reason;
    std::uint64_t gid = cmd.group_id();
    if (cmd.op() == "create") {
      // 平台-12 建群需特权（权限模型「建群需授权」=groupadd 需特权）：
      // 统一判权服务裁决——追加角色 group_creator（org role grant，平台-3
      // 时间窗机制）或基础 admin 放行；未授权默认拒（白名单口径）
      const auto d = server_.group_az().authorize(
          {account_, "group:create", "group", ""});
      if (!d.allowed) {
        ok = false;
        reason = "建群失败（" + d.reason +
                 "：建群是特权动作，须组织管理员授予 group_creator）";
      } else {
        std::vector<std::string> members(cmd.members().begin(),
                                         cmd.members().end());
        gid = server_.store().create_group(cmd.name(), account_, members);
        ok = gid > 0;
        if (!ok) reason = "建群失败（群名空或成员账号不存在）";
      }
    } else if (cmd.op() == "invite") {
      // 拉人者须为本群成员（防外部账号凭群号塞人）
      if (!server_.store().is_group_member(cmd.group_id(), account_)) {
        ok = false;
        reason = "拉人失败（仅群成员可拉人）";
      } else {
        ok = true;
        for (const auto& m : cmd.members()) {
          if (!server_.store().group_invite(cmd.group_id(), m)) {
            ok = false;
            reason = "拉人失败（群不存在／账号不存在／已在群里）：" + m;
            break;
          }
        }
      }
    } else if (cmd.op() == "leave") {
      ok = server_.store().group_leave(cmd.group_id(), account_);
      if (!ok) reason = "退群失败（群不存在或不在群里）";
    } else if (cmd.op() == "announce") {
      // 平台-12 群能力开关（权限模型「群能力管理员配全」）：公告能力被
      // 管理员停用即拒（未配置=现行允许；配置权在群主/管理员，CLI 面把守）
      if (!server_.store().group_capability_enabled(cmd.group_id(),
                                                    "notice")) {
        ok = false;
        reason = "公告设置失败（deny:capability-notice：群公告能力已被"
                 "管理员停用）";
      } else {
        ok = server_.store().group_announce(cmd.group_id(), account_,
                                            cmd.announcement());
        if (!ok) reason = "公告设置失败（仅群主/管理员可设）";
      }
      if (ok && !cmd.announcement().empty()) {
        // R24-1 联动三级推送：公告=重要强提醒，全员（含设置者）收 NOTICE
        //（清除公告不推——管理动作非新信息，全员强提醒是骚扰）。
        const auto info = server_.store().group_info(cmd.group_id());
        if (info.has_value()) {
          const std::string target =
              "group:" + std::to_string(cmd.group_id());
          deliver_notice(server_, target,
                         "群公告：" + info->name, cmd.announcement(),
                         static_cast<int>(v1::Notice::IMPORTANT), target);
        }
      }
    } else if (cmd.op() == "announce_history") {
      // R24-1 编辑历史查询：群成员可查（留痕是全员可见信息的一部分）；
      // 回执 history 倒序带全，客户端经专用信号展示。
      if (!server_.store().is_group_member(cmd.group_id(), account_)) {
        ok = false;
        reason = "公告历史查询失败（仅群成员可查）";
      } else {
        ok = true;
        want_history = true;
      }
    } else {
      break;
    }
    log(std::string("群命令 ") + cmd.op() + (ok ? " 成功" : " 失败：" + reason));
    memex::protocol::Message result;
    result.set_type(v1::GROUP_RESULT);
    result.set_to(account_);
    result.set_ts_ms(now_ms());
    auto* r = result.mutable_group_result();
    r->set_ok(ok);
    r->set_reason(reason);
    r->set_op(cmd.op());
    r->set_group_id(gid);
    if (want_history) {
      for (const auto& h :
           server_.store().announcement_history(cmd.group_id())) {
        auto* hr = r->add_history();
        hr->set_editor(h.editor);
        hr->set_content(h.content);
        hr->set_ts_ms(h.ts_ms);
      }
    }
    send(memex::protocol::encode(result));
    break;
  }
  case v1::GROUP_QUERY: {
    // 我加入的群（T4.1）：登录后可查，客户端群列表与公告数据源
    if (!logged_in_) break;
    memex::protocol::Message out;
    out.set_type(v1::GROUP_DATA);
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* data = out.mutable_group_data();
    for (const auto& g : server_.store().groups_of(account_)) {
      auto* gi = data->add_groups();
      gi->set_group_id(g.group_id);
      gi->set_name(g.name);
      gi->set_owner(g.owner);
      gi->set_announcement(g.announcement);
      for (const auto& m : g.members) gi->add_members(m);
    }
    send(memex::protocol::encode(out));
    break;
  }
  case v1::CROSS_LOG: {
    // 跨态会话日志（T4.2）：登录端上报与未登录设备的会话起止。
    // 载荷只有时间/双方/时长——无内容字段，服务端也只落这三类（留痕纪律）。
    if (!logged_in_ || !msg.has_cross_log()) break;
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
    break;
  }
  case v1::ACK:
    // 接收方回执：消息已收取，清离线队列；平台-4 落 delivered 事件
    //（ack 清队成功才记——重 ACK 不重记）
    if (logged_in_ && msg.has_ack() && !msg.ack().msg_id().empty()) {
      if (server_.store().ack_offline(msg.ack().msg_id(), account_)) {
        server_.store().append_message_event(msg.ack().msg_id(), "delivered",
                                             account_, "", now_ms());
      }
    }
    break;
  case v1::READ: {
    // 已读上报（T4.3）：接收方已读某条归档消息。留痕后若原发送方在线，
    // 推送 READ_NOTICE（发送方离线则仅留痕，不补投——已读态下次可查）。
    if (!logged_in_ || !msg.has_read() || msg.read().msg_id().empty()) break;
    const std::string target = msg.read().msg_id();
    const std::string original_from = server_.store().message_from(target);
    if (original_from.empty()) break; // 非归档消息（伪造 msg_id）不留痕
    // 重复上报幂等：已记过该读者的不再重发 NOTICE（发送方已知已读）
    bool is_new = true;
    for (const auto& r : server_.store().readers_for(target)) {
      if (r.reader == account_) {
        is_new = false;
        break;
      }
    }
    const std::int64_t rms = now_ms(); // 留痕与通知共用同一时刻
    if (!server_.store().record_read(target, account_, rms)) break;
    if (!is_new) break;
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
    break;
  }
  case v1::PRESENCE_QUERY: {
    // 在线账号表查询（T4.3）：登录后可查；变更推送由服务端主动广播
    if (!logged_in_) break;
    memex::protocol::Message out;
    out.set_type(v1::PRESENCE_DATA);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* p = out.mutable_presence_data();
    for (const auto& a : server_.online_accounts()) p->add_accounts(a);
    send(memex::protocol::encode(out));
    break;
  }
  case v1::FAV_QUERY: {
    // 常用联系人全量（T4.5）：登录后可查，星标置顶＋最近排序
    if (!logged_in_) break;
    memex::protocol::Message out;
    out.set_type(v1::FAV_DATA);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* d = out.mutable_fav_data();
    for (const auto& f : server_.store().fav_list(account_)) {
      auto* e = d->add_entries();
      e->set_peer(f.peer);
      e->set_starred(f.starred);
      e->set_last_ms(f.last_ms);
    }
    send(memex::protocol::encode(out));
    break;
  }
  case v1::FAV_CMD: {
    // 星标／取消／最近上报（T4.5）：受理后全量重推 FAV_DATA
    if (!logged_in_ || !msg.has_fav_cmd()) break;
    const auto& c = msg.fav_cmd();
    if (c.peer().empty()) break;
    if (c.op() == "star") {
      server_.store().fav_star(account_, c.peer(), true);
      log("常用联系人星标：" + c.peer());
    } else if (c.op() == "unstar") {
      server_.store().fav_star(account_, c.peer(), false);
      log("取消星标：" + c.peer());
    } else if (c.op() == "touch") {
      const std::int64_t ts = c.ts_ms() > 0 ? c.ts_ms() : now_ms();
      server_.store().fav_touch(account_, c.peer(), ts);
    } else {
      break;
    }
    memex::protocol::Message out;
    out.set_type(v1::FAV_DATA);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* d = out.mutable_fav_data();
    for (const auto& f : server_.store().fav_list(account_)) {
      auto* e = d->add_entries();
      e->set_peer(f.peer);
      e->set_starred(f.starred);
      e->set_last_ms(f.last_ms);
    }
    send(memex::protocol::encode(out));
    break;
  }
  case v1::PROFILE_CMD: {
    // 个人资料设置（需求批⑪）：目前仅个性签名（op=set_signature，空串=清除）。
    // 登录门＋长度门（120 字，proto 注释同口径）＋保存回执（ok/reason）。
    if (!logged_in_ || !msg.has_profile_cmd()) break;
    const auto& c = msg.profile_cmd();
    memex::protocol::Message out;
    out.set_type(v1::PROFILE_RESULT);
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* r = out.mutable_profile_result();
    r->set_op(c.op());
    if (c.op() != "set_signature") {
      r->set_ok(false);
      r->set_reason("unknown op");
    } else if (utf8_len(c.signature()) > 120) {
      r->set_ok(false);
      r->set_reason("签名过长（上限 120 字）");
    } else {
      r->set_ok(server_.store().set_signature(account_, c.signature()));
      if (!r->ok()) r->set_reason("保存失败");
      log(c.signature().empty() ? "清除个性签名" : "设置个性签名（" +
          std::to_string(utf8_len(c.signature())) + " 字）");
    }
    send(memex::protocol::encode(out));
    break;
  }
  case v1::RECALL: {
    // 撤回：仅置标记不清正文；事件独立留痕；转发给对端会话（本地展示标记）。
    if (!logged_in_ || !msg.has_recall()) break;
    const std::string target = msg.recall().msg_id();
    if (target.empty()) break;
    const std::string original_from = server_.store().message_from(target);
    if (original_from.empty()) {
      log("撤回目标不存在：" + target);
      break;
    }
    if (original_from != account_) {
      log("越权撤回被拒：目标发送方为 " + original_from);
      break;
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
    break;
  }
  case v1::FILE_AUTHZ: {
    // 平台-10 直连文件旁路授权（蓝图§十九）：文件字节不经服务器，判权
    // 必须经服务器——统一 AuthorizationService 四问裁决，seq 原样回带
    // 供客户端关联本次请求。
    if (!logged_in_ || !msg.has_file_authz()) break;
    const auto& fa = msg.file_authz();
    const std::string resource =
        "direct-file:" + (fa.sha256().empty() ? fa.name() : fa.sha256());
    const AuthzQuery q{
        account_, "direct-file:send", resource,
        "to=" + fa.to() + ";size=" + std::to_string(fa.size()) +
            ";forward=" + (fa.forward() ? "1" : "0")};
    const Decision d = server_.file_az().authorize(q);
    // 第四问答复随单携带：发送方生效策略的再转发开关
    const bool forwardable =
        server_.store().resolve_policy(account_).allow_forward_file;
    memex::protocol::Message out;
    out.set_type(v1::FILE_AUTHZ_RESULT);
    out.set_seq(msg.seq());
    out.set_from("server");
    out.set_to(account_);
    out.set_ts_ms(now_ms());
    auto* r = out.mutable_file_authz_result();
    r->set_allowed(d.allowed);
    r->set_reason(d.reason);
    r->set_forwardable(forwardable);
    log("文件旁路授权" + std::string(d.allowed ? "允许" : "拒绝") + "：" +
        account_ + " → " + fa.to() + "（" + fa.name() + "，" + d.reason +
        "）");
    send(memex::protocol::encode(out));
    break;
  }
  default:
    break;
  }
}

// 登录校验 → 记录（成功失败都记）→ 原子化互踢 → 回结果。
void Session::handle_login(const memex::protocol::Message& msg) {
  if (!msg.has_login()) {
    log("登录载荷缺字段，拒绝");
    close();
    return;
  }
  const auto& in = msg.login();
  account_ = in.account();
  device_fingerprint_ = in.device_fingerprint();
  device_name_ = in.device_name();
  kind_ = in.device_kind().empty() ? "desktop" : in.device_kind();

  LoginRecord rec;
  rec.account = account_;
  rec.fingerprint = device_fingerprint_;
  rec.kind = kind_;
  rec.name = device_name_;
  rec.source_ip = remote_;
  rec.version = in.client_version();

  const auto row = server_.store().find_account(account_);
  // 平台-2：口令凭据独立（credentials 表为判登唯一来源；accounts 行内
  // salt/digest 仅作老列兼容保留）
  const auto cred = server_.store().find_credential(account_, "password");
  bool ok = false;
  std::string reason;
  if (!row || !cred) {
    reason = "账号不存在";
    rec.result = "no_account";
  } else {
    const std::string digest =
        pbkdf2_sha256_hex(in.password(), cred->salt_hex, 60000);
    if (digest == cred->digest_hex) {
      // T3.3 设备台账：已停用设备拒绝登录（启停即时生效）
      const auto dev = server_.store().find_device(device_fingerprint_);
      if (dev && !dev->enabled) {
        reason = "设备已停用：请联系管理员";
        rec.result = "device_disabled";
      } else if (!dev && server_.store().resolve_policy(account_)
                               .new_device_approval) {
        // T3.4 新设备审批：首登即建档为停用（待审批），管理员 device approve 后放行
        server_.store().upsert_device(device_fingerprint_, kind_, device_name_,
                                      now_ms());
        server_.store().set_device_enabled(device_fingerprint_, false);
        reason = "新设备待审批：请联系管理员";
        rec.result = "pending_approval";
      } else {
        ok = true;
        rec.result = "ok";
      }
    } else {
      reason = "口令不符";
      rec.result = "bad_password";
    }
  }
  server_.store().add_login_record(rec);

  memex::protocol::Message result;
  result.set_type(v1::LOGIN_RESULT);
  result.set_to(account_);
  result.set_ts_ms(now_ms());
  if (ok) {
    // 同类型单点在线：同账号桌面端互踢、移动端互踢，桌面与手机并存。
    // register_online 在 io 线程内完成顶替，登录回包与互踢不会交错
    const auto kicked =
        server_.register_online(account_, kind_, shared_from_this());
    (void)kicked;
    logged_in_ = true;
    // 首登建档、再登刷新活跃时间（T3.3 台账随使用自动生长）
    server_.store().upsert_device(device_fingerprint_, kind_, device_name_,
                                  now_ms());
    result.mutable_login_result()->set_ok(true);
    result.mutable_login_result()->set_display_name(
        row->display_name);
    log("登录成功（" + account_ + "，" + kind_ + "）");
  } else {
    result.mutable_login_result()->set_ok(false);
    result.mutable_login_result()->set_reason(reason);
    log("登录失败：" + reason);
  }
  send(memex::protocol::encode(result));
  if (ok) {
    // 离线消息补投：登录回执之后推未 ACK 的队列（重复投递由接收端 msg_id 去重）
    for (const auto& blob : server_.store().pending_offline(account_)) {
      deliver_frame(blob);
    }
  }
}

void Session::handle_logout() {
  log("登出（" + account_ + "）");
  server_.unregister_online(account_, kind_, this);
  logged_in_ = false;
  close();
}

void Session::do_write() {
  auto self = shared_from_this();
  asio::async_write(socket_, asio::buffer(write_queue_.front()),
                    [this, self](std::error_code ec, std::size_t) {
                      if (ec) {
                        close();
                        return;
                      }
                      write_queue_.pop_front();
                      if (!write_queue_.empty()) {
                        do_write();
                      } else if (close_after_flush_) {
                        close(); // 互踢：KICK 帧落网后断开
                      }
                    });
}

void Session::close() {
  if (closed_) return;
  closed_ = true;
  std::error_code ignore;
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
  socket_.close(ignore);
}

void Session::log(const std::string& what) const {
  std::cout << "[MEMEX][session " << remote_ << "] " << what << std::endl;
}

} // namespace memex::server
