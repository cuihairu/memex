// 引擎接线（LAY-002 拆分）：wire_engines（直连引擎信号）与
// wire_collab（协作引擎信号）接入界面。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QJsonObject>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include "notify_center.hpp"
#include "notify_prefs.hpp"
namespace memex::client {

void MainWindow::wire_engines() {
  connect(&direct_engine_, &DirectEngine::peers_changed, this, [this] {
    cross_sweep(); // T4.2：对端离线／对端登录 → 跨态会话闭环
    update_banner(); // 跨态判定随宣告刷新（对端登录态变化即时反映）
    refresh_devices();
    show_status(status_hint_); // 在线数变化，刷新引擎态前缀
  });
  connect(&direct_engine_, &DirectEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms,
                 quint64 seq) {
            cross_touch(from); // T4.2：首条收发即上报会话建立
            if (from == current_peer_ &&
                current_kind_ == QStringLiteral("direct")) {
              append_message(from, text, ts_ms, false,
                             QStringLiteral("direct"));
              // 已读上报（需求批⑦·已读灰勾单）：会话开着即视为已览，
              // 是否上报受会话/全局已读回执开关裁决（与协作态同款）
              if (seq != 0 && read_receipts_enabled(from))
                direct_engine_.mark_read(from.toStdString(), seq);
            } else {
              // 未读角标（原型行 n-count）：非当前会话到达即计数并重渲行
              ++unread_counts_[from];
              refresh_devices();
              show_status(QStringLiteral("来自 %1 的新消息").arg(from));
            }
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何直连新消息
  // 都通知，不随分支改写而丢失）
  connect(&direct_engine_, &DirectEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64, quint64) {
            if (!isActiveWindow()) {
              event_notify(NotifyPrefs::load().popup_message,
                           SoundEvent::Message, QStringLiteral("新消息"),
                           QStringLiteral("来自 %1：%2").arg(from, text));
              alert_attention(); // 任务栏/窗口闪烁（开关与合并窗在实现内）
            }
          });
  connect(&direct_engine_, &DirectEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            set_delivery_state(
                ok ? QStringLiteral("已送达（seq %1）").arg(seq)
                   : QStringLiteral("送达失败（seq %1）").arg(seq));
            // 直连送达回执落气泡（需求批⑦·已读灰勾单）：store 侧由引擎
            // 落 delivered，界面按发送序号命中行改灰单勾（失败不冒充送达）
            if (ok) apply_direct_receipt(seq, QStringLiteral("delivered"));
          });
  // 直连已读上报回执（需求批⑦·已读灰勾单）：store 侧由引擎落 read
  //（只升不降），界面把灰单勾升为灰双勾；状态行同协作态口径提示
  connect(&direct_engine_, &DirectEngine::message_read, this,
          [this](quint64 seq, const QString& reader) {
            apply_direct_receipt(seq, QStringLiteral("read"));
            set_delivery_state(QStringLiteral("对方已读 ✓✓（%1）").arg(reader));
            // 「盯」：READ 落终态即查盯住集合并触发（无盯则静默穿过）
            watch_read_fired(direct_watch_key(reader, seq), reader);
          });
  connect(&direct_engine_, &DirectEngine::file_progress, this,
          [this](const QString& id, quint64 done, quint64 total) {
            update_tx_progress(id, done, total); // 传输台账（文件传输面）
            if (total == 0) return;
            // 状态栏节流：按 5% 步进刷新
            const int pct = static_cast<int>(done * 100 / total);
            static int last_pct = -1;
            if (pct / 5 != last_pct / 5 || pct == 100) {
              last_pct = pct;
              show_status(QStringLiteral("文件传输进度：%1%").arg(pct));
            }
          });
  // 振屏（需求批⑥）：直连域到达——抖窗＋提示音，当前会话渲染系统行
  connect(&direct_engine_, &DirectEngine::nudge_received, this,
          [this](const QString& from, quint64 /*seq*/, qint64 ts_ms) {
            on_nudge_received(from, ts_ms, QStringLiteral("direct"));
          });
  connect(&direct_engine_, &DirectEngine::file_finished, this,
          [this](const QString& id, bool ok, const QString& error) {
            last_file_error_ = error; // 授权回执验收面（失败原因留存可断言）
            finish_tx_entry(id, ok, error); // 传输台账（文件传输面）
            show_status(ok ? QStringLiteral("文件已送达")
                           : QStringLiteral("文件传输中断：%1").arg(error));
            // 文件传输成功→弹通知窗（事件开关组；默认关＝成功是常态）
            if (ok) {
              event_notify(NotifyPrefs::load().popup_transfer_done,
                           SoundEvent::File, QStringLiteral("文件传输成功"),
                           id.right(8));
            }
            // T4.4：截图临时文件在传输结束后清理（成功／失败均清）
            const auto shot = shot_paths_.constFind(id);
            if (shot != shot_paths_.cend()) {
              QFile::remove(shot.value());
              shot_paths_.erase(shot);
            }
          });
  // 需求批⑤ 文件夹作业聚合：文件粒度计数刷状态（引擎逐文件串行）
  connect(&direct_engine_, &DirectEngine::directory_progress, this,
          [this](const QString& job_id, quint64 done, quint64 total) {
            const auto it = folder_jobs_.find(job_id);
            if (it == folder_jobs_.end()) return;
            it->done = done;
            it->total = total;
            // 传输台账（文件传输面）：文件夹作业按文件粒度计数
            for (TransferEntry& e : transfers_) {
              if (e.id == job_id && e.is_folder) {
                e.files_done = done;
                e.files_total = total;
                break;
              }
            }
            if (sidebar_mode_ == QStringLiteral("transfers"))
              refresh_transfers_view();
            show_status(QStringLiteral("文件夹传输：%1（第 %2/%3 个文件）")
                            .arg(it->name, QString::number(done + 1),
                                 QString::number(total)));
          });
  connect(&direct_engine_, &DirectEngine::directory_finished, this,
          [this](const QString& job_id, bool ok) {
            const auto it = folder_jobs_.find(job_id);
            const QString name = it == folder_jobs_.end() ? job_id : it->name;
            folder_jobs_.remove(job_id);
            finish_tx_entry(job_id, ok, QString()); // 传输台账（文件传输面）
            if (ok) {
              append_system_line(
                  QStringLiteral("[文件夹] %1 发送完成").arg(esc(name)));
              show_status(QStringLiteral("文件夹发送完成：%1").arg(name));
            } else {
              show_status(
                  QStringLiteral(
                      "文件夹发送失败：%1（已传部分保留，重发自动续传）")
                      .arg(name));
            }
          });
  connect(&direct_engine_, &DirectEngine::file_received, this,
          [this](const QString& id, const QString& peer_id,
                 const QString& path) {
            last_received_file_ =
                peer_id + QLatin1Char('|') + path; // 验收面
            // 传输台账（文件传输面）：收侧落账＝已完成（接收端整文件
            // SHA-256 校验通过才落盘，foot 如实标校验）
            {
              const Peer rp = direct_engine_.peer(peer_id.toStdString());
              const bool via_collab = !rp.account.empty();
              TransferEntry e;
              e.id = id;
              e.name = QFileInfo(path).fileName();
              e.kind = via_collab ? QStringLiteral("collab")
                                  : QStringLiteral("direct");
              e.peer = via_collab
                           ? QString::fromStdString(rp.account)
                           : peer_id;
              e.path = path;
              e.bytes_done = e.bytes_total =
                  static_cast<quint64>(QFileInfo(path).size());
              const qint64 now = QDateTime::currentMSecsSinceEpoch();
              e.ts_ms = now;
              e.done_ms = now;
              e.state = 1; // 已完成
              e.outgoing = false;
              e.peer_display = tx_peer_display(e.kind, e.peer);
              track_transfer(e);
            }
            // 图片消息（需求批④）：图片扩展名且属于当前会话＝气泡内直接
            // 渲染图片；否则按普通文件系统行（口径不混淆）。
            // 会话归属按发送方设备号（peer_id）判，transfer id 是 UUID
            // 不指向任何会话。
            static const QStringList kImageExt = {
                QStringLiteral("png"), QStringLiteral("jpg"),
                QStringLiteral("jpeg"), QStringLiteral("gif"),
                QStringLiteral("bmp"), QStringLiteral("webp")};
            const bool is_image =
                kImageExt.contains(QFileInfo(path).suffix().toLower());
            bool in_session = false;
            if (is_image) {
              if (current_kind_ == QStringLiteral("direct")) {
                in_session = (peer_id == current_peer_);
              } else if (current_kind_ == QStringLiteral("collab")) {
                const Peer p = direct_engine_.peer(peer_id.toStdString());
                in_session =
                    !p.account.empty() &&
                    QString::fromStdString(p.account) == current_peer_;
              }
            }
            if (in_session) {
              append_image_message(id, path,
                                   QDateTime::currentMSecsSinceEpoch(),
                                   /*outgoing=*/false,
                                   QStringLiteral("direct"));
              show_status(QStringLiteral("图片消息已接收：%1").arg(path));
            } else {
              append_file_card(
                  id, QFileInfo(path).fileName(),
                  QStringLiteral("%1 · 直传 · 已接收")
                      .arg(human_size(QFileInfo(path).size())),
                  QDateTime::currentMSecsSinceEpoch(),
                  /*outgoing=*/false);
              show_status(QStringLiteral("文件已接收：%1").arg(path));
            }
            // 收到文件→弹通知窗（与消息同款激活门：激活中不打扰）
            if (!isActiveWindow()) {
              event_notify(NotifyPrefs::load().popup_file_arrive,
                           SoundEvent::File,
                           is_image ? QStringLiteral("收到图片")
                                    : QStringLiteral("收到文件"),
                           QFileInfo(path).fileName());
            }
          });

