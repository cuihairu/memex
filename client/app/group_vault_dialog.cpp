#include "group_vault_dialog/common.hpp"

namespace memex::client {

GroupVaultDialog::GroupVaultDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群密码箱"));
  resize(1000, 660);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接：%1@%2:%3（箱密码只在本机派生）")
                   .arg(client_->account(), host_->text(), port_->text()),
               false);
    conn_badge_->setText(
        QStringLiteral("已连接（箱密码仅本地参与解密，服务端不可找回）"));
    conn_badge_->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
            .arg(QColor(Qt::green).name()));
    btn_connect_->setEnabled(false);
    for (QLineEdit* e : {host_, port_, account_, password_}) {
      e->setEnabled(false);
    }
    refresh();
  });
  connect(client_, &FilesClient::login_failed, this, [this](const QString& r) {
    set_status(QStringLiteral("连接失败：%1").arg(r), true);
    conn_badge_->setText(QStringLiteral("未连接"));
    conn_badge_->setStyleSheet(QString());
    btn_connect_->setEnabled(true);
  });
  connect(client_, &FilesClient::group_vault_info_fetched, this,
          [this](const QJsonObject& info) {
            exists_ = info.value(QStringLiteral("exists")).toBool();
            if (exists_) {
              kdf_salt_ = info.value(QStringLiteral("kdf_salt")).toString();
              kdf_iters_ = info.value(QStringLiteral("kdf_iters")).toInt();
              wrapped_ = info.value(QStringLiteral("wrapped_dek")).toString();
              acl_.clear();
              for (const auto& a :
                   info.value(QStringLiteral("acl")).toArray()) {
                acl_.append(a.toString());
              }
            }
            // 已解锁且盐未变＝同一把箱钥匙仍有效；否则回落锁定态
            if (exists_ && !dek_.isEmpty() && unlocked_salt_ == kdf_salt_) {
              state_ = State::kUnlocked;
            } else {
              state_ = exists_ ? State::kLocked : State::kNoVault;
            }
            apply_state();
            if (state_ == State::kUnlocked) {
              client_->list_group_vault_entries(gid_);
            } else if (state_ == State::kNoVault) {
              stream_->clear();
              auto* it = new QListWidgetItem(stream_);
              it->setText(QStringLiteral("（该群尚未建箱）"));
              it->setFlags(Qt::NoItemFlags);
            }
          });
  connect(client_, &FilesClient::group_vault_initialized, this, [this] {
    exists_ = true;
    unlocked_salt_ = kdf_salt_;
    state_ = State::kUnlocked;
    apply_state();
    set_status(QStringLiteral("已建箱并解锁（箱密码请牢记：服务端不可找回）"),
               false);
    client_->list_group_vault_entries(gid_);
  });
  connect(client_, &FilesClient::group_vault_listed, this,
          [this](const QJsonArray& entries) {
            stream_->clear();
            for (const auto& v : entries) {
              const QJsonObject o = v.toObject();
              const qint64 id = static_cast<qint64>(
                  o.value(QStringLiteral("id")).toDouble());
              // 掩码面：列表只露名称/账号（密文只经 access 每访留痕）
              auto* it = new QListWidgetItem(stream_);
              it->setText(QStringLiteral("%1 · %2 · %3 · %4")
                              .arg(o.value(QStringLiteral("name")).toString(),
                                   o.value(QStringLiteral("account_name"))
                                       .toString(),
                                   o.value(QStringLiteral("created_by"))
                                       .toString(),
                                   fmt_time(static_cast<qint64>(
                                       o.value(QStringLiteral("updated_ms"))
                                           .toDouble()))));
              it->setData(kRoleId, id);
            }
            if (entries.isEmpty()) {
              auto* it = new QListWidgetItem(stream_);
              it->setText(QStringLiteral("（密码箱暂无条目）"));
              it->setFlags(Qt::NoItemFlags);
            }
            set_status(QStringLiteral("共 %1 条").arg(entries.size()), false);
          });
  connect(client_, &FilesClient::group_vault_entry_saved, this,
          [this](qint64) {
            if (reset_pending_) {
              // 重置箱全量重加密：计数每条落回；全部落回→覆盖包裹块收尾
              if (++reset_saved_ >= reset_total_) {
                new_wrapped_ = vault::wrap_dek(new_kek_, new_dek_);
                client_->rekey_group_vault(gid_, new_salt_, new_iters_,
                                           new_wrapped_);
              }
              return;
            }
            const bool was_edit = editing_id_ > 0;
            end_edit();
            set_status(was_edit ? QStringLiteral("条目已保存")
                                : QStringLiteral("条目已新建"),
                       false);
            client_->list_group_vault_entries(gid_);
          });
  connect(client_, &FilesClient::group_vault_entry_deleted, this,
          [this](qint64) {
            set_status(QStringLiteral("条目已删除"), false);
            client_->list_group_vault_entries(gid_);
          });
  connect(client_, &FilesClient::group_vault_accessed, this,
          [this](const QJsonObject& entry) {
            const qint64 id = static_cast<qint64>(
                entry.value(QStringLiteral("id")).toDouble());
            QString password, url, note;
            if (!unpack_secret(
                    vault::decrypt_secret(
                        dek_, entry.value(QStringLiteral("secret_ct"))
                                  .toString(),
                        entry.value(QStringLiteral("secret_nonce"))
                            .toString()),
                    &password, &url, &note)) {
              set_status(QStringLiteral("解密失败（密文损坏或钥匙不符）"), true);
              pending_action_.clear();
              return;
            }
            const QString name = entry.value(QStringLiteral("name")).toString();
            const QString acc =
                entry.value(QStringLiteral("account_name")).toString();
            if (pending_action_ == QStringLiteral("reveal")) {
              details_->setText(
                  QStringLiteral("【%1】账号：%2\n密码：%3\nURL：%4\n备注：%5")
                      .arg(name, acc, password,
                           url.isEmpty() ? QStringLiteral("—") : url,
                           note.isEmpty() ? QStringLiteral("—") : note));
              details_->show();
              set_status(QStringLiteral("已查看（服务端已留痕）"), false);
            } else if (pending_action_ == QStringLiteral("copy")) {
              QApplication::clipboard()->setText(password);
              details_->hide();
              set_status(QStringLiteral("密码已复制到剪贴板（服务端已留痕）"),
                         false);
            } else if (pending_action_ == QStringLiteral("edit")) {
              seed_name_ = name;
              seed_account_ = acc;
              seed_password_ = password;
              seed_url_ = url;
              seed_note_ = note;
              if (edit_manual_) {
                edit_manual_ = false;
                // 手工路径：取回明文预填编辑对话框（确定即重加密落回）
                QString n = seed_name_, a = seed_account_, p = seed_password_,
                        u = seed_url_, t = seed_note_;
                if (entry_dialog(&n, &a, &p, &u, &t, n, a, p, u, t)) {
                  submit_entry(n, a, p, u, t);
                  return; // 在途保存（成功/失败各走其信号）
                }
                end_edit();
                set_status(QStringLiteral("已取消编辑"), false);
              } else {
                set_status(QStringLiteral("正在编辑条目 #%1（submit_entry 保存；"
                                          "再点编辑＝取消）")
                               .arg(editing_id_),
                           false);
              }
            } else if (pending_action_ == QStringLiteral("reset")) {
              // 全量重加密：旧 DEK 解出明文→新 DEK 加密落回（id 不变）
              QString nonce_b64;
              const QString ct = vault::encrypt_secret(
                  new_dek_, pack_secret(password, url, note), &nonce_b64);
              if (ct.isEmpty()) {
                set_status(QStringLiteral("重加密失败，重置中止"), true);
                reset_pending_ = false;
                reset_queue_.clear();
              } else {
                client_->save_group_vault_entry(gid_, name, acc, ct, nonce_b64,
                                                id);
              }
              if (!reset_queue_.isEmpty()) {
                // 队列还有下一条：继续取回（pending_action_ 保持 reset）
                const qint64 next =
                    reset_queue_.takeFirst().toLongLong();
                client_->access_group_vault_entry(gid_, next,
                                                  QStringLiteral("reveal"));
                return;
              }
            }
            pending_action_.clear();
          });
  connect(client_, &FilesClient::group_vault_rekeyed, this, [this] {
    kek_ = new_kek_;
    kdf_salt_ = new_salt_;
    kdf_iters_ = new_iters_;
    wrapped_ = new_wrapped_; // 新包裹块落缓存（解锁验签面）
    new_wrapped_.clear();
    unlocked_salt_ = new_salt_;
    if (reset_pending_) {
      reset_pending_ = false;
      dek_ = new_dek_;
      set_status(QStringLiteral("重置完成（新箱钥匙＋全量重加密）"), false);
    } else {
      set_status(QStringLiteral("箱密码已更改（条目密文未动，仍同一把箱钥匙）"),
                 false);
    }
    client_->list_group_vault_entries(gid_);
  });
  connect(client_, &FilesClient::group_vault_acl_set, this, [this] {
    set_status(QStringLiteral("授权名单已更新"), false);
    refresh();
  });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败[%1]（%2）：%3")
                           .arg(op, status > 0 ? QString::number(status)
                                               : QStringLiteral("网络"),
                                error),
                       true);
            if (op == QStringLiteral("group-vault.access") &&
                pending_action_ == QStringLiteral("reset")) {
              // 重置中止：包裹块未覆盖，旧箱密码仍可解锁后重试
              //（已落回的条目是新 DEK、未落回的是旧 DEK——服务端条目仍各各
              // 可解，只是混了两把 DEK；重试会再次全量取回重加密拉平）
              reset_pending_ = false;
              reset_queue_.clear();
              pending_action_.clear();
              set_status(QStringLiteral("重置中止（包裹块未覆盖，旧箱密码仍有效；"
                                        "可重试）"),
                         true);
            }
          });
}

