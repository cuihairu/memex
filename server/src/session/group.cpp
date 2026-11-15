// session.cpp 拆分模块（LAY-015）：群管理·群查询·常用联系人·资料面·文件判权；公共面见 common.hpp，声明在 session.hpp。
#include "common.hpp"

namespace memex::server {
void Session::h_group_cmd(const memex::protocol::Message& msg) { // 群管理命令分发（建群/邀请/退群/公告…）（LAY-015 自 handle_message switch 臂逐字提取）
    // 群管理（T4.1）：建群／拉人／退群／群公告；结果经 GROUP_RESULT 回执
    if (!logged_in_ || !msg.has_group_cmd()) return;
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
        // 群组-1：建群随群落资料（主题/目的均可空＝「未填」，定案①跳过
        // ＝直接建群；老客户端不填→空串行为不变）
        gid = server_.store().create_group(cmd.name(), account_, members,
                                           cmd.topic(), cmd.purpose());
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
          // 邮箱·公告入箱（mailbox.md §I/§IV-P2：公告归公告类——弱提醒
          //＋已读确认留痕（谁/何时可查）；收件人=群成员不含设置者（镜像
          // NOTICE 扇出语义）；dedup=群+内容摘要：同内容重设不重复入箱）
          std::vector<std::string> recipients;
          for (const auto& m : server_.store().group_members(cmd.group_id())) {
            if (m != account_) recipients.push_back(m);
          }
          std::string first_line = cmd.announcement();
          const auto nl = first_line.find('\n');
          if (nl != std::string::npos) first_line.resize(nl);
          if (first_line.size() > 60) first_line = first_line.substr(0, 60) + "…";
          (void)server_.store().mailbox_add(
              "announce", first_line, "弱提醒 · 已读确认台账可查", info->name,
              target,
              "announce:" + std::to_string(cmd.group_id()) + ":" +
                  sha256_hex(cmd.announcement()),
              std::move(recipients), now_ms());
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
    } else if (cmd.op() == "profile") {
      // 群组-1 群资料（主题/目的）后期编辑：权沿群公告先例（群主/管理员，
      // store 层裁决）；两字段整组覆盖＝留空即清除。不属群能力开关七项
      // 词汇表（资料非能力，不扩词表）。
      ok = server_.store().group_set_profile(cmd.group_id(), account_,
                                             cmd.topic(), cmd.purpose());
      if (!ok) {
        reason = "群资料设置失败（仅群主/管理员可设，或群不存在）";
      }
    } else {
      return;
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
}

void Session::h_group_query(const memex::protocol::Message& msg) { // 群信息查询（LAY-015 自 handle_message switch 臂逐字提取）
    // 我加入的群（T4.1）：登录后可查，客户端群列表与公告数据源
    if (!logged_in_) return;
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
      gi->set_topic(g.topic); // 群组-1：空=未填＝客户端不显示该行
      gi->set_purpose(g.purpose);
      for (const auto& m : g.members) gi->add_members(m);
    }
    send(memex::protocol::encode(out));
}

void Session::h_fav_query(const memex::protocol::Message& msg) { // 常用联系人查询（T4.5）（LAY-015 自 handle_message switch 臂逐字提取）
    // 常用联系人全量（T4.5）：登录后可查，星标置顶＋最近排序
    if (!logged_in_) return;
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
}

void Session::h_fav_cmd(const memex::protocol::Message& msg) { // 常用联系人增删（LAY-015 自 handle_message switch 臂逐字提取）
    // 星标／取消／最近上报（T4.5）：受理后全量重推 FAV_DATA
    if (!logged_in_ || !msg.has_fav_cmd()) return;
    const auto& c = msg.fav_cmd();
    if (c.peer().empty()) return;
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
      return;
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
}

void Session::h_profile_cmd(const memex::protocol::Message& msg) { // 个性签名等资料面（LAY-015 自 handle_message switch 臂逐字提取）
    // 个人资料设置（需求批⑪）：目前仅个性签名（op=set_signature，空串=清除）。
    // 登录门＋长度门（120 字，proto 注释同口径）＋保存回执（ok/reason）。
    if (!logged_in_ || !msg.has_profile_cmd()) return;
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
}

void Session::h_file_authz(const memex::protocol::Message& msg) { // 文件判权（LAY-015 自 handle_message switch 臂逐字提取）
    // 平台-10 直连文件旁路授权（蓝图§十九）：文件字节不经服务器，判权
    // 必须经服务器——统一 AuthorizationService 四问裁决，seq 原样回带
    // 供客户端关联本次请求。
    if (!logged_in_ || !msg.has_file_authz()) return;
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
}

} // namespace memex::server