  // 平台-10 文件旁路授权门（蓝图§十九）：文件不经服务器，判权必须经
  // 服务器——发送前先问服务端统一 AuthorizationService（四问：可发/
  // 可收/跨部门/再转发），裁决回流关单。协作未登录＝服务端不可达域，
  // 协作引擎本地即拒（fail-closed：文本是归档面可降级，文件是权限面
  // 不可绕权）。
  direct_engine_.set_file_authorizer(
      [this](const memex::client::FileAuthzRequest& r) {
        collab_engine_.file_authz(r.req, QString::fromStdString(r.to_account),
                                  r.size, QString::fromStdString(r.name),
                                  QString(), r.forward);
      });
  connect(&collab_engine_, &CollabEngine::file_authz_result, this,
          [this](quint64 req, bool allowed, const QString& reason, bool) {
            direct_engine_.file_authz_resolved(req, allowed, reason);
          });
}

// 协作信号接入界面（T2.4）：形态切换提示、降级提示、协作消息渲染。
// 降级口径：服务端不可达＝回落直连态，必须明示「消息不进归档」（A11）。
void MainWindow::wire_collab() {
  connect(&collab_engine_, &CollabEngine::logged_in, this,
          [this](const QString& account, const QString& display) {
            // T4.2：本端登录态写进发现宣告——对端据此判定跨态与显示账号
            direct_engine_.set_collab_account(account.toStdString());
            collab_login_ms_ = QDateTime::currentMSecsSinceEpoch(); // A8 归档起点
            act_collab_logout_->setEnabled(true);
            collab_was_logged_in_ = true;
            kick_text_.clear(); // T4.3：新会话清除旧互踢提示
            collab_degraded_ = false; // 平台-7：登录恢复即退出降级态
            // 在线表以服务端推送为准（登录广播先于 LOGIN_RESULT 到达，
            // 推送已含自己——此处不清，否则会擦掉刚收到的推送）。
            seed_collab_peers();
            update_banner();
            refresh_devices();
            append_system_line(
                QStringLiteral("已切换协作态（%1）：此后消息经服务端转发并"
                               "全量归档")
                    .arg(esc(display)));
            if (current_kind_ == QStringLiteral("direct")) {
              // A8 客户端面：跨态直连会话归档自行（此前直连消息不进归档）
              append_system_line(QStringLiteral(
                  "归档自 %1（本次协作态登录时刻）：此前跨态直连消息不进"
                  "归档，转协作会话后自登录时刻起计入")
                      .arg(QDateTime::currentDateTime().toString(
                          QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
            }
            show_status(QStringLiteral("协作态已登录：%1").arg(display));
            // 登录即拉组织架构（策略与建群数据源）与群列表（T3.4／T4.1）
            collab_engine_.query_org();
            collab_engine_.query_groups();
            // 需求批⑫：文件面同源自动登录（口令复用；端口走 QSettings，
            // 未配置＝静默跳过——头像操作时再明示失败）
            files_ensure_login();
          });
  connect(&collab_engine_, &CollabEngine::login_failed, this,
          [this](const QString& reason) {
            act_collab_logout_->setEnabled(false);
            if (reason == QStringLiteral("无法连接服务器")) {
              // 平台-7 降级：登录尝试因服务端不可达而失败＝本应归档而
              // 不得（用户意图是协作态）——与曾否登录无关；显式登出回
              // 直连不算降级（logout_collab 清标志）。标志先置再刷横幅
              collab_degraded_ = true;
              update_banner();
              append_system_line(QStringLiteral(
                  "服务端不可达：已回落直连态，消息不进归档"
                  "（点对点传输，仍可正常收发）"));
              show_status(
                  QStringLiteral("服务端不可达，已回落直连态：消息不进归档"));
            } else {
              update_banner();
              show_status(QStringLiteral("协作登录失败：%1").arg(reason));
            }
          });
  connect(&collab_engine_, &CollabEngine::connection_lost, this, [this] {
    // 平台-7：登录中断线＝DEGRADED（本应归档而服务端不可达）；被顶下线
    // （kicked 先行清了登录态）与服务端无涉，不算降级
    if (collab_was_logged_in_) collab_degraded_ = true;
    collab_was_logged_in_ = false;
    online_accounts_.clear(); // 断线即未知在线态（重连推送后刷新）
    update_banner();
    refresh_devices();
    show_status(QStringLiteral(
        "服务端连接断开，直连态仍可用：消息不进归档（自动重连中）"));
  });
  connect(&collab_engine_, &CollabEngine::reconnected, this, [this] {
    collab_was_logged_in_ = true;
    update_banner();
    refresh_devices();
    show_status(QStringLiteral("协作态已恢复：消息重新进入归档"));
  });
  connect(&collab_engine_, &CollabEngine::kicked, this,
          [this](const QString& reason, const QString& replaced_by) {
            cross_end_all(); // T4.2：被踢即会话终点（此刻引擎尚未断开，
                             // CROSS_LOG 帧随断开前队列刷出）
            collab_was_logged_in_ = false;
            online_accounts_.clear(); // 在线表随会话失效（重登后推送刷新）
            act_collab_logout_->setEnabled(false);
            // kicked 信号先于引擎 logged_in_=false 到达：横幅与列表刷新
            // 排队到状态落定后（否则仍按登录态渲染协作横幅，状态说谎）。
            QMetaObject::invokeMethod(
                this,
                [this] {
                  update_banner();
                  refresh_devices();
                },
                Qt::QueuedConnection);
            // T4.3 桌面单点在线提示：常驻文案（测试断言面）＋非模态弹窗
            //（模态会阻塞自动化验收；关闭按钮常驻可查）。
            kick_text_ = QStringLiteral("已被顶替下线（%1）：%2")
                             .arg(replaced_by, reason);
            set_delivery_state(kick_text_);
            // T4.7：互踢同时走系统通知（托盘气泡；无托盘仅记录不断言面）
            tray_notify(QStringLiteral("已被顶替下线"), kick_text_);
            auto* box = new QMessageBox(
                QMessageBox::Warning, QStringLiteral("已被顶替下线"),
                kick_text_ + QStringLiteral("\n同账号在另一台设备登录，"
                                            "本机会话已断开（消息不进归档）。"),
                QMessageBox::Ok, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setModal(false);
            box->show();
          });
  connect(&collab_engine_, &CollabEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms,
                 const QString& msg_id) {
            if (current_kind_ == QStringLiteral("collab") &&
                from == current_peer_) {
              // 本地库已由引擎落库（同库），这里只做界面渲染
              append_message(from, text, ts_ms, false,
                             QStringLiteral("collab"));
              // T4.3：当前会话开着=已读，立即上报（发送方收 READ_NOTICE）；
              // 需求批⑦：是否上报受会话/全局已读回执开关裁决
              if (!msg_id.isEmpty() && read_receipts_enabled(from))
                collab_engine_.mark_read(msg_id);
            } else {
              // 未读角标（非当前会话；本分支尾统一 refresh_devices 重渲行）
              ++unread_counts_[from];
              show_status(QStringLiteral("来自 %1 的协作消息").arg(from));
            }
            collab_peers_.insert(from);
            refresh_devices();
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何协作新消息
  // 都通知，不随分支改写而丢失）
  connect(&collab_engine_, &CollabEngine::message_received, this,
          [this](const QString& from, const QString&, qint64, const QString&) {
            // T4.10：通知（发送方＝kNoticeSender）由通知中心分级自管，不在此
            // 重复托盘气泡（普通默认不弹，重要／紧急由通知中心直发）
            if (from ==
                QString::fromUtf8(memex::protocol::kNoticeSender)) {
              return;
            }
            if (!isActiveWindow()) {
              event_notify(NotifyPrefs::load().popup_message,
                           SoundEvent::Message, QStringLiteral("新消息"),
                           QStringLiteral("来自 %1 的协作消息").arg(from));
              alert_attention();
            }
          });
  connect(&collab_engine_, &CollabEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            // 已读是终态：后到的受理回执不回退状态（自发自收回环时先已读后回执）
            if (delivery_text_.contains(QStringLiteral("已读"))) return;
            set_delivery_state(
                ok ? QStringLiteral("协作消息已送达（seq %1）").arg(seq)
                   : QStringLiteral("协作消息送达超时（seq %1）").arg(seq));
          });
  // 振屏（需求批⑥）：协作域到达——抖窗＋提示音，当前会话渲染系统行
  connect(&collab_engine_, &CollabEngine::nudge_received, this,
          [this](const QString& from, qint64 ts_ms) {
            on_nudge_received(from, ts_ms, QStringLiteral("collab"));
          });
  connect(&collab_engine_, &CollabEngine::org_received, this,
          [this](const QString& org_json) {
            last_org_json_ = org_json;
            // 需求批⑪：成员签名入缓存（会话列表悬浮 tooltip 数据源）
            member_signatures_.clear();
            // 需求批⑩：成员在线时长入缓存（tooltip 与个人资料展示面）
            member_online_.clear();
            // 需求批⑫：成员头像版本戳入缓存（0=未设置——默认头像；ver 变
            // 即排队下载 256 档，本地缩放缓存四档）
            member_avatar_ver_.clear();
            const nlohmann::json j = nlohmann::json::parse(
                org_json.toStdString(), nullptr, false);
            if (!j.is_discarded() && j.contains("members")) {
              for (const auto& m : j["members"]) {
                const QString a = QString::fromStdString(
                    m.value("account", std::string{}));
                if (!a.isEmpty()) {
                  member_signatures_.insert(
                      a, QString::fromStdString(
                             m.value("signature", std::string{})));
                  member_online_.insert(
                      a, QStringLiteral("在线时长：今日 %1 · 本周 %2 · 本月 %3")
                             .arg(format_online_s(m.value("online_day_s", 0)),
                                  format_online_s(m.value("online_week_s", 0)),
                                  format_online_s(
                                      m.value("online_month_s", 0))));
                  member_avatar_ver_.insert(a, m.value("avatar_ver", 0));
                }
              }
            }
            for (auto it = member_avatar_ver_.constBegin();
                 it != member_avatar_ver_.constEnd(); ++it) {
              queue_avatar_download(it.key(), it.value());
            }
            apply_policy(org_json);
            refresh_devices(); // 签名 tooltip 随数据到达即时刷新
            if (org_dialog_pending_) org_dialog_pending_ = false;
            if (sidebar_mode_ == QStringLiteral("org")) {
              rebuild_org_view(); // 组织架构面在屏：随数据即时刷新
            } else {
              show_status(QStringLiteral("组织架构已更新"));
            }
          });
  connect(&collab_engine_, &CollabEngine::fav_received, this,
          [this](const QString& fav_json) { apply_favs(fav_json); });
  // 需求批⑪：签名回执——成功即重拉组织架构（缓存与悬浮 tooltip 刷新）
  connect(&collab_engine_, &CollabEngine::profile_result, this,
          [this](bool ok, const QString& reason, const QString&) {
            if (ok) {
              collab_engine_.query_org();
              show_status(QStringLiteral("个性签名已保存"));
            } else {
              show_status(QStringLiteral("签名设置失败（%1）").arg(reason));
            }
          });
  // —— T4.1 群聊 ——
  connect(&collab_engine_, &CollabEngine::group_result, this,
          [this](bool ok, const QString& reason, const QString& op,
                 quint64 group_id) {
            if (ok) {
              show_status(QStringLiteral("群操作成功（%1，群 %2）")
                              .arg(op, QString::number(group_id)));
              // 群组-1：资料（主题/目的）落定后重拉群列表对齐 meta 展示
              if (op == QStringLiteral("profile")) {
                collab_engine_.query_groups();
              }
            } else {
              show_status(QStringLiteral("群操作失败（%1）：%2").arg(op, reason));
            }
          });
  connect(&collab_engine_, &CollabEngine::groups_received, this,
          [this](const QString& groups_json) { apply_groups(groups_json); });
  connect(&collab_engine_, &CollabEngine::group_message_received, this,
          [this](const QString& group_key, const QString& sender,
                 const QString& text, qint64 ts_ms, const QString& msg_id) {
            if (current_kind_ == QStringLiteral("group") &&
                group_key == current_peer_) {
              append_message(sender, text, ts_ms, false,
                             QStringLiteral("collab"));
              // T4.3：群会话开着=已读（发送方按读者逐条收 READ_NOTICE）；
              // 需求批⑦：开关裁决同单聊
              if (!msg_id.isEmpty() && read_receipts_enabled(group_key))
                collab_engine_.mark_read(msg_id);
            } else {
              // 未读角标：群键计数（打开该群即清）
              ++unread_counts_[group_key];
              refresh_devices();
              const QString gname = groups_.value(group_key.mid(6).toULongLong())
                                        .name;
              show_status(QStringLiteral("来自群「%1」%2 的消息")
                              .arg(gname.isEmpty() ? group_key : gname, sender));
            }
          });
  // T4.7 系统通知独立接入（与渲染分支解耦：窗口未激活时任何群新消息
  // 都通知，不随分支改写而丢失）
  connect(&collab_engine_, &CollabEngine::group_message_received, this,
          [this](const QString& group_key, const QString& sender,
                 const QString&, qint64, const QString&) {
            // T4.10：群通知同样由通知中心分级自管（不重复托盘气泡）
            if (sender ==
                QString::fromUtf8(memex::protocol::kNoticeSender)) {
              return;
            }
            if (!isActiveWindow()) {
              const QString gname =
                  groups_.value(group_key.mid(6).toULongLong()).name;
              event_notify(NotifyPrefs::load().popup_message,
                           SoundEvent::Message, QStringLiteral("新消息"),
                           QStringLiteral("来自群「%1」%2 的消息")
                               .arg(gname.isEmpty() ? group_key : gname,
                                    sender));
              alert_attention();
            }
          });
  // —— T4.10 分级推送接入：通知中心按个人偏好裁决普通／重要／紧急三级
  // （普通＝仅站内消息已渲染；重要＝托盘强提醒；紧急＝置顶确认弹窗）——
  connect(&collab_engine_, &CollabEngine::notice_received,
          &NotificationCenter::instance(), &NotificationCenter::on_notice);
  // 邮箱（mailbox.md）：NOTICE 推达＝「有新事」信号，仅触发未读计数轻拉
  // （条目只拉不推——不新增推送链；未登录文件面时静默跳过）
  connect(&collab_engine_, &CollabEngine::notice_received, this,
          [this](const QString&, const QString&, const QString&, int,
                 const QString&, qint64, const QString&) {
            refresh_mailbox_badge();
          });
  connect(&NotificationCenter::instance(),
          &NotificationCenter::want_tray_notify, this,
          [this](const QString& title, const QString& text) {
            tray_notify(title, text); // 通知中心裁决出的强提醒，不看激活态
          });

  // —— T4.3 已读回执与在线状态 ——
  connect(&collab_engine_, &CollabEngine::message_read, this,
          [this](const QString& msg_id, const QString& reader,
                 qint64 /*read_ms*/) {
            // READ_NOTICE 只发给原发送方：在此处即「对方已读我发出的消息」
            set_delivery_state(
                QStringLiteral("对方已读 ✓✓（%1）").arg(reader));
            // 需求批⑦：消息旁状态可见——落行重渲（引擎已落本地库，
            // 历史重载同构）
            apply_message_receipt(msg_id, QStringLiteral("read"));
            // 「盯」：协作域同口径（c:<msg_id> 键触发）
            watch_read_fired(collab_watch_key(msg_id), reader);
          });
  // —— 需求批⑦ 送达级回执：状态行＋消息旁 ✓ ——
  connect(&collab_engine_, &CollabEngine::message_delivered, this,
          [this](const QString& msg_id, const QString& delivered_to,
                 qint64 /*delivered_ms*/) {
            set_delivery_state(
                QStringLiteral("已送达对方 ✓（%1）").arg(delivered_to));
            apply_message_receipt(msg_id, QStringLiteral("delivered"));
          });
  // —— 需求批⑦ 回执态补查回包：逐条对齐当前会话的行 ——
  connect(&collab_engine_, &CollabEngine::receipts_received, this,
          [this](const QString& receipts_json) {
            const QJsonDocument doc = QJsonDocument::fromJson(
                receipts_json.toUtf8());
            if (!doc.isArray()) return;
            for (const QJsonValue& v : doc.array()) {
              const QJsonObject e = v.toObject();
              const QString mid = e.value(QStringLiteral("msg_id")).toString();
              if (mid.isEmpty()) continue;
              const bool read = e.value(QStringLiteral("readers"))
                                    .toArray().isEmpty() == false;
              const bool delivered = e.value(QStringLiteral("delivered_to"))
                                         .toArray().isEmpty() == false;
              if (!read && !delivered) continue; // 未送达不标注
              apply_message_receipt(
                  mid, read ? QStringLiteral("read")
                            : QStringLiteral("delivered"));
              // 「盯」：补拉对齐出的已读同样触发（离线期 READ 的兜底面）
              if (read) watch_read_fired(collab_watch_key(mid));
            }
          });
  connect(&collab_engine_, &CollabEngine::presence_changed, this,
          [this](const QStringList& accounts) {
            // 联系人上线通知（用户令 2026-10-08）：推送前对比新增账号，
            // 逐个按开关弹（自己也在广播里，排除）
            const QStringList joined =
                presence_joined(online_accounts_.values(), accounts);
            // 逐个插入（不依赖 QSet 区间构造的可移植性）
            online_accounts_.clear();
            for (const QString& a : accounts) online_accounts_.insert(a);
            const QString me = collab_engine_.account();
            const NotifyPrefs p = NotifyPrefs::load();
            for (const QString& a : joined) {
              if (a == me) continue;
              event_notify(p.popup_peer_online, SoundEvent::Online,
                           QStringLiteral("联系人上线"), a);
            }
            refresh_devices(); // 协作会话行在线标识随推送刷新
          });
  // —— 需求批⑧ 日期范围补拉回包：落库已完成（引擎），此处只管重渲 ——
  connect(&collab_engine_, &CollabEngine::history_received, this,
          [this](const QString& peer, qint64, qint64, int) {
            if (peer != current_peer_ || chat_panel_->isHidden()) return;
            if (filter_from_ms_ > 0 || filter_until_ms_ > 0) {
              render_filtered_history(); // 筛选窗内新行并入
              return;
            }
            if (pending_jump_.isValid()) {
              render_open_history(); // 跳转日数据到齐——重渲后再定位
              scroll_to_day(pending_jump_);
            }
            // 既不筛选也无挂起跳转：补拉行静默落库（下次打开/筛选可见）
          });
}

} // namespace memex::client
