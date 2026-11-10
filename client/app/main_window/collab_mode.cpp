// 两态切换公共入口（LAY-002 拆分）：协作登录/登出、本人资料面、
// 生效策略解析、归档提示条与登录后历史会话补入。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include "collab_defaults.hpp"

#include "message_search_dialog.hpp"
#include <core/local_store.hpp>

#include "theme.hpp"
namespace memex::client {

// —— T2.4 模式切换公共入口 ——

void MainWindow::login_collab(const QString& host, quint16 port,
                              const QString& account,
                              const QString& password) {
  // 域名解析覆盖（部署批 424）：命中规则回 IP，未命中原样；collab_host_
  // 存已解析值，文件面同源回退（空 collab_host_ 读设置处）不再二次处理
  const QString resolved = resolve_conn_host(host);
  collab_host_ = resolved; // 需求批⑫：文件面同源主机默认（端口走 QSettings）
  collab_engine_.login(resolved, port, account, password);
}

void MainWindow::logout_collab() {
  cross_end_all();          // T4.2：登出前闭环全部跨态会话（end 帧先于 LOGOUT）
  collab_engine_.logout();
  direct_engine_.set_collab_account(""); // 登出即广播「未登录」（对端即时闭环）
  act_collab_logout_->setEnabled(false);
  collab_was_logged_in_ = false;
  collab_degraded_ = false; // 平台-7：显式登出＝主动回直连，非降级
  online_accounts_.clear(); // T4.3：登出即未知在线态
  delivery_text_.clear();   // T4.3：发送状态随会话失效
  // 需求批⑫：文件面会话随登出作废（在途头像挂起态一并清）
  if (files_client_) files_client_->logout();
  avatar_pick_pending_ = false;
  avatar_pending_accounts_.clear();
  update_banner();
  append_system_line(QStringLiteral(
      "已登出协作态，回到直连态：消息不进归档（本地历史保留，合并展示）"));
  show_status(QStringLiteral("协作态已登出：当前直连态，消息不进归档"));
  refresh_devices();
}

QString MainWindow::self_account() const {
  return collab_engine_.account();
}

QString MainWindow::self_display_name() const {
  const QString n = roster_display(collab_engine_.account());
  return n.isEmpty() ? collab_engine_.account() : n;
}

QPixmap MainWindow::self_avatar(int size) const {
  const QString acc = collab_engine_.account();
  if (acc.isEmpty()) return {};
  // 与 rail 自头像同源（main_window.cpp:7907 同式）
  return avatar_for(acc, member_avatar_ver_.value(acc), size);
}

void MainWindow::open_collab_peer(const QString& account) {
  if (account.isEmpty()) return;
  if (account.startsWith(QStringLiteral("group:"))) { // 历史群会话键
    open_group(account);
    return;
  }
  collab_peers_.insert(account);
  refresh_devices();
  open_chat(QStringLiteral("collab"), account);
}

// 解析本人生效策略：本人部门 → 逐级上级部门 → 全局行 → 默认宽松
void MainWindow::apply_policy(const QString& org_json) {
  nlohmann::json j =
      nlohmann::json::parse(org_json.toStdString(), nullptr, false);
  if (j.is_discarded()) return;
  // 本人部门路径
  QString dept;
  const QString me = collab_engine_.account();
  if (j.contains("members")) {
    for (const auto& m : j["members"]) {
      if (QString::fromStdString(m.value("account", std::string{})) == me) {
        dept = QString::fromStdString(m.value("department_path", std::string{}));
        break;
      }
    }
  }
  const auto match = [&](const QString& path) -> const nlohmann::json* {
    if (!j.contains("policies")) return nullptr;
    for (const auto& p : j["policies"]) {
      if (QString::fromStdString(p.value("department_path", std::string{})) ==
          path) {
        return &p;
      }
    }
    return nullptr;
  };
  const nlohmann::json* row = nullptr;
  QString path = dept;
  while (!path.isEmpty()) {
    if (const nlohmann::json* hit = match(path)) {
      row = hit;
      break;
    }
    const int pos = path.lastIndexOf(QChar('/'));
    if (pos <= 0) break;
    path.truncate(pos);
  }
  if (!row) row = match(QString()); // 全局行
  if (row) {
    anonymous_allowed_ = row->value("allow_anonymous", true);
    cross_state_allowed_ = row->value("allow_cross_state", true);
  } else {
    anonymous_allowed_ = true; // 未配置＝默认宽松
    cross_state_allowed_ = true;
  }
}

bool MainWindow::collab_logged_in() const {
  return collab_engine_.is_logged_in();
}

// —— T4.1 群聊 ——

QString MainWindow::groups_json() const { return last_groups_json_; }

// 服务端无群列表推送（GROUP_DATA 仅应答 GROUP_QUERY），群变更他端发起时
// 由此重拉（登录时已自动拉一次）
void MainWindow::refresh_groups() { collab_engine_.query_groups(); }

// 组织架构全部账号（建群／拉人数据源；未拉到组织架构时为空，弹窗走手输）
QStringList MainWindow::org_accounts() const {
  QStringList out;
  nlohmann::json j = nlohmann::json::parse(last_org_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.contains("members")) return out;
  for (const auto& m : j["members"]) {
    const QString a =
        QString::fromStdString(m.value("account", std::string{}));
    if (!a.isEmpty()) out << a;
  }
  return out;
}

// —— T3.1 组织架构：登录后向服务端查询，管理端维护即生效到客户端 ——

void MainWindow::request_org() {
  if (collab_engine_.is_logged_in()) collab_engine_.query_org();
}

QString MainWindow::org_json() const { return last_org_json_; }

void MainWindow::update_banner() {
  // 协作单聊与服务端群聊都走服务端转发归档（T4.1 后者同口径）
  const bool collab_session =
      (current_kind_ == QStringLiteral("collab") ||
       current_kind_ == QStringLiteral("group")) &&
      collab_engine_.is_logged_in();
  const ThemeTokens& t = tk();
  if (collab_session) {
    // 群公告占横幅（原型 desktop-group.html .chat-banner.collab-ok：品牌底
    // 承公告）；无公告走通用归档口径。模型只存公告文本不存更新时刻——署
    // 群主不带「更新于」（台账近似项）
    const GroupEntry* g = current_kind_ == QStringLiteral("group")
                              ? current_group()
                              : nullptr;
    if (g && !g->announcement.isEmpty()) {
      QString owner = roster_display(g->owner);
      if (owner.isEmpty()) owner = g->owner;
      banner_->setStyleSheet(
          QStringLiteral("background:%1; color:%2; font-size:12px; "
                         "padding:6px 10px;")
              .arg(t.brand_wash.name(), t.brand_wash_text.name()));
      banner_->setText(QStringLiteral("　📢 群公告（群主：%1）：%2")
                           .arg(owner, g->announcement));
    } else {
      banner_->setStyleSheet(
          QStringLiteral("background:%1; color:%2; font-size:12px; "
                         "padding:6px 10px;")
              .arg(t.success_wash.name(), t.success_text.name()));
      banner_->setText(QStringLiteral(
          "　✔ 协作态会话：消息经服务端转发并全量归档；撤回仅改显示，"
          "服务端保留原文与撤回记录"));
    }
  } else {
    banner_->setStyleSheet(
        QStringLiteral("background:%1; color:%2; font-size:12px; "
                       "padding:6px 10px;")
            .arg(t.brand_wash.name(), t.brand_wash_text.name()));
    // 平台-7：DEGRADED（本应归档但服务端不可达）恒先判——全面标识
    if (collab_degraded_) {
      banner_->setText(QStringLiteral(
          "　⚠ 已降级直连态（未归档通信）：服务端不可达，消息不进归档"
          "（点对点传输，仅保存在双方本机；恢复后协作消息自动补传归档）"));
    } else {
      // T4.2 跨态会话（恰一边登录）：固定「未归档」标识，常驻不可关闭（A7）
      bool cross = false;
      if (current_kind_ == QStringLiteral("direct")) {
        cross = is_cross_state(current_peer_);
      } else if (current_kind_ == QStringLiteral("dgroup")) {
        for (const QString& dev : dgroup_members_.value(current_peer_)) {
          if (is_cross_state(dev)) {
            cross = true;
            break;
          }
        }
      }
      if (cross) {
        banner_->setText(QStringLiteral(
            "　⚠ 跨态会话 · 未归档：恰一边登录协作态，消息点对点传输不进归档"
            "（与未登录终端的会话标记常驻不可关闭）"));
      } else {
        banner_->setText(QStringLiteral(
            "　⚠ 直连态会话：消息点对点传输，不经过服务器——消息不进归档"
            "（服务端无任何记录，仅保存在双方本机）"));
      }
    }
  }
  // 平台-7：输入框提示随态——降级态明示「不进归档」，群聊走原型占位
  //（「输入消息，@ 成员可加提醒…」），其余常规
  if (collab_degraded_) {
    input_box_->setPlaceholderText(QStringLiteral(
        "未归档通信（已降级直连）：此会话消息不进服务端归档"));
  } else if (current_kind_ == QStringLiteral("group")) {
    input_box_->setPlaceholderText(
        QStringLiteral("输入消息，@ 成员可加提醒…"));
  } else {
    input_box_->setPlaceholderText(QStringLiteral("输入消息…"));
  }
  // 「@ 唤起成员选择」提示（原型 .input-toolbar .hint）：群会话专用
  if (at_hint_) at_hint_->setVisible(current_kind_ == QStringLiteral("group"));
  // 输入区固定说明条（原型 .input-note：跨态/直连＝警示底「不可关闭」，
  // 协作＝素底归档说明）；头部徽标（.noarch）只在未归档会话挂「未归档」
  if (input_note_) {
    const QString warn_note =
        QStringLiteral("background:%1; color:%2; font-size:11px; "
                       "padding:6px 18px;")
            .arg(t.warning_wash.name(), t.warning.name());
    const QString plain_note =
        QStringLiteral("background:transparent; color:%1; font-size:11px; "
                       "padding:6px 18px;")
            .arg(t.text_muted.name());
    if (collab_session) {
      input_note_->setStyleSheet(plain_note);
      input_note_->setText(
          current_kind_ == QStringLiteral("group")
              ? QStringLiteral("群聊 · 消息经服务端按成员扇出并全量归档")
              : QStringLiteral("协作态 · 本会话消息将进入服务端归档"));
    } else if (collab_degraded_) {
      input_note_->setStyleSheet(warn_note);
      input_note_->setText(QStringLiteral(
          "已降级直连：服务端不可达，消息暂不归档；恢复后协作消息自动补传归档。"
          "此说明固定显示，不可关闭。"));
    } else if (current_kind_ == QStringLiteral("direct") &&
               is_cross_state(current_peer_)) {
      input_note_->setStyleSheet(warn_note);
      input_note_->setText(QStringLiteral(
          "本会话当前为跨态直达：你发出的消息不进入归档。"
          "此说明固定显示，不可关闭。"));
    } else if (!current_peer_.isEmpty()) {
      input_note_->setStyleSheet(warn_note);
      input_note_->setText(QStringLiteral(
          "直连态：消息点对点传输、不经服务器、不进入归档（仅保存在双方本机）。"));
    } else {
      input_note_->setStyleSheet(plain_note);
      input_note_->setText(QString());
    }
  }
  if (local_badge_) {
    if (collab_session) {
      local_badge_->hide(); // 归档会话无「未归档」徽标（原型仅未归档会话挂）
    } else {
      local_badge_->setText(QStringLiteral("未归档"));
      local_badge_->setStyleSheet(
          QStringLiteral("background:%1; color:%2; border:1px dashed %3; "
                         "border-radius:999px; padding:1px 8px; font-size:11px; "
                         "font-weight:700;")
              .arg(t.warning_wash.name(), t.warning.name(), t.warning.name()));
      local_badge_->show();
    }
  }
  // 输入区未归档胶囊（原型 .input-foot .pill-outline）：文案随形态，
  // 归档会话不挂
  if (noarch_pill_) {
    if (collab_session &&
        current_kind_ == QStringLiteral("group")) {
      // 群会话归档口径胶囊（原型 desktop-group.html .input-foot
      // .pill-outline「群消息全量归档」）：描边素胶囊
      noarch_pill_->setText(QStringLiteral("群消息全量归档"));
      noarch_pill_->setStyleSheet(
          QStringLiteral("color:%1; border:1px solid %1; border-radius:999px; "
                         "padding:2px 8px; font-size:11px;")
              .arg(t.text_muted.name()));
    } else if (collab_session || current_peer_.isEmpty()) {
      noarch_pill_->clear();
      noarch_pill_->setStyleSheet(QString());
    } else {
      noarch_pill_->setText(
          collab_degraded_
              ? QStringLiteral("降级 · 未归档")
              : (current_kind_ == QStringLiteral("direct") &&
                         is_cross_state(current_peer_)
                     ? QStringLiteral("跨态 · 未归档")
                     : QStringLiteral("直连 · 无归档")));
      noarch_pill_->setStyleSheet(
          QStringLiteral("color:%1; border:1px solid %1; border-radius:999px; "
                         "padding:2px 8px; font-size:11px;")
              .arg(t.text_muted.name()));
    }
  }
}

// 登录后把本地库里的历史协作会话补进列表（换机／重启后会话入口不丢）。
// 群会话（"group:N"）不入单聊列表——群列表由 GROUP_DATA 单独维护（T4.1）。
void MainWindow::seed_collab_peers() {
  if (LocalStore* store = direct_engine_.store()) {
    const QStringList hist = store->peers(QStringLiteral("collab"));
    for (const QString& p : hist) {
      if (!p.startsWith(QStringLiteral("group:"))) collab_peers_.insert(p);
    }
  }
}

} // namespace memex::client
