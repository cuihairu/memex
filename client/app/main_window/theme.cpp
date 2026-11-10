// 主题（LAY-002 拆分）：apply_theme_styles 控件样式统一出口、主题
// 设置入口、状态行出口。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QDialogButtonBox>
#include <QIcon>
#include <QStatusBar>
#include "theme.hpp"
#include "theme_settings_page.hpp"
namespace memex::client {

// 控件样式统一出口：所有颜色只从令牌取（R19 · T4.9）。主题切换后
// ThemeManager::theme_changed 触发重刷，无需重启界面。
void MainWindow::apply_theme_styles() {
  const ThemeTokens& t = tk();
  if (side_) {
    side_->setStyleSheet(QStringLiteral("background:%1; border-right:1px solid %2;")
                             .arg(t.surface_alt.name(), t.border.name()));
  }
  if (side_title_) {
    side_title_->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:14px; color:%1;")
            .arg(t.text.name()));
  }
  if (device_count_) {
    // 原型 device-count：绿点 + text-3 计数（点走富文本 success 跨度）
    device_count_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(t.text_muted.name()));
  }
  if (disc_hint_) {
    disc_hint_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px;").arg(t.text_muted.name()));
  }
  if (new_chat_btn_) {
    // 原型 .icon-btn.brand（brand-soft 底 brand-deep 字）＋悬停品牌底
    new_chat_btn_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none;"
                       " border-radius:8px; font-weight:700; font-size:15px; }"
                       "QPushButton:hover { background:%3; color:%4; }")
            .arg(t.brand_wash.name(), t.brand_text.name(), t.brand.name(),
                 t.on_brand.name()));
  }
  if (device_list_) {
    device_list_->setStyleSheet(QStringLiteral(
        "QListWidget { background:%1; border:1px solid %2; border-radius:8px; }"
        "QListWidget::item { padding:8px; border-bottom:1px solid %3; }"
        "QListWidget::item:selected { background:%4; color:%5; }")
        .arg(t.surface_alt.name(), t.border.name(), t.divider.name(),
             t.selection.name(), t.text.name()));
  }
  if (head_) {
    // 原型 .ch-actions .icon-btn：30px 圆角 8 透明描边、悬停浮面底
    head_->setStyleSheet(
        QStringLiteral(
            "background:%1;"
            "QPushButton { border:none; border-radius:8px; color:%2; }"
            "QPushButton:hover { background:%3; }")
            .arg(t.surface_raised.name(), t.text_muted.name(),
                 t.surface_alt.name()));
  }
  if (chat_title_) {
    chat_title_->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:15px; color:%1;")
            .arg(t.text.name()));
  }
  if (chat_meta_) {
    chat_meta_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(t.text_muted.name()));
  }
  update_chat_avatar(); // on_brand 随主题，头像字色须重取
  if (local_badge_) {
    local_badge_->setStyleSheet(
        QStringLiteral("background:%1; color:%2; border-radius:8px; "
                       "padding:2px 8px; font-size:12px;")
            .arg(t.brand_tint.name(), t.brand_text.name()));
  }
  if (chat_view_) {
    chat_view_->setStyleSheet(QStringLiteral("QTextBrowser { background:%1; border:none; }")
                                  .arg(t.chat_bg.name()));
  }
  if (input_row_) {
    input_row_->setStyleSheet(
        QStringLiteral("background:%1;").arg(t.surface_raised.name()));
  }
  // 描边按钮（发文件／截图／表情／颜色）同款：品牌色描边＋品牌色文字
  const QString outline = QStringLiteral(
      "QPushButton { background:%1; color:%2; border:1px solid %3; "
      "border-radius:8px; padding:6px 12px; }"
      "QPushButton:hover { background:%4; }")
      .arg(t.surface_raised.name(), t.brand_text.name(), t.brand.name(),
           t.brand_wash.name());
  for (QPushButton* btn :
       {file_btn_, folder_btn_, shot_btn_, emoji_btn_, color_btn_, nudge_btn_}) {
    if (btn) btn->setStyleSheet(outline);
  }
  if (send_btn_) {
    send_btn_->setStyleSheet(QStringLiteral(
        "QPushButton { background:%1; color:%2; border:none; "
        "border-radius:8px; padding:6px 18px; font-weight:600; }"
        "QPushButton:hover { background:%3; }"
        "QPushButton:disabled { background:%4; }")
        .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name(),
             t.disabled_bg.name()));
  }
  // rail 导航栏：令牌即状态（图标色随主题重画）；推广卡同刷
  if (rail_) rail_->apply_tokens(t);
  if (collab_promo_) {
    collab_promo_->setStyleSheet(
        QStringLiteral(
            "QWidget#collab_promo { background:%1; border:1px solid %2; "
            "border-radius:8px; }"
            "QLabel#collab_promo_title { font-weight:600; color:%3; }"
            "QLabel#collab_promo_sub { color:%4; font-size:11px; }"
            "QPushButton#collab_promo_btn { background:%5; color:%6; "
            "border:none; border-radius:6px; padding:3px 12px; "
            "font-weight:600; }"
            "QPushButton#collab_promo_btn:hover { background:%7; }")
            .arg(t.surface_raised.name(), t.border.name(), t.text.name(),
                 t.text_muted.name(), t.brand.name(), t.on_brand.name(),
                 t.brand_hover.name()));
  }
  if (banner_) {
    update_banner(); // 横幅底/文随形态与主题两变
  }
  if (at_hint_) {
    at_hint_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px;")
            .arg(t.text_muted.name()));
  }
  if (head_members_btn_) {
    // 原型 .icon-btn.brand：brand_wash 底＋brand_text 字（仅群会话可见）
    head_members_btn_->setStyleSheet(
        QStringLiteral("background:%1; color:%2; border:none; "
                       "border-radius:8px;")
            .arg(t.brand_wash.name(), t.brand_wash_text.name()));
  }
  if (group_panel_) {
    // 原型 .infopanel：panel 底＋左缘 1px 分隔线；内容（成员/文件/卡）随
    // 主题重建
    group_panel_->setStyleSheet(
        QStringLiteral("background:%1; border-left:1px solid %2;")
            .arg(t.surface.name(), t.surface_alt.name()));
    update_group_panel();
  }
  // —— 文件传输面（原型 desktop-files.html）：骨架样式在此刷；侧栏行与
  //     传输卡内嵌令牌色，随 rebuild_transfer_side/refresh_transfers_view
  //     全量重建同刷 ——
  if (transfer_side_) {
    transfer_side_->setStyleSheet(
        QStringLiteral("background:%1; border-right:1px solid %2;")
            .arg(t.surface_alt.name(), t.border.name()));
    rebuild_transfer_side();
  }
  if (transfer_page_) {
    transfer_page_->setStyleSheet(
        QStringLiteral("background:%1;"
                       "QLabel#tx_title { font-weight:600; font-size:15px; "
                       "color:%2; }"
                       "QLabel#tx_note { background:%3; border:1px solid %4; "
                       "border-radius:10px; padding:10px 14px; "
                       "font-size:11.5px; color:%5; }")
            .arg(t.surface.name(), t.text.name(), t.surface_alt.name(),
                 t.border.name(), t.text_muted.name()));
    refresh_transfers_view();
  }
  if (tx_send_btn_) {
    // 原型 .ch-actions .icon-btn.brand：品牌底＋品牌字
    tx_send_btn_->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                       "border-radius:8px; padding:6px 12px; font-weight:700; "
                       "font-size:11.5px; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name()));
  }
  // —— 组织架构面（原型 desktop-org.html）：骨架样式在此刷；树/网格/档案
  //     内嵌令牌色，随 rebuild_org_view 全量重建同刷 ——
  if (org_side_) {
    org_side_->setStyleSheet(
        QStringLiteral("background:%1; border-right:1px solid %2;")
            .arg(t.surface_alt.name(), t.border.name()));
    if (org_search_) {
      org_search_->setStyleSheet(
          QStringLiteral("QLineEdit { background:%1; border:1px solid %2; "
                         "border-radius:8px; padding:6px 10px; "
                         "font-size:12px; color:%3; }")
              .arg(t.surface_raised.name(), t.border.name(), t.text.name()));
    }
    if (sidebar_mode_ == QStringLiteral("org")) rebuild_org_view();
  }
  if (org_page_) {
    org_page_->setStyleSheet(
        QStringLiteral("background:%1;")
            .arg(t.surface.name()));
  }
  if (org_profile_) {
    org_profile_->setStyleSheet(
        QStringLiteral("background:%1; border-left:1px solid %2;")
            .arg(t.surface_alt.name(), t.border.name()));
  }
  {
    // 发起群聊 brand 钮（findChild 兜底：钮在骨架里建、无成员指针）
    if (auto* b = org_page_
                      ? org_page_->findChild<QPushButton*>(
                            QStringLiteral("org_group_btn"))
                      : nullptr) {
      b->setStyleSheet(
          QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                         "border-radius:8px; padding:7px 18px; "
                         "font-weight:700; font-size:12px; }"
                         "QPushButton:hover { background:%3; }")
              .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name()));
    }
  }
  // 聊天区富文本里的颜色也是内联的（气泡/@高亮/系统行）——按记录重渲一遍
  rerender_chat();
}

void MainWindow::show_theme_settings() {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("主题"));
  auto* layout = new QVBoxLayout(&dlg);
  auto* page = new ThemeSettingsPage(&ThemeManager::instance(), &dlg);
  layout->addWidget(page);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  layout->addWidget(buttons);
  dlg.exec();
}

void MainWindow::show_status(const QString& text) {
  status_hint_ = text;
  statusBar()->showMessage(
      QString::fromStdString(direct_engine_.status_text()) +
          QStringLiteral("　|　") +
          QString::fromStdString(collab_engine_.status_text()) +
          QStringLiteral("　|　") + status_hint_,
      0);
}

} // namespace memex::client
