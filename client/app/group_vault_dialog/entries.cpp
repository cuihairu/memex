// group_vault_dialog.cpp 拆分模块（LAY-017）：条目操作（编辑框/提交/编辑/查看/复制/删除）与审计窗；公共面见 common.hpp，方法声明在 group_vault_dialog.hpp。
#include "common.hpp"

namespace memex::client {

// 名称/账号/密码/URL/备注五字段编辑子对话框（手工新建/编辑共用）
bool GroupVaultDialog::entry_dialog(QString* name, QString* account_name,
                                    QString* pass, QString* url, QString* note,
                                    const QString& init_name,
                                    const QString& init_account,
                                    const QString& init_pass,
                                    const QString& init_url,
                                    const QString& init_note) {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("密码箱条目"));
  dlg.resize(460, 380);
  auto* layout = new QVBoxLayout(&dlg);
  auto* name_edit = new QLineEdit(init_name, &dlg);
  name_edit->setPlaceholderText(QStringLiteral("名称（必填，如：生产库 WiFi）"));
  auto* acc_edit = new QLineEdit(init_account, &dlg);
  acc_edit->setPlaceholderText(QStringLiteral("账号（必填）"));
  auto* pass_edit = new QLineEdit(init_pass, &dlg);
  pass_edit->setPlaceholderText(QStringLiteral("密码（必填；只进密码箱密文存储）"));
  auto* url_edit = new QLineEdit(init_url, &dlg);
  url_edit->setPlaceholderText(QStringLiteral("URL（选填）"));
  auto* note_edit = new QPlainTextEdit(init_note, &dlg);
  note_edit->setPlaceholderText(QStringLiteral("备注（选填）"));
  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                           &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                   &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                   &QDialog::reject);
  layout->addWidget(name_edit);
  layout->addWidget(acc_edit);
  layout->addWidget(pass_edit);
  layout->addWidget(url_edit);
  layout->addWidget(note_edit, 1);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return false;
  *name = name_edit->text().trimmed();
  *account_name = acc_edit->text().trimmed();
  *pass = pass_edit->text();
  *url = url_edit->text().trimmed();
  *note = note_edit->toPlainText();
  return !name->isEmpty() && !account_name->isEmpty() && !pass->isEmpty();
}

bool GroupVaultDialog::submit_entry(const QString& name,
                                    const QString& account_name,
                                    const QString& password,
                                    const QString& url, const QString& note) {
  if (state_ != State::kUnlocked) {
    set_status(QStringLiteral("先解锁密码箱"), true);
    return false;
  }
  if (name.trimmed().isEmpty() || account_name.trimmed().isEmpty() ||
      password.isEmpty()) {
    set_status(QStringLiteral("名称/账号/密码为必填"), true);
    return false;
  }
  QString nonce_b64;
  const QString ct =
      vault::encrypt_secret(dek_, pack_secret(password, url, note), &nonce_b64);
  if (ct.isEmpty() || nonce_b64.isEmpty()) {
    set_status(QStringLiteral("条目加密失败"), true);
    return false;
  }
  client_->save_group_vault_entry(gid_, name.trimmed(), account_name.trimmed(),
                                  ct, nonce_b64, editing_id_);
  return true;
}

bool GroupVaultDialog::edit_selected() {
  const auto* it = stream_->currentItem();
  if (!it || it->flags() == Qt::NoItemFlags) return false;
  const qint64 id = it->data(kRoleId).toLongLong();
  if (editing_id_ == id) {
    end_edit(); // 再进一次＝取消编辑
    set_status(QStringLiteral("已取消编辑"), false);
    return true;
  }
  editing_id_ = id;
  // 取回现明文预填（wire 用 reveal——编辑前读即一次查看，照章留痕）
  pending_action_ = QStringLiteral("edit");
  client_->access_group_vault_entry(gid_, id, QStringLiteral("reveal"));
  return true;
}

bool GroupVaultDialog::reveal_selected() {
  const auto* it = stream_->currentItem();
  if (!it || it->flags() == Qt::NoItemFlags) return false;
  if (state_ != State::kUnlocked) {
    set_status(QStringLiteral("先解锁密码箱"), true);
    return false;
  }
  pending_action_ = QStringLiteral("reveal");
  client_->access_group_vault_entry(gid_, it->data(kRoleId).toLongLong(),
                                    QStringLiteral("reveal"));
  return true;
}

bool GroupVaultDialog::copy_selected() {
  const auto* it = stream_->currentItem();
  if (!it || it->flags() == Qt::NoItemFlags) return false;
  if (state_ != State::kUnlocked) {
    set_status(QStringLiteral("先解锁密码箱"), true);
    return false;
  }
  pending_action_ = QStringLiteral("copy");
  client_->access_group_vault_entry(gid_, it->data(kRoleId).toLongLong(),
                                    QStringLiteral("copy"));
  return true;
}

bool GroupVaultDialog::delete_selected() {
  const auto* it = stream_->currentItem();
  if (!it || it->flags() == Qt::NoItemFlags) return false;
  client_->delete_group_vault_entry(gid_, it->data(kRoleId).toLongLong());
  return true;
}

void GroupVaultDialog::open_audit() {
  auto* dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(QStringLiteral("密码箱访问审计"));
  dlg->resize(560, 420);
  auto* layout = new QVBoxLayout(dlg);
  audit_list_ = new QListWidget(dlg);
  audit_list_->setWordWrap(true);
  layout->addWidget(audit_list_, 1);
  auto* btn_close = new QPushButton(QStringLiteral("关闭"), dlg);
  layout->addWidget(btn_close);
  connect(btn_close, &QPushButton::clicked, dlg, &QDialog::close);
  connect(dlg, &QObject::destroyed, this, [this] { audit_list_ = nullptr; });
  // 审计填充（连接挂 dlg 生命周期：窗销毁连接自断）
  connect(client_, &FilesClient::group_vault_audit_listed, dlg,
          [this](const QJsonArray& rows) {
            if (!audit_list_) return;
            audit_list_->clear();
            for (const auto& v : rows) {
              const QJsonObject o = v.toObject();
              auto* item = new QListWidgetItem(audit_list_);
              item->setText(QStringLiteral("%1　%2　%3　条目#%4")
                                .arg(fmt_time(static_cast<qint64>(
                                         o.value(QStringLiteral("ts_ms"))
                                             .toDouble())),
                                     o.value(QStringLiteral("actor"))
                                         .toString(),
                                     o.value(QStringLiteral("action"))
                                         .toString() ==
                                             QStringLiteral("reveal")
                                         ? QStringLiteral("查看")
                                         : QStringLiteral("复制"),
                                     QString::number(static_cast<qint64>(
                                         o.value(QStringLiteral("entry_id"))
                                             .toDouble()))));
            }
            if (rows.isEmpty()) {
              auto* item = new QListWidgetItem(audit_list_);
              item->setText(QStringLiteral("暂无访问记录"));
              item->setFlags(Qt::NoItemFlags);
            }
          });
  client_->group_vault_audit(gid_);
  dlg->show(); // 非模态：信号驱动填充
}

} // namespace memex::client
