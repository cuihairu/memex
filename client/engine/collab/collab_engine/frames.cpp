// collab_engine 拆分模块（LAY-013）：入站帧分发 handle_frame（握手回包与全类型路由）——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 collab_engine.hpp。
#include "common.hpp"

namespace memex::client {
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
      gj["topic"] = g.topic(); // 群组-1：空=未填＝展示层不显示该行
      gj["purpose"] = g.purpose();
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

} // namespace memex::client
