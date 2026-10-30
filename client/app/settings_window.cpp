// 二期·设置窗口实现（原型 settings-general.html 统一设置面）。通用页按
// 原型分组（外观：主题/界面字号；偏好：语言/快捷键/登录后自动启动/下载
// 位置）逐行对齐；已实现行（主题/快捷键/开机启动/下载位置只读）真实接线，
// 未实现行（界面字号/语言/更改…）控件置灰＋行内「未提供」注（不造假控件，
// 同原型 footnote 口径）。消息页按 settings-message.html 三组接线
// （NotifyPrefs 直写，load→改→save 保未展示字段；已读回执走主窗全局键）；
// 隐私/高级两页为诚实占位。
#include "settings_window.hpp"

#include "avatar_store.hpp"
#include "main_window.hpp"
#include "notify_prefs.hpp"
#include "shortcut_settings.hpp"
#include "theme.hpp"

#include <QBitmap>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace memex::client {

namespace {

// 页面标题（原型 .page-title：20px 700）
QLabel* page_title_label(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  QFont f = l->font();
  f.setPointSize(15);
  f.setBold(true);
  l->setFont(f);
  return l;
}

// 页面副题（原型 .page-sub：text-2 12px）
QLabel* page_sub_label(const QString& text, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  l->setStyleSheet(QStringLiteral("color:#80868B;font-size:12px;"));
  l->setWordWrap(true);
  return l;
}

// 原型 .group：面板底＋描边＋组头。描边/圆角/标题色走应用主题 QGroupBox
// 规则（%border%/%radius%/%text_muted% 令牌，亮暗自适应，不硬编码浅色）。
QGroupBox* make_group(const QString& title, QWidget* page) {
  return new QGroupBox(title, page);
}

// 原型 .setting-row：标题 b（13px）＋副题 small（11.5px text-2）＋右端控件。
// control 置灰＝未实现（诚实降级）。
void add_setting_row(QVBoxLayout* group, const QString& name,
                     const QString& sub, QWidget* control) {
  auto* row = new QHBoxLayout;
  auto* copy = new QVBoxLayout;
  copy->setSpacing(2);
  auto* b = new QLabel(name, group->parentWidget());
  QFont bf = b->font();
  bf.setPointSize(10);
  bf.setBold(true);
  b->setFont(bf);
  copy->addWidget(b);
  auto* small = new QLabel(sub, group->parentWidget());
  small->setStyleSheet(QStringLiteral("color:#80868B;font-size:11px;"));
  small->setWordWrap(true);
  copy->addWidget(small);
  row->addLayout(copy, 1);
  row->addWidget(control, 0, Qt::AlignVCenter);
  group->addLayout(row);
  group->addSpacing(6);
}

} // namespace

SettingsWindow::SettingsWindow(MainWindow* win, QWidget* parent)
    : QDialog(parent), win_(win) {
  setWindowTitle(QStringLiteral("设置"));
  resize(1120, 720);
  build_ui();
}

