// settings_window.cpp 拆分模块（LAY-016）：账号页＋头像/昵称/登录态回填；工厂与公共面见 common.hpp，方法声明在 settings_window.hpp。
#include "common.hpp"

namespace memex::client {

QWidget* SettingsWindow::build_account_page() {
  auto* page = new QWidget(this);
  page->setProperty("pageTitle", QStringLiteral("账号"));
  auto* root = new QVBoxLayout(page);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(10);

  root->addWidget(page_title_label(QStringLiteral("账号"), page));
  root->addWidget(page_sub_label(
      QStringLiteral("管理当前登录身份与本机登录状态。"), page));
  root->addSpacing(6);

  // —— 组：个人资料（头像／昵称；真实接线，登录门在主窗）——
  auto* profile = make_group(QStringLiteral("个人资料"), page);
  auto* prof_lay = new QVBoxLayout(profile);
  prof_lay->setContentsMargins(14, 18, 14, 10);

  {
    auto* ctrl = new QWidget(page);
    auto* c = new QHBoxLayout(ctrl);
    c->setContentsMargins(0, 0, 0, 0);
    c->setSpacing(8);
    acct_avatar_ = new QLabel(ctrl);
    acct_avatar_->setFixedSize(42, 42);
    acct_avatar_->setAlignment(Qt::AlignCenter);
    c->addWidget(acct_avatar_);
    acct_avatar_btn_ = new QPushButton(QStringLiteral("更换头像"), ctrl);
    acct_avatar_btn_->setObjectName(QStringLiteral("settings_acct_avatar_btn"));
    if (win_) {
      connect(acct_avatar_btn_, &QPushButton::clicked, this,
              [this] { win_->change_avatar(); });
    }
    c->addWidget(acct_avatar_btn_);
    c->addStretch();
    add_setting_row(prof_lay, QStringLiteral("头像"),
                    QStringLiteral("当前账号头像。"), ctrl);
  }

  {
    auto* ctrl = new QWidget(page);
    auto* c = new QHBoxLayout(ctrl);
    c->setContentsMargins(0, 0, 0, 0);
    c->setSpacing(8);
    acct_name_ = new QLineEdit(ctrl);
    acct_name_->setObjectName(QStringLiteral("settings_acct_name"));
    acct_name_->setReadOnly(true);
    c->addWidget(acct_name_, 1);
    acct_profile_btn_ = new QPushButton(QStringLiteral("编辑资料"), ctrl);
    acct_profile_btn_->setObjectName(QStringLiteral("settings_acct_profile_btn"));
    if (win_) {
      connect(acct_profile_btn_, &QPushButton::clicked, this,
              [this] { win_->show_profile_dialog(); });
    }
    c->addWidget(acct_profile_btn_);
    add_setting_row(prof_lay, QStringLiteral("昵称"),
                    QStringLiteral("显示给联系人和群成员的名称。"), ctrl);
  }

  root->addWidget(profile);

  // —— 组：安全与设备（账号安全／设备管理——无对应实现，置灰＋未提供注）——
  auto* sec = make_group(QStringLiteral("安全与设备"), page);
  auto* sec_lay = new QVBoxLayout(sec);
  sec_lay->setContentsMargins(14, 18, 14, 10);
  {
    auto* btn = new QPushButton(QStringLiteral("账号安全"), page);
    btn->setEnabled(false);
    btn->setToolTip(QStringLiteral("未提供：本版本无修改密码界面。"));
    add_setting_row(sec_lay, QStringLiteral("账号安全"),
                    QStringLiteral("账号密码由服务器账号体系管理。未提供：本"
                                   "版本无改密界面。"),
                    btn);
  }
  {
    auto* btn = new QPushButton(QStringLiteral("管理设备"), page);
    btn->setEnabled(false);
    btn->setToolTip(QStringLiteral("未提供：本版本无设备管理界面。"));
    add_setting_row(sec_lay, QStringLiteral("设备管理"),
                    QStringLiteral("查看已登录设备与会话。未提供：本版本无设"
                                   "备管理界面。"),
                    btn);
  }
  root->addWidget(sec);

  // —— 组：登录状态（退出登录——真实登出，回直连态）——
  auto* login = make_group(QStringLiteral("登录状态"), page);
  auto* login_lay = new QVBoxLayout(login);
  login_lay->setContentsMargins(14, 18, 14, 10);
  {
    acct_logout_btn_ = new QPushButton(QStringLiteral("退出登录"), page);
    acct_logout_btn_->setObjectName(QStringLiteral("settings_acct_logout_btn"));
    if (win_) {
      connect(acct_logout_btn_, &QPushButton::clicked, this, [this] {
        win_->logout_collab();
        refresh_account_page();
      });
    }
    add_setting_row(login_lay, QStringLiteral("退出登录"),
                    QStringLiteral("退出当前协作账号；本机直连功能仍可使用。"),
                    acct_logout_btn_);
  }
  root->addWidget(login);
  root->addStretch();

  auto* footnote = new QLabel(
      QStringLiteral("原型说明：控件仅展示布局，不代表尚未实现的功能已可操作。"
                     "账号安全与设备管理暂未提供界面。"),
      page);
  footnote->setWordWrap(true);
  footnote->setStyleSheet(QStringLiteral("color:#9AA0A6;font-size:11px;"));
  root->addWidget(footnote);

  refresh_account_page();
  return page;
}

void SettingsWindow::refresh_account_page() {
  if (!acct_avatar_) return;
  const bool logged = win_ && win_->collab_logged_in();
  const auto& t = ThemeManager::instance().tokens();
  // 头像：真实头像→默认首字头像→未登录空位（诚实回填，不造假数据）；
  // 空位底色随主题令牌（亮暗自适应，不硬编码浅色）
  QPixmap px = win_ ? win_->self_avatar(42) : QPixmap();
  if (px.isNull() && logged) px = default_avatar(win_->self_account(), 42);
  if (!px.isNull()) {
    QBitmap mask(42, 42);
    mask.fill(Qt::color0);
    QPainter p(&mask);
    p.setBrush(Qt::color1);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, 42, 42, 8, 8);
    p.end();
    px.setMask(mask);
    acct_avatar_->setPixmap(px);
    acct_avatar_->setText(QString());
    acct_avatar_->setStyleSheet(QStringLiteral("border-radius:8px;"));
  } else {
    acct_avatar_->setPixmap(QPixmap());
    acct_avatar_->setText(QStringLiteral("—"));
    acct_avatar_->setStyleSheet(
        QStringLiteral("border-radius:8px;background:%1;color:%2;")
            .arg(t.surface_alt.name(), t.text_muted.name()));
  }
  if (acct_name_) {
    acct_name_->setText(logged ? win_->self_display_name()
                               : QStringLiteral("未登录"));
  }
  if (acct_avatar_btn_) acct_avatar_btn_->setEnabled(logged);
  if (acct_profile_btn_) acct_profile_btn_->setEnabled(logged);
  if (acct_logout_btn_) acct_logout_btn_->setEnabled(logged);
}

} // namespace memex::client
