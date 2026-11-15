// group_vault_dialog.cpp 拆分模块（LAY-017）：连接与解锁生命周期（connect/set_group/init/unlock/refresh/改密/重置/ACL）；公共面见 common.hpp，方法声明在 group_vault_dialog.hpp。
#include "common.hpp"

namespace memex::client {

void GroupVaultDialog::connect_to(const QString& host, quint16 files_port,
                                  const QString& acc, const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  host_->setText(host);
  port_->setText(QString::number(files_port));
  account_->setText(acc);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"), QString::number(files_port));
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool GroupVaultDialog::is_connected() const { return client_->is_logged_in(); }

void GroupVaultDialog::set_group(quint64 gid, const QString& group_name) {
  if (gid_ == gid) return;
  gid_ = gid;
  setWindowTitle(QStringLiteral("群密码箱（%1）").arg(group_name));
  // 换群即锁：清钥匙与在途状态（显式解锁语义不跨群残留）
  state_ = State::kUnknown;
  exists_ = false;
  kdf_salt_.clear();
  kdf_iters_ = 0;
  wrapped_.clear();
  acl_.clear();
  kek_.clear();
  dek_.clear();
  unlocked_salt_.clear();
  end_edit();
  pending_action_.clear();
  reset_pending_ = false;
  reset_queue_.clear();
  edit_manual_ = false;
  details_->hide();
  details_->clear();
  stream_->clear();
  apply_state();
  if (client_->is_logged_in()) refresh();
}

bool GroupVaultDialog::init_vault(const QString& password) {
  if (gid_ == 0 || !client_->is_logged_in()) {
    set_status(QStringLiteral("先连接服务器"), true);
    return false;
  }
  if (exists_) {
    set_status(QStringLiteral("该群已建箱"), true);
    return false;
  }
  if (password.isEmpty()) {
    set_status(QStringLiteral("箱密码不能为空"), true);
    return false;
  }
  // 建箱材料全本地：随机盐 16B＋随机 DEK 32B＋600k 轮 KEK 包裹
  kdf_salt_ = vault::random_b64(16);
  kdf_iters_ = kKdfIters;
  dek_ = QByteArray::fromBase64(vault::random_b64(32).toLatin1());
  wrapped_ =
      vault::wrap_dek(vault::derive_kek(password, kdf_salt_, kdf_iters_), dek_);
  if (kdf_salt_.isEmpty() || wrapped_.isEmpty()) {
    set_status(QStringLiteral("建箱材料生成失败"), true);
    return false;
  }
  client_->init_group_vault(gid_, kdf_salt_, kdf_iters_, wrapped_);
  return true;
}

bool GroupVaultDialog::unlock(const QString& password) {
  if (gid_ == 0 || !client_->is_logged_in()) {
    set_status(QStringLiteral("先连接服务器"), true);
    return false;
  }
  if (!exists_) {
    set_status(QStringLiteral("该群尚未建箱"), true);
    return false;
  }
  if (password.isEmpty()) {
    set_status(QStringLiteral("箱密码不能为空"), true);
    return false;
  }
  // 本地派生验签：错口令不发声（不发任何请求）
  const QByteArray kek = vault::derive_kek(password, kdf_salt_, kdf_iters_);
  const QByteArray dek = vault::unwrap_dek(kek, wrapped_);
  if (dek.isEmpty()) {
    set_status(QStringLiteral("箱密码错误"), true);
    return false;
  }
  kek_ = kek;
  dek_ = dek;
  unlocked_salt_ = kdf_salt_;
  state_ = State::kUnlocked;
  apply_state();
  vault_pass_->clear();
  set_status(QStringLiteral("已解锁"), false);
  client_->list_group_vault_entries(gid_);
  return true;
}

void GroupVaultDialog::refresh() {
  if (gid_ == 0 || !client_->is_logged_in()) return;
  client_->group_vault_info(gid_);
}

bool GroupVaultDialog::change_vault_password(const QString& old_pass,
                                             const QString& new_pass) {
  if (state_ != State::kUnlocked) {
    set_status(QStringLiteral("先解锁密码箱"), true);
    return false;
  }
  if (new_pass.isEmpty()) {
    set_status(QStringLiteral("新箱密码不能为空"), true);
    return false;
  }
  // 现箱密码本地验签（错口令不发声）
  if (vault::unwrap_dek(vault::derive_kek(old_pass, kdf_salt_, kdf_iters_),
                        wrapped_)
          .isEmpty()) {
    set_status(QStringLiteral("现箱密码错误"), true);
    return false;
  }
  // 同 DEK 重包：新盐新轮数，条目密文不动
  new_salt_ = vault::random_b64(16);
  new_iters_ = kKdfIters;
  new_dek_ = dek_;
  new_kek_ = vault::derive_kek(new_pass, new_salt_, new_iters_);
  new_wrapped_ = vault::wrap_dek(new_kek_, new_dek_);
  if (new_salt_.isEmpty() || new_wrapped_.isEmpty()) {
    set_status(QStringLiteral("新材料生成失败"), true);
    return false;
  }
  client_->rekey_group_vault(gid_, new_salt_, new_iters_, new_wrapped_);
  return true;
}

bool GroupVaultDialog::reset_vault(const QString& old_pass,
                                   const QString& new_pass) {
  if (state_ != State::kUnlocked) {
    set_status(QStringLiteral("先解锁密码箱"), true);
    return false;
  }
  if (new_pass.isEmpty()) {
    set_status(QStringLiteral("新箱密码不能为空"), true);
    return false;
  }
  if (vault::unwrap_dek(vault::derive_kek(old_pass, kdf_salt_, kdf_iters_),
                        wrapped_)
          .isEmpty()) {
    set_status(QStringLiteral("现箱密码错误"), true);
    return false;
  }
  // 新箱钥匙材料（旧 DEK 解出的明文逐条用新 DEK 重加密）
  new_salt_ = vault::random_b64(16);
  new_iters_ = kKdfIters;
  new_dek_ = QByteArray::fromBase64(vault::random_b64(32).toLatin1());
  new_kek_ = vault::derive_kek(new_pass, new_salt_, new_iters_);
  if (new_salt_.isEmpty() || new_dek_.isEmpty() || new_kek_.isEmpty()) {
    set_status(QStringLiteral("新材料生成失败"), true);
    return false;
  }
  QStringList ids;
  for (int i = 0; i < stream_->count(); ++i) {
    const auto* it = stream_->item(i);
    if (it->flags() != Qt::NoItemFlags) ids.append(it->data(kRoleId).toString());
  }
  if (ids.isEmpty()) {
    // 无条目：直接覆盖包裹块（无需重加密）
    new_wrapped_ = vault::wrap_dek(new_kek_, new_dek_);
    client_->rekey_group_vault(gid_, new_salt_, new_iters_, new_wrapped_);
    return true;
  }
  reset_total_ = ids.size();
  reset_saved_ = 0;
  reset_pending_ = true;
  reset_queue_ = ids;
  pending_action_ = QStringLiteral("reset");
  const qint64 first = reset_queue_.takeFirst().toLongLong();
  client_->access_group_vault_entry(gid_, first, QStringLiteral("reveal"));
  return true;
}

bool GroupVaultDialog::set_acl(const QStringList& accounts) {
  if (gid_ == 0 || !client_->is_logged_in()) {
    set_status(QStringLiteral("先连接服务器"), true);
    return false;
  }
  if (!exists_) {
    set_status(QStringLiteral("该群尚未建箱"), true);
    return false;
  }
  client_->set_group_vault_acl(gid_, accounts);
  return true;
}

} // namespace memex::client