void SettingsWindow::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  auto* body = new QHBoxLayout;
  body->setSpacing(0);
  body->setContentsMargins(0, 0, 0, 0);

  // 侧栏：原型 .settings-nav 220px（panel 底＋右分隔线）——「设置」组头＋
  // 五项导航（通用/账号/消息/隐私/高级）
  auto* side = new QWidget(this);
  side->setFixedWidth(220);
  auto* side_lay = new QVBoxLayout(side);
  side_lay->setContentsMargins(0, 12, 0, 0);
  side_lay->setSpacing(0);
  auto* side_head = new QLabel(QStringLiteral("设置"), side);
  side_head->setStyleSheet(
      QStringLiteral("color:#80868B;font-size:12px;padding:4px 18px 10px;"));
  side_lay->addWidget(side_head);

  nav_ = new QListWidget(side);
  nav_->setObjectName(QStringLiteral("settings_nav"));
  nav_->setFrameShape(QFrame::NoFrame);
  // 主题感知：样式随 ThemeManager 令牌重刷（亮暗两套；选中行沿用应用选中
  // 语义——brand_tint 底＋品牌左条，同 device_list/rail 一致口径，不引入
  // 原型外来蓝）
  const auto refresh_nav = [this]() {
    const auto& t = ThemeManager::instance().tokens();
    nav_->setStyleSheet(
        QStringLiteral("QListWidget#settings_nav{background:transparent;"
                       "border:none;font-size:13px;}"
                       "QListWidget#settings_nav::item{height:38px;"
                       "padding-left:18px;color:%1;}"
                       "QListWidget#settings_nav::item:selected{background:%2;"
                       "color:%3;border-left:3px solid %3;}")
            .arg(t.text.name(), t.selection.name(), t.brand.name()));
  };
  refresh_nav();
  connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
          [refresh_nav](const QString&) { refresh_nav(); });
  // 账号页头像空位底色随主题令牌重刷（登录态 pixmap 无需重刷）
  connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
          [this](const QString&) { refresh_account_page(); });

  stack_ = new QStackedWidget(this);

  struct Page {
    const char* id;
    const char* title;
    QWidget* widget;
  };
  const Page pages[] = {
      {"general", "通用", build_general_page()},
      {"account", "账号", build_account_page()},
      {"message", "消息", build_message_page()},
      {"privacy", "隐私",
       build_stub_page("隐私", "管理消息可见性与本机数据保护相关选项。",
                       "本页为占位视图：随隐私设置面（原型 "
                       "settings-privacy.html）接入后填充——黑名单、在线状态、"
                       "本机消息数据与锁屏。")},
      {"advanced", "高级",
       build_stub_page("高级", "连接、安全过滤与诊断信息。",
                       "本页为占位视图：随高级设置面（原型 "
                       "settings-advanced.html）接入后填充——代理、域名解析覆盖、"
                       "开发者选项、版本与关于。")},
  };

  for (const auto& p : pages) {
    auto* item = new QListWidgetItem(QString::fromUtf8(p.title), nav_);
    item->setData(Qt::UserRole, QString::fromUtf8(p.id));
    stack_->addWidget(p.widget);
  }

  connect(nav_, &QListWidget::currentRowChanged, this,
          [this](int row) { stack_->setCurrentIndex(row); });

  side_lay->addWidget(nav_, 1);
  body->addWidget(side);
  body->addWidget(stack_, 1);
  root->addLayout(body);

  nav_->setCurrentRow(0);
}

bool SettingsWindow::show_page(const QString& id) {
  for (int i = 0; i < nav_->count(); ++i) {
    if (nav_->item(i)->data(Qt::UserRole).toString() == id) {
      nav_->setCurrentRow(i);
      if (id == QStringLiteral("account")) refresh_account_page();
      return true;
    }
  }
  return false;
}

void SettingsWindow::showEvent(QShowEvent* e) {
  QDialog::showEvent(e);
  // 每次显示回填登录态面（登出/换号后重开窗口头像昵称不残留旧值）
  if (current_page() == QStringLiteral("account")) refresh_account_page();
}

QString SettingsWindow::current_page() const {
  int row = nav_->currentRow();
  if (row < 0) return {};
  return nav_->item(row)->data(Qt::UserRole).toString();
}

QString SettingsWindow::page_title() const {
  if (!stack_->currentWidget()) return {};
  return stack_->currentWidget()->property("pageTitle").toString();
}

QString SettingsWindow::theme_mode() const {
  return theme_combo_ ? theme_combo_->currentData().toString() : QString();
}

QString SettingsWindow::download_dir_text() const {
  return download_dir_ ? download_dir_->text() : QString();
}

QString SettingsWindow::shortcut_text() const {
  return shortcut_btn_ ? shortcut_btn_->text() : QString();
}

bool SettingsWindow::autostart_checked() const {
  return autostart_check_ && autostart_check_->isChecked();
}

bool SettingsWindow::msg_desktop_notif() const {
  return msg_desktop_notif_ && msg_desktop_notif_->isChecked();
}

bool SettingsWindow::msg_flash() const {
  return msg_flash_ && msg_flash_->isChecked();
}

bool SettingsWindow::msg_peer_online() const {
  return msg_peer_online_ && msg_peer_online_->isChecked();
}

bool SettingsWindow::msg_file_arrive() const {
  return msg_file_arrive_ && msg_file_arrive_->isChecked();
}

