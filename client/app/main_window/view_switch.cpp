// 整屏视图编排（LAY-002 拆分）：传输/组织架构/邮箱三视图进出与
// apply_view_mode 互斥显隐。

#include "main_window.hpp"
#include "main_window/common.hpp"


namespace memex::client {

// —— 文件传输面（原型 desktop-files.html 整屏）——
// 进入：rail「文件传输」切视图（侧栏换「传输通道」页、主区换传输列表）。
// 列表态窄窗（320）放宽到工作面宽——侧栏 220＋传输列表需要横向余量；
// 会话态宽窗保持原尺寸。
void MainWindow::enter_transfers_view() {
  const bool entering = sidebar_mode_ != QStringLiteral("transfers");
  sidebar_mode_ = QStringLiteral("transfers");
  if (entering) {
    setMaximumWidth(QWIDGETSIZE_MAX); // 释放列表态右缘上限（close_chat 设）
    if (!isMaximized() && width() < 700) resize(960, qMax(height(), 560));
  }
  if (rail_) rail_->set_active(QStringLiteral("files"));
  apply_view_mode();
}

// 离开：有会话＝复展开聊天面板（会话态几何本就在）；无会话＝回列表态
// （窄窗＋右缘上限，与 ✕ 关闭同一几何口径）。
void MainWindow::leave_transfers_view() {
  if (sidebar_mode_ != QStringLiteral("transfers")) return;
  sidebar_mode_ = QStringLiteral("sessions");
  if (rail_) rail_->set_active(QStringLiteral("messages"));
  apply_view_mode();
  if (!current_peer_.isEmpty() && chat_open_) {
    side_->setFixedWidth(260);
    chat_panel_->show();
    update_group_panel();
  } else {
    close_chat();
  }
}

// —— 组织架构面（原型 desktop-org.html 三栏整屏）——
// 进入：登录守卫→置态→数据未到即请求（到达回调里重建）→在屏即重建。
// rebuild 只发生在此处／ORG_DATA 回调／主题切换——apply_view_mode 只管
// 显隐（show_org_profile 尾部回显隐不得再触发 rebuild，成环实录 SEGFAULT）。
void MainWindow::enter_org_view() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("组织架构需登录协作态后查看"));
    return;
  }
  const bool entering = sidebar_mode_ != QStringLiteral("org");
  sidebar_mode_ = QStringLiteral("org");
  if (entering) {
    setMaximumWidth(QWIDGETSIZE_MAX); // 释放列表态右缘上限（同传输面）
    if (!isMaximized() && width() < 700) resize(960, qMax(height(), 560));
    if (last_org_json_.isEmpty()) {
      org_dialog_pending_ = true; // 数据到达回调里 rebuild_org_view
      collab_engine_.query_org();
    }
  }
  if (rail_) rail_->set_active(QStringLiteral("org"));
  if (entering) rebuild_org_view();
  apply_view_mode();
}

bool MainWindow::org_view_active() const {
  return org_side_ && org_page_ && !org_side_->isHidden() &&
         !org_page_->isHidden();
}

// 离开：回会话态（有会话＝复展开聊天面板，无会话＝回列表态）。
void MainWindow::leave_org_view() {
  if (sidebar_mode_ != QStringLiteral("org")) return;
  sidebar_mode_ = QStringLiteral("sessions");
  if (rail_) rail_->set_active(QStringLiteral("messages"));
  apply_view_mode();
  if (!current_peer_.isEmpty() && chat_open_) {
    side_->setFixedWidth(260);
    chat_panel_->show();
    update_group_panel();
  } else {
    close_chat();
  }
}

// —— 邮箱·统一收件箱面（原型 desktop-mailbox.html 整屏）——
// 进入：协作登录守卫→文件面登录（懒建客户端，异步）→切视图＋拉条目。
// 未读徽章三面同源：files 登录成功、邮箱动作回执、NOTICE 推达各拉一次
// 计数（条目本身只拉不推——不新增推送链）。
void MainWindow::enter_mailbox_view() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("邮箱需登录协作态后查看（未读数以服务端为准）"));
    return;
  }
  const bool entering = sidebar_mode_ != QStringLiteral("mailbox");
  sidebar_mode_ = QStringLiteral("mailbox");
  if (entering) {
    setMaximumWidth(QWIDGETSIZE_MAX); // 释放列表态右缘上限（同传输面）
    if (!isMaximized() && width() < 700) resize(980, qMax(height(), 600));
  }
  if (rail_) rail_->set_active(QStringLiteral("mailbox"));
  if (mailbox_page_ && files_ensure_login()) mailbox_page_->refresh();
  apply_view_mode();
}

bool MainWindow::mailbox_view_active() const {
  return mailbox_page_ && !mailbox_page_->isHidden();
}

// 离开：回会话态（有会话＝复展开聊天面板，无会话＝回列表态）。
void MainWindow::leave_mailbox_view() {
  if (sidebar_mode_ != QStringLiteral("mailbox")) return;
  sidebar_mode_ = QStringLiteral("sessions");
  if (rail_) rail_->set_active(QStringLiteral("messages"));
  apply_view_mode();
  if (!current_peer_.isEmpty() && chat_open_) {
    side_->setFixedWidth(260);
    chat_panel_->show();
    update_group_panel();
  } else {
    close_chat();
  }
}

// rail 邮箱徽章轻量刷新：只拉未读计数（不打扰条目表）
void MainWindow::refresh_mailbox_badge() {
  if (mailbox_page_ && files_client_ && files_client_->is_logged_in())
    mailbox_page_->refresh_unread();
}

// 视图编排：传输面 ↔ 会话面互斥显隐。会话侧栏/聊天面板的各 setVisible
// 调用（update_side_head 等）以本函数收口——任何刷新后传输面显隐不被
// 侧栏控件显隐写穿。
void MainWindow::apply_view_mode() {
  const bool tx = sidebar_mode_ == QStringLiteral("transfers");
  const bool org = sidebar_mode_ == QStringLiteral("org");
  const bool mbx = sidebar_mode_ == QStringLiteral("mailbox");
  if (side_) side_->setVisible(!tx && !org && !mbx);
  if (transfer_side_) transfer_side_->setVisible(tx);
  if (transfer_page_) transfer_page_->setVisible(tx);
  if (org_side_) org_side_->setVisible(org);
  if (org_page_) org_page_->setVisible(org);
  if (mailbox_page_) mailbox_page_->setVisible(mbx);
  // 档案栏随选中态（原型 .profile-side：选中成员即在；未选中不占位）
  if (org_profile_)
    org_profile_->setVisible(org && !org_selected_member_.isEmpty());
  if (tx) {
    if (chat_panel_) chat_panel_->hide();
    if (group_panel_) group_panel_->hide();
    rebuild_transfer_side();
    refresh_transfers_view();
  }
  if (org) {
    if (chat_panel_) chat_panel_->hide();
    if (group_panel_) group_panel_->hide();
    // 不在此 rebuild：进图（enter_org_view）／数据（ORG_DATA）／主题
    // （apply_theme_styles）三入口各自触发；此处若重建会与
    // show_org_profile 尾部的显隐回调成环（无限递归实录）
  }
  // 显隐切换后立即重算中栏几何：不靠事件循环消化 LayoutRequest（截图腿
  // 曾抓到未结算的布局——传输侧栏被排到聊天面板旧位之后，窗中留空洞）
  if (QWidget* c = centralWidget()) {
    if (QLayout* l = c->layout()) l->activate();
  }
}

} // namespace memex::client