void GroupVaultDialog::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 12, 12, 12);
  root->setSpacing(8);

  // —— 连接区（登录成功后整体停用；原型 conn-row 右上「已连接」徽章）——
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* conn_row = new QHBoxLayout;
  host_ = new QLineEdit(
      settings.value(QStringLiteral("host"), default_collab_host())
          .toString(),
      this);
  port_ = new QLineEdit(
      settings.value(QStringLiteral("files_port"), default_files_port())
          .toString(),
      this);
  port_->setMaximumWidth(90);
  account_ = new QLineEdit(
      settings.value(QStringLiteral("account")).toString(), this);
  account_->setMaximumWidth(140);
  password_ = new QLineEdit(this);
  password_->setEchoMode(QLineEdit::Password);
  password_->setMaximumWidth(140);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn_badge_ = new QLabel(QStringLiteral("未连接"), this);
  conn_badge_->setObjectName(QStringLiteral("vault_conn_badge"));
  conn_row->addWidget(new QLabel(QStringLiteral("服务器"), this));
  conn_row->addWidget(host_);
  conn_row->addWidget(new QLabel(QStringLiteral("文件面端口"), this));
  conn_row->addWidget(port_);
  conn_row->addWidget(new QLabel(QStringLiteral("账号"), this));
  conn_row->addWidget(account_);
  conn_row->addWidget(new QLabel(QStringLiteral("口令"), this));
  conn_row->addWidget(password_);
  conn_row->addWidget(btn_connect_);
  conn_row->addStretch(1);
  conn_row->addWidget(conn_badge_);
  root->addLayout(conn_row);

  // —— 双栏 split（原型 .split）：左 400px 条目列＋右详情列 ——
  auto* split = new QHBoxLayout;
  split->setSpacing(8);
  auto* left = new QWidget(this);
  left->setFixedWidth(400);
  auto* left_lay = new QVBoxLayout(left);
  left_lay->setContentsMargins(0, 0, 0, 0);
  left_lay->setSpacing(8);

  // —— 解锁区（已建箱未解锁时可见；建箱按钮仅未建箱时可见）——
  auto* unlock_row = new QHBoxLayout;
  btn_init_ = new QPushButton(QStringLiteral("建箱…"), left);
  btn_init_->setToolTip(
      QStringLiteral("建箱仅群主/管理员；箱密码服务端不可找回"));
  vault_pass_ = new QLineEdit(left);
  vault_pass_->setEchoMode(QLineEdit::Password);
  vault_pass_->setPlaceholderText(QStringLiteral("箱密码"));
  vault_pass_->setMaximumWidth(200);
  btn_unlock_ = new QPushButton(QStringLiteral("解锁"), left);
  unlock_row->addWidget(btn_init_);
  unlock_row->addWidget(vault_pass_, 1);
  unlock_row->addWidget(btn_unlock_);
  left_lay->addLayout(unlock_row);

  // —— 条目列表（掩码：名称 · 账号 · 维护人 · 时间）——
  stream_ = new QListWidget(left);
  stream_->setAlternatingRowColors(true);
  stream_->setWordWrap(true);
  left_lay->addWidget(stream_, 1);

  // —— 条目动作行 ——
  auto* act_row = new QHBoxLayout;
  btn_new_ = new QPushButton(QStringLiteral("新建条目"), left);
  btn_edit_ = new QPushButton(QStringLiteral("编辑"), left);
  btn_reveal_ = new QPushButton(QStringLiteral("查看"), left);
  btn_copy_ = new QPushButton(QStringLiteral("复制密码"), left);
  btn_delete_ = new QPushButton(QStringLiteral("删除"), left);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), left);
  btn_edit_->setEnabled(false);
  btn_reveal_->setEnabled(false);
  btn_copy_->setEnabled(false);
  btn_delete_->setEnabled(false);
  btn_new_->setToolTip(QStringLiteral("维护条目=群主/管理员"));
  btn_edit_->setToolTip(QStringLiteral("维护条目=群主/管理员；取回明文会留痕"));
  btn_copy_->setToolTip(
      QStringLiteral("复制=显式动作，服务端留痕（谁/何时/哪条）"));
  btn_delete_->setToolTip(QStringLiteral("删除恒归群主/管理员"));
  act_row->addWidget(btn_new_);
  act_row->addWidget(btn_edit_);
  act_row->addWidget(btn_reveal_);
  act_row->addWidget(btn_copy_);
  act_row->addWidget(btn_delete_);
  act_row->addWidget(btn_refresh_);
  left_lay->addLayout(act_row);

  // —— 箱管理区（原型 .admin：改箱密码/重置箱/审计=群主管理员；名单仅
  //    群主；flex-wrap 换行≈按钮一行＋右注独占一行）——
  auto* mgmt = new QVBoxLayout;
  mgmt->setSpacing(2);
  auto* mgmt_row = new QHBoxLayout;
  btn_pass_ = new QPushButton(QStringLiteral("改箱密码…"), left);
  btn_reset_ = new QPushButton(QStringLiteral("重置箱…"), left);
  btn_acl_ = new QPushButton(QStringLiteral("授权名单…"), left);
  btn_audit_ = new QPushButton(QStringLiteral("查看审计…"), left);
  btn_pass_->setEnabled(false);
  btn_reset_->setEnabled(false);
  btn_acl_->setEnabled(false);
  btn_audit_->setEnabled(false);
  btn_pass_->setToolTip(QStringLiteral("同箱钥匙重包裹：条目密文不动"));
  btn_reset_->setToolTip(
      QStringLiteral("新箱钥匙全量重加密：逐条取回重加密落回（均留痕）"));
  btn_acl_->setToolTip(QStringLiteral("授权名单仅群主可改；空名单=全成员可解锁"));
  btn_audit_->setToolTip(QStringLiteral("谁/何时/哪条/查看还是复制（倒序）"));
  mgmt_row->addWidget(new QLabel(QStringLiteral("群主/管理员："), left));
  mgmt_row->addWidget(btn_pass_);
  mgmt_row->addWidget(btn_reset_);
  mgmt_row->addWidget(btn_acl_);
  mgmt_row->addWidget(btn_audit_);
  mgmt_row->addStretch(1);
  mgmt->addLayout(mgmt_row);
  auto* mgmt_note = new QLabel(
      QStringLiteral("默认全体成员可解锁；群主可收紧为仅授权名单"), left);
  mgmt_note->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  mgmt->addWidget(mgmt_note);
  left_lay->addLayout(mgmt);
  split->addWidget(left);

  // —— 详情列（原型 .det-col：详情卡＋硬性分离告警）——
  auto* right = new QWidget(this);
  auto* right_lay = new QVBoxLayout(right);
  right_lay->setContentsMargins(0, 0, 0, 0);
  right_lay->setSpacing(8);
  auto* det_box = new QGroupBox(
      QStringLiteral("条目详情 · 查看/复制均为显式动作，服务端全留痕"), right);
  auto* det_lay = new QVBoxLayout(det_box);
  details_ = new QLabel(det_box);
  details_->setWordWrap(true);
  details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details_->hide();
  det_lay->addWidget(details_);
  right_lay->addWidget(det_box, 1);
  auto* warn = new QLabel(
      QStringLiteral("⚠ 与群备忘录（明文）硬性分离：密码类内容只进本箱；箱"
                     "密码服务端不可找回，重置＝全箱重加密后旧条目不可恢复。"),
      right);
  warn->setWordWrap(true);
  right_lay->addWidget(warn);
  split->addWidget(right, 1);
  root->addLayout(split, 1);

  // —— 状态行 ——
  status_ = new QLabel(
      QStringLiteral("未连接（与协作面同源账号；文件面端口独立）"), this);
  status_->setWordWrap(true);
  root->addWidget(status_);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(resolve_conn_host(host_->text().trimmed()),
               static_cast<quint16>(port_->text().toUInt()),
               account_->text().trimmed(), password_->text());
  });
  connect(password_, &QLineEdit::returnPressed, this, [this] {
    if (btn_connect_->isEnabled()) btn_connect_->click();
  });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  connect(btn_init_, &QPushButton::clicked, this, [this] {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("建箱"));
    auto* layout = new QVBoxLayout(&dlg);
    auto* p1 = new QLineEdit(&dlg);
    p1->setEchoMode(QLineEdit::Password);
    p1->setPlaceholderText(QStringLiteral("箱密码（服务端不可找回）"));
    auto* p2 = new QLineEdit(&dlg);
    p2->setEchoMode(QLineEdit::Password);
    p2->setPlaceholderText(QStringLiteral("再输一遍"));
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    layout->addWidget(p1);
    layout->addWidget(p2);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    if (p1->text().isEmpty() || p1->text() != p2->text()) {
      set_status(QStringLiteral("两次输入不一致或为空"), true);
      return;
    }
    init_vault(p1->text());
  });
  connect(btn_unlock_, &QPushButton::clicked, this,
          [this] { unlock(vault_pass_->text()); });
  connect(vault_pass_, &QLineEdit::returnPressed, this,
          [this] { unlock(vault_pass_->text()); });

  connect(stream_, &QListWidget::itemSelectionChanged, this, [this] {
    const bool has = state_ == State::kUnlocked &&
                     stream_->currentItem() != nullptr &&
                     stream_->currentItem()->flags() != Qt::NoItemFlags;
    btn_edit_->setEnabled(has);
    btn_reveal_->setEnabled(has);
    btn_copy_->setEnabled(has);
    btn_delete_->setEnabled(has);
  });
  connect(btn_new_, &QPushButton::clicked, this, [this] {
    QString name, acc, pass, url, note;
    if (!entry_dialog(&name, &acc, &pass, &url, &note, QString(), QString(),
                      QString(), QString(), QString())) {
      return;
    }
    submit_entry(name, acc, pass, url, note);
  });
  connect(btn_edit_, &QPushButton::clicked, this, [this] {
    edit_manual_ = true;
    if (!edit_selected()) edit_manual_ = false;
  });
  connect(btn_reveal_, &QPushButton::clicked, this,
          [this] { reveal_selected(); });
  connect(btn_copy_, &QPushButton::clicked, this, [this] { copy_selected(); });
  connect(btn_delete_, &QPushButton::clicked, this, [this] {
    const auto* it = stream_->currentItem();
    if (!it || it->flags() == Qt::NoItemFlags) return;
    if (QMessageBox::question(this, QStringLiteral("删除"),
                              QStringLiteral("删除这条密码箱条目？")) !=
        QMessageBox::Yes) {
      return;
    }
    delete_selected();
  });
  connect(btn_pass_, &QPushButton::clicked, this, [this] {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("改箱密码"));
    auto* layout = new QVBoxLayout(&dlg);
    auto* p0 = new QLineEdit(&dlg);
    p0->setEchoMode(QLineEdit::Password);
    p0->setPlaceholderText(QStringLiteral("现箱密码"));
    auto* p1 = new QLineEdit(&dlg);
    p1->setEchoMode(QLineEdit::Password);
    p1->setPlaceholderText(QStringLiteral("新箱密码（条目密文不动）"));
    auto* p2 = new QLineEdit(&dlg);
    p2->setEchoMode(QLineEdit::Password);
    p2->setPlaceholderText(QStringLiteral("再输一遍"));
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    layout->addWidget(p0);
    layout->addWidget(p1);
    layout->addWidget(p2);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    if (p1->text().isEmpty() || p1->text() != p2->text()) {
      set_status(QStringLiteral("两次新密码不一致或为空"), true);
      return;
    }
    change_vault_password(p0->text(), p1->text());
  });
  connect(btn_reset_, &QPushButton::clicked, this, [this] {
    if (QMessageBox::question(
            this, QStringLiteral("重置箱"),
            QStringLiteral("换新箱钥匙并全量重加密？（逐条取回明文重加密落回，"
                           "每条都会留下查看痕迹）")) != QMessageBox::Yes) {
      return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("重置箱"));
    auto* layout = new QVBoxLayout(&dlg);
    auto* p0 = new QLineEdit(&dlg);
    p0->setEchoMode(QLineEdit::Password);
    p0->setPlaceholderText(QStringLiteral("现箱密码"));
    auto* p1 = new QLineEdit(&dlg);
    p1->setEchoMode(QLineEdit::Password);
    p1->setPlaceholderText(QStringLiteral("新箱密码"));
    auto* p2 = new QLineEdit(&dlg);
    p2->setEchoMode(QLineEdit::Password);
    p2->setPlaceholderText(QStringLiteral("再输一遍"));
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    layout->addWidget(p0);
    layout->addWidget(p1);
    layout->addWidget(p2);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    if (p1->text().isEmpty() || p1->text() != p2->text()) {
      set_status(QStringLiteral("两次新密码不一致或为空"), true);
      return;
    }
    reset_vault(p0->text(), p1->text());
  });
  connect(btn_acl_, &QPushButton::clicked, this, [this] {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("授权名单"));
    dlg.resize(420, 140);
    auto* layout = new QVBoxLayout(&dlg);
    auto* edit = new QLineEdit(acl_.join(QStringLiteral(" ")), &dlg);
    edit->setPlaceholderText(
        QStringLiteral("可解锁共享箱的账号（空格/逗号分隔；清空=全成员）"));
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                     &QDialog::reject);
    layout->addWidget(edit);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList accounts;
    for (const QString& tok : edit->text().split(
             QRegularExpression(QStringLiteral("[,\\s]+")),
             Qt::SkipEmptyParts)) {
      accounts.append(tok);
    }
    set_acl(accounts);
  });
  connect(btn_audit_, &QPushButton::clicked, this, [this] { open_audit(); });
}

