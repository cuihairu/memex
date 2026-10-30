// 二期·设置窗口实现（原型 settings-general.html 统一设置面）。通用页按
// 原型分组（外观：主题/界面字号；偏好：语言/快捷键/登录后自动启动/下载
// 位置）逐行对齐；已实现行（主题/快捷键/开机启动/下载位置只读）真实接线，
// 未实现行（界面字号/语言/更改…）控件置灰＋行内「未提供」注（不造假控件，
// 同原型 footnote 口径）。账号/消息/隐私/高级四页为诚实占位。
#include "settings_window.hpp"

#include "main_window.hpp"
#include "shortcut_settings.hpp"
#include "theme.hpp"

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

  stack_ = new QStackedWidget(this);

  struct Page {
    const char* id;
    const char* title;
    QWidget* widget;
  };
  const Page pages[] = {
      {"general", "通用", build_general_page()},
      {"account", "账号",
       build_stub_page("账号", "管理当前登录身份与本机登录状态。",
                       "本页为占位视图：随账号设置面（原型 settings-account.html）"
                       "接入后填充——个人资料、安全与设备、登录状态。")},
      {"message", "消息",
       build_stub_page("消息", "管理提醒方式、免打扰时段与本机消息数据。",
                       "本页为占位视图：随消息设置面（原型 "
                       "settings-message.html）接入后填充——通知方式、免打扰、"
                       "消息与记录。")},
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
      return true;
    }
  }
  return false;
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
                     "其余页（账号 / 消息 / 隐私 / 高级）为占位视图，随对应"
                     "设置面接入后填充。"),
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
