#include <app/away_settings.hpp>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include <app/lock_screen.hpp>
#include <app/main_window.hpp>
#include <app/theme.hpp>

namespace memex::client {

AwayLockSettingsDialog::AwayLockSettingsDialog(MainWindow* win,
                                               QWidget* parent)
    : QDialog(parent), win_(win) {
  setWindowTitle(QStringLiteral("离开与锁屏"));
  setModal(false);

  auto* layout = new QVBoxLayout(this);

  auto* lbl_lock = new QLabel(QStringLiteral("离开锁屏"), this);
  lbl_lock->setStyleSheet(QStringLiteral("font-weight:600;"));
  layout->addWidget(lbl_lock);

  chk_enabled_ = new QCheckBox(QStringLiteral("无操作自动锁屏"), this);
  chk_enabled_->setChecked(away_lock::enabled() && away_lock::has_password());
  layout->addWidget(chk_enabled_);

  auto* form = new QHBoxLayout;
  form->addWidget(new QLabel(QStringLiteral("无操作超时（分钟）"), this));
  spin_timeout_ = new QSpinBox(this);
  spin_timeout_->setRange(1, 120);
  spin_timeout_->setValue(qMax(1, away_lock::timeout_seconds() / 60));
  form->addWidget(spin_timeout_);
  form->addStretch();
  layout->addLayout(form);

  layout->addWidget(
      new QLabel(QStringLiteral("离开密码（已设则留空＝不改）"), this));
  edit_pwd_ = new QLineEdit(this);
  edit_pwd_->setEchoMode(QLineEdit::Password);
  edit_pwd_->setPlaceholderText(QStringLiteral("新密码"));
  layout->addWidget(edit_pwd_);

  edit_confirm_ = new QLineEdit(this);
  edit_confirm_->setEchoMode(QLineEdit::Password);
  edit_confirm_->setPlaceholderText(QStringLiteral("确认新密码"));
  layout->addWidget(edit_confirm_);

  auto* note = new QLabel(
      QStringLiteral("锁屏面只显示未读消息数量，不显示内容；密码只在本机"
                     "存盐＋哈希。presence 在线表无「离开」状态字段，对端"
                     "看不到离开标记（协议扩列另批）。"), this);
  note->setWordWrap(true);
  layout->addWidget(note);

  auto* btn_row = new QHBoxLayout;
  auto* btn_save = new QPushButton(QStringLiteral("保存"), this);
  btn_row->addStretch();
  btn_row->addWidget(btn_save);
  layout->addLayout(btn_row);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  layout->addWidget(status_);

  connect(btn_save, &QPushButton::clicked, this, [this] {
    const QString err = save_settings(chk_enabled_->isChecked(),
                                      edit_pwd_->text(),
                                      edit_confirm_->text(),
                                      spin_timeout_->value());
    if (err.isEmpty()) {
      accept();
      return;
    }
    status_->setStyleSheet(
        QStringLiteral("color:%1;")
            .arg(ThemeManager::instance().tokens().danger.name()));
    status_->setText(err);
  });
}

QString AwayLockSettingsDialog::save_settings(bool enable,
                                              const QString& pwd,
                                              const QString& pwd_confirm,
                                              int timeout_min) {
  if (timeout_min < 1) return QStringLiteral("超时至少 1 分钟");
  const bool has_pwd = away_lock::has_password();
  const bool want_pwd = !pwd.isEmpty();
  if (want_pwd && pwd != pwd_confirm)
    return QStringLiteral("两次输入的密码不一致");
  if (enable && !has_pwd && !want_pwd)
    return QStringLiteral("先设置离开密码，才能启用自动锁屏");
  if (want_pwd && !away_lock::set_password(pwd))
    return QStringLiteral("密码不能为空");
  away_lock::set_timeout_seconds(timeout_min * 60);
  away_lock::set_enabled(enable);
  if (win_) win_->apply_away_lock_settings(); // 超时/开关即时生效
  return QString();
}

QString AwayLockSettingsDialog::status_text() const { return status_->text(); }

} // namespace memex::client
