// settings_window.cpp 拆分模块（LAY-016）：通用页（外观/偏好组）＋主题/下载目录/快捷键/自启动读取；工厂与公共面见 common.hpp，方法声明在 settings_window.hpp。
#include "common.hpp"

namespace memex::client {

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
                     "界面字号与语言暂未提供。"),
      page);
  footnote->setWordWrap(true);
  footnote->setStyleSheet(QStringLiteral("color:#9AA0A6;font-size:11px;"));
  root->addWidget(footnote);

  return page;
}

} // namespace memex::client
