#include <app/lock_screen.hpp>

#include <QCloseEvent>
#include <QCryptographicHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QPalette>
#include <QSettings>
#include <QVBoxLayout>

#include <app/theme.hpp>

namespace memex::client {

namespace away_lock {

namespace {
QString salt_key() { return QStringLiteral("away/pwd_salt"); }
QString hash_key() { return QStringLiteral("away/pwd_hash"); }

QByteArray hash_pwd(const QString& salt, const QString& pwd) {
  return QCryptographicHash::hash((salt + pwd).toUtf8(),
                                  QCryptographicHash::Sha256);
}
} // namespace

bool enabled() {
  return QSettings().value(QStringLiteral("away/enabled"), false).toBool();
}

void set_enabled(bool on) { QSettings().setValue(QStringLiteral("away/enabled"), on); }

int timeout_seconds() {
  return QSettings().value(QStringLiteral("away/timeout_sec"), 300).toInt();
}

void set_timeout_seconds(int seconds) {
  if (seconds <= 0) return; // 坏值不落盘（保持现值）
  QSettings().setValue(QStringLiteral("away/timeout_sec"), seconds);
}

bool has_password() {
  QSettings s;
  return s.contains(salt_key()) && s.contains(hash_key());
}

bool set_password(const QString& pwd) {
  if (pwd.isEmpty()) return false;
  // 新盐每次设密重生成：同密码两台机器落盘哈希也不同
  QString salt;
  for (int i = 0; i < 4; ++i)
    salt += QStringLiteral("%1").arg(QRandomGenerator::system()->generate(),
                                     8, 16, QLatin1Char('0'));
  QSettings s;
  s.setValue(salt_key(), salt);
  s.setValue(hash_key(), QString::fromLatin1(hash_pwd(salt, pwd).toHex()));
  return true;
}

bool verify_password(const QString& pwd) {
  QSettings s;
  if (!has_password()) return false;
  const QString salt = s.value(salt_key()).toString();
  const QString expect = s.value(hash_key()).toString();
  return QString::fromLatin1(hash_pwd(salt, pwd).toHex()) == expect;
}

} // namespace away_lock

LockScreenDialog::LockScreenDialog(std::function<bool(const QString&)> verifier,
                                   QWidget* parent)
    : QDialog(parent), verifier_(std::move(verifier)) {
  setWindowTitle(QStringLiteral("离开锁定"));
  setWindowFlag(Qt::FramelessWindowHint, true);
  setWindowFlag(Qt::WindowStaysOnTopHint, true);
  setModal(true);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(40, 60, 40, 40);
  layout->setSpacing(18);

  lbl_head_ = new QLabel(QStringLiteral("离开锁定"), this);
  lbl_head_->setAlignment(Qt::AlignCenter);
  QLabel* sub = new QLabel(
      QStringLiteral("锁屏期间收到的消息只显示数量，不显示内容"), this);
  sub->setAlignment(Qt::AlignCenter);

  lbl_unread_ = new QLabel(this);
  lbl_unread_->setAlignment(Qt::AlignCenter);

  edit_pwd_ = new QLineEdit(this);
  edit_pwd_->setEchoMode(QLineEdit::Password);
  edit_pwd_->setPlaceholderText(QStringLiteral("输入密码解锁"));
  edit_pwd_->setFixedWidth(280);
  connect(edit_pwd_, &QLineEdit::returnPressed, this,
          [this] { try_unlock(edit_pwd_->text()); });

  lbl_state_ = new QLabel(this);
  lbl_state_->setAlignment(Qt::AlignCenter);

  btn_unlock_ = new QPushButton(QStringLiteral("解锁"), this);
  btn_unlock_->setFixedWidth(280);
  connect(btn_unlock_, &QPushButton::clicked, this,
          [this] { try_unlock(edit_pwd_->text()); });

  layout->addStretch();
  layout->addWidget(lbl_head_);
  layout->addWidget(sub);
  layout->addWidget(lbl_unread_);
  layout->addSpacing(12);
  layout->addWidget(edit_pwd_, 0, Qt::AlignHCenter);
  layout->addWidget(btn_unlock_, 0, Qt::AlignHCenter);
  layout->addWidget(lbl_state_);
  layout->addStretch();

  // 深色遮罩：盖住底下的会话内容（令文口径＝只显数量不显内容）；
  // 遮罩底固定暗色（锁屏语义＝不看底下，亮主题也用暗面）——QDialog 背景
  // 用 QPalette 填（类选择器 QSS 对顶窗背景不生效），子控件样式仍走 QSS
  QPalette pal = palette();
  pal.setColor(QPalette::Window, QColor(0x1d, 0x1f, 0x24));
  setPalette(pal);
  setAutoFillBackground(true);
  const ThemeTokens tk = ThemeManager::instance().tokens();
  setStyleSheet(QStringLiteral(
      "QLabel{color:#f2f2f2;font-size:15px;}"
      "QLabel#lock_head{font-size:26px;font-weight:600;}"
      "QLabel#lock_unread{font-size:34px;font-weight:600;}"
      "QLineEdit{background:rgba(255,255,255,0.92);color:#222;"
      "border:none;border-radius:4px;padding:8px;}"
      "QPushButton{background:%1;color:#fff;border:none;border-radius:4px;"
      "padding:8px;}"
      "QPushButton:hover{background:%2;}")
                    .arg(tk.brand.name(), tk.brand_hover.name()));
  lbl_head_->setObjectName(QStringLiteral("lock_head"));
  lbl_unread_->setObjectName(QStringLiteral("lock_unread"));
  lbl_unread_->setText(QStringLiteral("暂无新消息")); // 初始渲染（计数从真 0 起）
}

void LockScreenDialog::bump_unread() {
  ++unread_;
  lbl_unread_->setText(unread_ > 0
                           ? QStringLiteral("锁屏期间新消息 %1 条").arg(unread_)
                           : QStringLiteral("暂无新消息"));
}

QString LockScreenDialog::unread_text() const { return lbl_unread_->text(); }

void LockScreenDialog::try_unlock(const QString& pwd) {
  if (verifier_ && verifier_(pwd)) {
    accept();
    return;
  }
  lbl_state_->setStyleSheet(QStringLiteral("color:#ff9a8a;")); // 错密码红字
  lbl_state_->setText(QStringLiteral("密码不对"));
  edit_pwd_->clear();
  edit_pwd_->setFocus();
}

void LockScreenDialog::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) return; // Esc 不关（只认密码）
  QDialog::keyPressEvent(event);
}

void LockScreenDialog::closeEvent(QCloseEvent* event) {
  event->ignore(); // 关闭钮/Alt+F4 不放行（唯一出路＝密码）
}

} // namespace memex::client
