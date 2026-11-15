// settings_window.cpp 拆分模块（LAY-016）：消息页（通知方式/免打扰/消息与记录）＋通知读取 10 法；工厂与公共面见 common.hpp，方法声明在 settings_window.hpp。
#include "common.hpp"

namespace memex::client {

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

} // namespace memex::client
