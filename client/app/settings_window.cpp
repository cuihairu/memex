// 二期·设置窗口实现（原型 settings-general.html 统一设置面）。通用页按
// 原型分组（外观：主题/界面字号；偏好：语言/快捷键/登录后自动启动/下载
// 位置）逐行对齐；已实现行（主题/快捷键/开机启动/下载位置只读）真实接线，
// 未实现行（界面字号/语言/更改…）控件置灰＋行内「未提供」注（不造假控件，
// 同原型 footnote 口径）。消息页按 settings-message.html 三组接线
// （NotifyPrefs 直写，load→改→save 保未展示字段；已读回执走主窗全局键）；
// 隐私页按 settings-privacy.html 三组接线（网段黑名单/离开锁屏走主窗共用
// 入口，存储说明列真实本机数据目录）；高级页按 settings-advanced.html 三组
// 接线（网段黑名单开关走 net_blacklist 真实键，域名解析覆盖/关于走主窗共用
// 入口，版本标签列真实安装版本；代理/开发者选项/运行日志置灰「暂未提供」）。
#include "settings_window/common.hpp"

namespace memex::client {

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
      {"privacy", "隐私", build_privacy_page()},
      {"advanced", "高级", build_advanced_page()},
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
      // 高级页黑名单开关回读真实键（隐私页「管理网络黑名单」对话框可能已
      // 改过；setChecked 同值回写幂等）
      if (id == QStringLiteral("advanced") && adv_blacklist_)
        adv_blacklist_->setChecked(net_blacklist::enabled());
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

} // namespace memex::client