int SettingsWindow::msg_sound_mode() const {
  return msg_sound_mode_ ? msg_sound_mode_->currentIndex() : -1;
}

bool SettingsWindow::msg_dnd() const {
  return msg_dnd_ && msg_dnd_->isChecked();
}

QString SettingsWindow::msg_dnd_start() const {
  return msg_dnd_start_ ? msg_dnd_start_->time().toString(
                              QStringLiteral("HH:mm"))
                        : QString();
}

QString SettingsWindow::msg_dnd_end() const {
  return msg_dnd_end_
             ? msg_dnd_end_->time().toString(QStringLiteral("HH:mm"))
             : QString();
}

bool SettingsWindow::msg_read_receipts() const {
  return msg_read_receipt_ && msg_read_receipt_->isChecked();
}

QWidget* SettingsWindow::build_general_page() {
  auto* page = new QWidget(this);
  page->setProperty("pageTitle", QStringLiteral("通用"));
  auto* root = new QVBoxLayout(page);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(10);

  root->addWidget(page_title_label(QStringLiteral("通用"), page));
  root->addWidget(page_sub_label(
      QStringLiteral("调整 Memex 在这台设备上的显示与启动方式。"), page));
  root->addSpacing(6);

  // —— 组：外观（主题／界面字号）——
  auto* appearance = make_group(QStringLiteral("外观"), page);
  auto* app_lay = new QVBoxLayout(appearance);
  app_lay->setContentsMargins(14, 18, 14, 10);

  // 主题（已实现：ThemeManager 即时生效并持久化）
  {
    theme_combo_ = new QComboBox(page);
    theme_combo_->setObjectName(QStringLiteral("settings_theme"));
    theme_combo_->addItem(QStringLiteral("跟随系统"), QStringLiteral("system"));
    theme_combo_->addItem(QStringLiteral("亮色"), QStringLiteral("light"));
    theme_combo_->addItem(QStringLiteral("暗色"), QStringLiteral("dark"));
    const QString cur = ThemeManager::instance().mode();
    int idx = theme_combo_->findData(cur);
    if (idx < 0) idx = 0;
    theme_combo_->setCurrentIndex(idx);
    connect(theme_combo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int i) {
              ThemeManager::instance().set_mode(
                  theme_combo_->itemData(i).toString());
            });
    add_setting_row(app_lay, QStringLiteral("主题"),
                    QStringLiteral("切换亮色、暗色或跟随系统。"), theme_combo_);
  }

  // 界面字号（未提供：置灰＋注）
  {
    auto* sel = new QWidget(page);
    auto* selrow = new QHBoxLayout(sel);
    selrow->setContentsMargins(0, 0, 0, 0);
    selrow->setSpacing(5);
    const char* sizes[] = {"小", "标准", "大"};
    for (int i = 0; i < 3; ++i) {
      auto* b = new QPushButton(QString::fromUtf8(sizes[i]), sel);
      b->setCheckable(true);
      b->setEnabled(false);
      b->setChecked(i == 1);
      selrow->addWidget(b);
    }
    add_setting_row(app_lay, QStringLiteral("界面字号"),
                    QStringLiteral("调整消息与列表文字大小。未提供：本版本未"
                                   "实现全局字号缩放。"),
                    sel);
  }

  root->addWidget(appearance);

  // —— 组：偏好（语言／快捷键／登录后自动启动／下载位置）——
  auto* prefs = make_group(QStringLiteral("偏好"), page);
  auto* pref_lay = new QVBoxLayout(prefs);
  pref_lay->setContentsMargins(14, 18, 14, 10);

  // 语言（未提供）
  {
    auto* combo = new QComboBox(page);
    combo->addItem(QStringLiteral("简体中文"));
    combo->setEnabled(false);
    add_setting_row(pref_lay, QStringLiteral("语言"),
                    QStringLiteral("当前原型使用简体中文。未提供：本版本未接"
                                   "入多语言。"),
                    combo);
  }

  // 快捷键（已实现：主窗口径判权/冲突检测）
  {
    shortcut_btn_ = new QPushButton(MainWindow::screenshot_shortcut(), page);
    shortcut_btn_->setObjectName(QStringLiteral("settings_shortcut"));
    if (win_) {
      connect(shortcut_btn_, &QPushButton::clicked, this, [this]() {
        ShortcutSettingsDialog dlg(win_, this);
        dlg.exec();
        shortcut_btn_->setText(MainWindow::screenshot_shortcut());
      });
    } else {
      shortcut_btn_->setEnabled(false);
    }
    add_setting_row(pref_lay, QStringLiteral("快捷键"),
                    QStringLiteral("截图快捷键；当前支持窗口内快捷键。"),
                    shortcut_btn_);
  }

  // 登录后自动启动（已实现：freedesktop 自启动项）
  {
    autostart_check_ = new QCheckBox(QStringLiteral("自动启动"), page);
    autostart_check_->setObjectName(QStringLiteral("settings_autostart"));
    if (win_) {
      autostart_check_->setChecked(win_->autostart_enabled());
      connect(autostart_check_, &QCheckBox::toggled, this, [this](bool on) {
        win_->set_autostart(on);
        autostart_check_->setChecked(win_->autostart_enabled());
      });
    } else {
      autostart_check_->setEnabled(false);
    }
    add_setting_row(pref_lay, QStringLiteral("登录后自动启动"),
                    QStringLiteral("登录系统后启动 Memex。"), autostart_check_);
  }

  // 下载位置（只读真实目录；更改未提供）
  {
    auto* field = new QWidget(page);
    auto* frow = new QHBoxLayout(field);
    frow->setContentsMargins(0, 0, 0, 0);
    frow->setSpacing(6);
    download_dir_ = new QLineEdit(
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), field);
    download_dir_->setObjectName(QStringLiteral("settings_download_dir"));
    download_dir_->setReadOnly(true);
    frow->addWidget(download_dir_, 1);
    auto* change = new QPushButton(QStringLiteral("更改…"), field);
    change->setEnabled(false);
    frow->addWidget(change);
    add_setting_row(pref_lay, QStringLiteral("下载位置"),
                    QStringLiteral("接收的文件默认保存目录。未提供：固定为系"
                                   "统下载目录，更改不可用。"),
                    field);
  }

  root->addWidget(prefs);
  root->addStretch();

  // footnote（原型 .footnote 11px text-3 底部说明行）
  auto* footnote = new QLabel(
      QStringLiteral("原型说明：控件仅展示布局，不代表尚未实现的功能已可操作。"
                     "其余页（隐私 / 高级）为占位视图，随对应设置面接入后填充。"),
      page);
  footnote->setWordWrap(true);
  footnote->setStyleSheet(QStringLiteral("color:#9AA0A6;font-size:11px;"));
  root->addWidget(footnote);

  return page;
}

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

