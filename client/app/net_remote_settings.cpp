#include <app/net_remote_settings.hpp>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <app/main_window.hpp>
#include <app/net_guard.hpp>
#include <app/theme.hpp>

namespace memex::client {

NetRemoteSettingsDialog::NetRemoteSettingsDialog(MainWindow* win,
                                                 QWidget* parent)
    : QDialog(parent), win_(win) {
  setWindowTitle(QStringLiteral("网络与远程"));
  setModal(false);

  auto* layout = new QVBoxLayout(this);

  // —— 网段黑名单：段+掩码命中＝直连发现不响应＋入连接当场拒 ——
  auto* lbl_net = new QLabel(QStringLiteral("网段黑名单"), this);
  lbl_net->setStyleSheet(QStringLiteral("font-weight:600;"));
  layout->addWidget(lbl_net);

  chk_blacklist_ = new QCheckBox(QStringLiteral("启用网段黑名单"), this);
  chk_blacklist_->setObjectName(QStringLiteral("chk_blacklist"));
  chk_blacklist_->setChecked(net_blacklist::enabled());
  layout->addWidget(chk_blacklist_);

  auto* row = new QHBoxLayout;
  edit_cidr_ = new QLineEdit(this);
  edit_cidr_->setPlaceholderText(QStringLiteral("段 a.b.c.d/掩码位（如 192.168.10.0/24）"));
  row->addWidget(edit_cidr_, 1);
  auto* btn_add = new QPushButton(QStringLiteral("添加"), this);
  row->addWidget(btn_add);
  auto* btn_del = new QPushButton(QStringLiteral("删除选中"), this);
  row->addWidget(btn_del);
  layout->addLayout(row);

  list_entries_ = new QListWidget(this);
  list_entries_->setObjectName(QStringLiteral("list_entries"));
  for (const QString& e : net_blacklist::entries()) list_entries_->addItem(e);
  list_entries_->setMaximumHeight(110);
  layout->addWidget(list_entries_);
  connect(btn_add, &QPushButton::clicked, this,
          [this] { add_entry(edit_cidr_->text()); });
  connect(btn_del, &QPushButton::clicked, this, [this] { del_entry(); });

  // —— 远程控制：默认关；开启后受控方批准前须输对配对密码 ——
  auto* lbl_remote = new QLabel(QStringLiteral("远程控制"), this);
  lbl_remote->setStyleSheet(QStringLiteral("font-weight:600;"));
  layout->addWidget(lbl_remote);

  chk_remote_ = new QCheckBox(QStringLiteral("开启远程控制（默认关闭）"), this);
  chk_remote_->setObjectName(QStringLiteral("chk_remote"));
  chk_remote_->setChecked(remote_control::enabled());
  layout->addWidget(chk_remote_);

  layout->addWidget(
      new QLabel(QStringLiteral("配对密码（已设则留空＝不改；只存盐＋哈希）"), this));
  edit_pwd_ = new QLineEdit(this);
  edit_pwd_->setObjectName(QStringLiteral("edit_pwd"));
  edit_pwd_->setEchoMode(QLineEdit::Password);
  edit_pwd_->setPlaceholderText(QStringLiteral("新密码"));
  layout->addWidget(edit_pwd_);
  edit_confirm_ = new QLineEdit(this);
  edit_confirm_->setObjectName(QStringLiteral("edit_confirm"));
  edit_confirm_->setEchoMode(QLineEdit::Password);
  edit_confirm_->setPlaceholderText(QStringLiteral("确认新密码"));
  layout->addWidget(edit_confirm_);

  auto* note = new QLabel(
      QStringLiteral("黑名单关＝全放行；远程控制默认关闭、开启须显式操作——"
                     "开启后受控方批准远程协助前须输入配对密码。"), this);
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

  connect(btn_save, &QPushButton::clicked, this,
          [this] { const QString e = save_settings();
                   if (e.isEmpty()) { accept(); return; }
                   status_->setStyleSheet(QStringLiteral("color:%1;").arg(
                       ThemeManager::instance().tokens().danger.name()));
                   status_->setText(e); });
}

QString NetRemoteSettingsDialog::add_entry(const QString& cidr) {
  const QString err = net_guard::validate_cidr(cidr);
  if (!err.isEmpty()) return err; // 坏段红字不入表（调用方决定怎么显示）
  const QString trimmed = cidr.trimmed();
  for (int i = 0; i < list_entries_->count(); ++i) {
    if (list_entries_->item(i)->text() == trimmed)
      return QStringLiteral("该段已在表中"); // 重复不双录
  }
  list_entries_->addItem(trimmed);
  edit_cidr_->clear();
  return QString();
}

void NetRemoteSettingsDialog::del_entry() {
  const auto items = list_entries_->selectedItems();
  for (auto* item : items) delete list_entries_->takeItem(list_entries_->row(item));
}

QString NetRemoteSettingsDialog::save_settings() {
  const QString pwd = edit_pwd_->text();
  const QString confirm = edit_confirm_->text();
  if (!pwd.isEmpty() && pwd != confirm)
    return QStringLiteral("两次输入的密码不一致");
  const bool remote_on = chk_remote_->isChecked();
  if (remote_on && !remote_control::has_password() && pwd.isEmpty())
    return QStringLiteral("先设置配对密码，才能开启远程控制");
  if (!pwd.isEmpty() && !remote_control::set_password(pwd))
    return QStringLiteral("配对密码不能为空");

  QStringList cidrs;
  for (int i = 0; i < list_entries_->count(); ++i)
    cidrs << list_entries_->item(i)->text();
  net_blacklist::set_entries(cidrs);
  net_blacklist::set_enabled(chk_blacklist_->isChecked());
  remote_control::set_enabled(remote_on);
  if (win_) win_->apply_net_settings(); // 开关/段表即时生效（运行中改即按新表判）
  return QString();
}

QString NetRemoteSettingsDialog::status_text() const { return status_->text(); }

} // namespace memex::client