void GroupVaultDialog::apply_state() {
  btn_init_->setVisible(state_ == State::kNoVault);
  const bool locked = state_ == State::kLocked;
  vault_pass_->setVisible(locked);
  btn_unlock_->setVisible(locked);
  const bool unlocked = state_ == State::kUnlocked;
  btn_new_->setEnabled(unlocked);
  btn_refresh_->setEnabled(client_->is_logged_in());
  btn_pass_->setEnabled(unlocked);
  btn_reset_->setEnabled(unlocked);
  btn_acl_->setEnabled(unlocked);
  btn_audit_->setEnabled(unlocked);
  if (!unlocked) {
    btn_edit_->setEnabled(false);
    btn_reveal_->setEnabled(false);
    btn_copy_->setEnabled(false);
    btn_delete_->setEnabled(false);
  }
}

void GroupVaultDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : stream_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

void GroupVaultDialog::end_edit() {
  editing_id_ = 0;
  for (QString* s : {&seed_name_, &seed_account_, &seed_password_, &seed_url_,
                     &seed_note_}) {
    s->clear();
  }
}

QString GroupVaultDialog::status_text() const { return status_->text(); }

int GroupVaultDialog::stream_count() const { return stream_->count(); }

QString GroupVaultDialog::details_text() const { return details_->text(); }

QString GroupVaultDialog::clipboard_text() const {
  return QApplication::clipboard()->text();
}

int GroupVaultDialog::audit_count() const {
  return audit_list_ ? audit_list_->count() : -1;
}

} // namespace memex::client