QWidget* SettingsWindow::build_message_page() {
  auto* page = new QWidget(this);
  page->setProperty("pageTitle", QStringLiteral("消息"));
  auto* root = new QVBoxLayout(page);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(10);

  root->addWidget(page_title_label(QStringLiteral("消息"), page));
  root->addWidget(page_sub_label(
      QStringLiteral("管理提醒方式、免打扰时段与本机消息数据。"), page));
  root->addSpacing(6);

  const NotifyPrefs prefs = NotifyPrefs::load();
  // 落盘口径：load→只改本页字段→save。分级弹窗/全屏策略/传输完成/声音
  // 子位不在本页展示，逐项直写整存会把它们的显式设置清回默认值。
  const auto save_prefs = [](auto&& patch) {
    NotifyPrefs p = NotifyPrefs::load();
    patch(p);
    p.save();
  };

  // —— 组：通知方式（原型五行：桌面通知/窗口闪烁/上线通知/文件通知/提示音）——
  auto* notify = make_group(QStringLiteral("通知方式"), page);
  auto* n_lay = new QVBoxLayout(notify);
  n_lay->setContentsMargins(14, 18, 14, 10);

  {
    msg_desktop_notif_ = new QCheckBox(page);
    msg_desktop_notif_->setObjectName(QStringLiteral("settings_msg_desktop"));
    msg_desktop_notif_->setChecked(prefs.popup_message);
    connect(msg_desktop_notif_, &QCheckBox::toggled, this, [this](bool on) {
      NotifyPrefs p = NotifyPrefs::load();
      p.popup_message = on;
      p.save();
    });
    add_setting_row(n_lay, QStringLiteral("桌面通知"),
                    QStringLiteral("按消息级别控制弹窗提醒。"),
                    msg_desktop_notif_);
  }

  {
    msg_flash_ = new QCheckBox(page);
    msg_flash_->setObjectName(QStringLiteral("settings_msg_flash"));
    msg_flash_->setChecked(prefs.flash_alert);
    connect(msg_flash_, &QCheckBox::toggled, this, [this](bool on) {
      NotifyPrefs p = NotifyPrefs::load();
      p.flash_alert = on;
      p.save();
    });
    add_setting_row(n_lay, QStringLiteral("窗口闪烁"),
                    QStringLiteral("窗口未激活时收到新消息提示。"), msg_flash_);
  }

  {
    msg_peer_online_ = new QCheckBox(page);
    msg_peer_online_->setObjectName(QStringLiteral("settings_msg_peer_online"));
    msg_peer_online_->setChecked(prefs.popup_peer_online);
    connect(msg_peer_online_, &QCheckBox::toggled, this, [this](bool on) {
      NotifyPrefs p = NotifyPrefs::load();
      p.popup_peer_online = on;
      p.save();
    });
    add_setting_row(n_lay, QStringLiteral("联系人上线通知"),
                    QStringLiteral("联系人上线时显示桌面通知。"),
                    msg_peer_online_);
  }

  {
    msg_file_arrive_ = new QCheckBox(page);
    msg_file_arrive_->setObjectName(QStringLiteral("settings_msg_file_arrive"));
    msg_file_arrive_->setChecked(prefs.popup_file_arrive);
    connect(msg_file_arrive_, &QCheckBox::toggled, this, [this](bool on) {
      NotifyPrefs p = NotifyPrefs::load();
      p.popup_file_arrive = on;
      p.save();
    });
    add_setting_row(n_lay, QStringLiteral("文件到达通知"),
                    QStringLiteral("收到文件时显示桌面通知。"),
                    msg_file_arrive_);
  }

  {
    msg_sound_mode_ = new QComboBox(page);
    msg_sound_mode_->setObjectName(QStringLiteral("settings_msg_sound"));
    msg_sound_mode_->addItem(QStringLiteral("关闭"));
    msg_sound_mode_->addItem(QStringLiteral("每条播放"));
    msg_sound_mode_->addItem(QStringLiteral("按事件类型"));
    msg_sound_mode_->setCurrentIndex(prefs.sound_mode);
    connect(msg_sound_mode_,
            qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
              NotifyPrefs p = NotifyPrefs::load();
              p.sound_mode = idx;
              p.save();
            });
    add_setting_row(n_lay, QStringLiteral("提示音"),
                    QStringLiteral("选择关闭、每条播放或按事件类型播放。"),
                    msg_sound_mode_);
  }

  root->addWidget(notify);

  // —— 组：免打扰（启用开关＋起止时段，跨零点区间由 dnd_active 裁决）——
  auto* dnd = make_group(QStringLiteral("免打扰"), page);
  auto* d_lay = new QVBoxLayout(dnd);
  d_lay->setContentsMargins(14, 18, 14, 10);

  {
    msg_dnd_ = new QCheckBox(page);
    msg_dnd_->setObjectName(QStringLiteral("settings_msg_dnd"));
    msg_dnd_->setChecked(prefs.dnd);
    connect(msg_dnd_, &QCheckBox::toggled, this, [this](bool on) {
      NotifyPrefs p = NotifyPrefs::load();
      p.dnd = on;
      p.save();
    });
    add_setting_row(d_lay, QStringLiteral("启用免打扰"),
                    QStringLiteral("普通与重要消息在设定时段静默。"), msg_dnd_);
  }

  {
    auto* span = new QWidget(page);
    auto* srow = new QHBoxLayout(span);
    srow->setContentsMargins(0, 0, 0, 0);
    srow->setSpacing(6);
    const auto mk_time = [&span](const QString& name, const QString& t) {
      auto* edit = new QTimeEdit(QTime::fromString(t, QStringLiteral("HH:mm")),
                                 span);
      edit->setObjectName(name);
      edit->setDisplayFormat(QStringLiteral("HH:mm"));
      return edit;
    };
    msg_dnd_start_ = mk_time(QStringLiteral("settings_msg_dnd_start"),
                             prefs.dnd_start);
    msg_dnd_end_ =
        mk_time(QStringLiteral("settings_msg_dnd_end"), prefs.dnd_end);
    srow->addWidget(msg_dnd_start_);
    srow->addWidget(new QLabel(QStringLiteral("至"), span));
    srow->addWidget(msg_dnd_end_);
    connect(msg_dnd_start_, &QTimeEdit::timeChanged, this,
            [this](const QTime& t) {
              NotifyPrefs p = NotifyPrefs::load();
              p.dnd_start = t.toString(QStringLiteral("HH:mm"));
              p.save();
            });
    connect(msg_dnd_end_, &QTimeEdit::timeChanged, this,
            [this](const QTime& t) {
              NotifyPrefs p = NotifyPrefs::load();
              p.dnd_end = t.toString(QStringLiteral("HH:mm"));
              p.save();
            });
    add_setting_row(d_lay, QStringLiteral("免打扰时段"),
                    QStringLiteral("跨午夜时段同样适用。"), span);
  }

  root->addWidget(dnd);

  // —— 组：消息与记录（已读回执全局键；迁移备份原型自标「暂未提供」）——
  auto* records = make_group(QStringLiteral("消息与记录"), page);
  auto* r_lay = new QVBoxLayout(records);
  r_lay->setContentsMargins(14, 18, 14, 10);

  {
    msg_read_receipt_ = new QCheckBox(page);
    msg_read_receipt_->setObjectName(QStringLiteral("settings_msg_receipts"));
    if (win_) {
      msg_read_receipt_->setChecked(win_->read_receipts_enabled(QString()));
      connect(msg_read_receipt_, &QCheckBox::toggled, this, [this](bool on) {
        win_->set_read_receipts_enabled(QString(), on);
      });
    } else {
      msg_read_receipt_->setEnabled(false);
    }
    add_setting_row(r_lay, QStringLiteral("发送已读回执"),
                    QStringLiteral("允许对方看到消息已读状态。"),
                    msg_read_receipt_);
  }

  {
    auto* btn = new QPushButton(QStringLiteral("暂未提供"), page);
    btn->setObjectName(QStringLiteral("settings_msg_backup"));
    btn->setEnabled(false);
    btn->setToolTip(QStringLiteral("未提供：本客户端无内置迁移或备份入口。"));
    add_setting_row(r_lay, QStringLiteral("聊天记录迁移与备份"),
                    QStringLiteral("当前客户端没有内置迁移或备份入口。"), btn);
  }

  root->addWidget(records);
  root->addStretch();

  // footnote（原型 .footnote 通用口径；唯一未实现行按钮自标「暂未提供」）
  auto* footnote = new QLabel(
      QStringLiteral("原型说明：控件仅展示布局，不代表尚未实现的功能已可操作。"
                     "分级弹窗与全屏策略另有通知偏好设置（托盘菜单）。"),
      page);
  footnote->setWordWrap(true);
  footnote->setStyleSheet(QStringLiteral("color:#9AA0A6;font-size:11px;"));
  root->addWidget(footnote);

  return page;
}

QWidget* SettingsWindow::build_stub_page(const QString& title,
                                         const QString& sub,
                                         const QString& note) {
  auto* page = new QWidget(this);
  page->setProperty("pageTitle", title);
  auto* root = new QVBoxLayout(page);
  root->setContentsMargins(28, 24, 28, 24);
  root->setSpacing(4);

  root->addWidget(page_title_label(title, page));
  root->addWidget(page_sub_label(sub, page));
  root->addSpacing(16);

  auto* box = new QGroupBox(page);
  auto* bl = new QVBoxLayout(box);
  auto* note_label = new QLabel(note, page);
  note_label->setWordWrap(true);
  note_label->setStyleSheet(QStringLiteral("color:#9AA0A6;"));
  bl->addWidget(note_label);
  root->addWidget(box);
  root->addStretch();
  return page;
}

} // namespace memex::client
