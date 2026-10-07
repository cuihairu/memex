#include "group_vault_dialog.hpp"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

#include <engine/collab/files_client.hpp>

#include "group_vault_crypto.hpp"

namespace memex::client {

namespace {
// 列表条目角色：id（审计窗同构取值）
constexpr int kRoleId = Qt::UserRole + 2;
// 建箱/重包裹 KDF 迭代数（wingman 口径 600k；服务端下限 10000）
constexpr int kKdfIters = 600000;

QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms)
      .toString(QStringLiteral("MM-dd HH:mm"));
}
} // namespace

GroupVaultDialog::GroupVaultDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群密码箱"));
  resize(640, 560);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接：%1@%2:%3（箱密码只在本机派生）")
                   .arg(client_->account(), host_->text(), port_->text()),
               false);
    btn_connect_->setEnabled(false);
    for (QLineEdit* e : {host_, port_, account_, password_}) {
      e->setEnabled(false);
    }
    refresh();
  });
  connect(client_, &FilesClient::login_failed, this, [this](const QString& r) {
    set_status(QStringLiteral("连接失败：%1").arg(r), true);
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
                client_->rekey_group_vault(gid_, new_salt_, new_iters_,
                                           vault::wrap_dek(new_kek_, new_dek_));
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

  // —— 连接区（登录成功后整体停用）——
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* conn_row = new QHBoxLayout;
  host_ = new QLineEdit(
      settings.value(QStringLiteral("host"), QStringLiteral("127.0.0.1"))
          .toString(),
      this);
  port_ = new QLineEdit(
      settings.value(QStringLiteral("files_port"), QStringLiteral("24561"))
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
  conn_row->addWidget(new QLabel(QStringLiteral("服务器"), this));
  conn_row->addWidget(host_);
  conn_row->addWidget(new QLabel(QStringLiteral("文件面端口"), this));
  conn_row->addWidget(port_);
  conn_row->addWidget(new QLabel(QStringLiteral("账号"), this));
  conn_row->addWidget(account_);
  conn_row->addWidget(new QLabel(QStringLiteral("口令"), this));
  conn_row->addWidget(password_);
  conn_row->addWidget(btn_connect_);
  root->addLayout(conn_row);

  // —— 解锁区（已建箱未解锁时可见；建箱按钮仅未建箱时可见）——
  auto* unlock_row = new QHBoxLayout;
  btn_init_ = new QPushButton(QStringLiteral("建箱…"), this);
  btn_init_->setToolTip(
      QStringLiteral("建箱仅群主/管理员；箱密码服务端不可找回"));
  vault_pass_ = new QLineEdit(this);
  vault_pass_->setEchoMode(QLineEdit::Password);
  vault_pass_->setPlaceholderText(QStringLiteral("箱密码"));
  vault_pass_->setMaximumWidth(200);
  btn_unlock_ = new QPushButton(QStringLiteral("解锁"), this);
  unlock_row->addWidget(btn_init_);
  unlock_row->addWidget(vault_pass_, 1);
  unlock_row->addWidget(btn_unlock_);
  root->addLayout(unlock_row);

  // —— 条目列表（掩码：名称 · 账号 · 维护人 · 时间）——
  stream_ = new QListWidget(this);
  stream_->setAlternatingRowColors(true);
  stream_->setWordWrap(true);
  root->addWidget(stream_, 1);

  // —— 查看详情区（reveal 落这里；复制不落明文）——
  details_ = new QLabel(this);
  details_->setWordWrap(true);
  details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details_->hide();
  root->addWidget(details_);

  // —— 条目动作行 ——
  auto* act_row = new QHBoxLayout;
  btn_new_ = new QPushButton(QStringLiteral("新建条目"), this);
  btn_edit_ = new QPushButton(QStringLiteral("编辑"), this);
  btn_reveal_ = new QPushButton(QStringLiteral("查看"), this);
  btn_copy_ = new QPushButton(QStringLiteral("复制密码"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
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
  act_row->addStretch(1);
  root->addLayout(act_row);

  // —— 箱管理行（改箱密码/重置箱/审计=群主管理员；名单仅群主）——
  auto* mgmt_row = new QHBoxLayout;
  btn_pass_ = new QPushButton(QStringLiteral("改箱密码…"), this);
  btn_reset_ = new QPushButton(QStringLiteral("重置箱…"), this);
  btn_acl_ = new QPushButton(QStringLiteral("授权名单…"), this);
  btn_audit_ = new QPushButton(QStringLiteral("查看审计…"), this);
  btn_pass_->setEnabled(false);
  btn_reset_->setEnabled(false);
  btn_acl_->setEnabled(false);
  btn_audit_->setEnabled(false);
  btn_pass_->setToolTip(QStringLiteral("同箱钥匙重包裹：条目密文不动"));
  btn_reset_->setToolTip(
      QStringLiteral("新箱钥匙全量重加密：逐条取回重加密落回（均留痕）"));
  btn_acl_->setToolTip(QStringLiteral("授权名单仅群主可改；空名单=全成员可解锁"));
  btn_audit_->setToolTip(QStringLiteral("谁/何时/哪条/查看还是复制（倒序）"));
  mgmt_row->addWidget(btn_pass_);
  mgmt_row->addWidget(btn_reset_);
  mgmt_row->addWidget(btn_acl_);
  mgmt_row->addWidget(btn_audit_);
  mgmt_row->addStretch(1);
  root->addLayout(mgmt_row);

  // —— 状态行 ——
  status_ = new QLabel(
      QStringLiteral("未连接（与协作面同源账号；文件面端口独立）"), this);
  status_->setWordWrap(true);
  root->addWidget(status_);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(host_->text().trimmed(),
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

QString GroupVaultDialog::pack_secret(const QString& password,
                                      const QString& url,
                                      const QString& note) {
  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("password"), password},
                                {QStringLiteral("url"), url},
                                {QStringLiteral("note"), note}})
          .toJson(QJsonDocument::Compact));
}

bool GroupVaultDialog::unpack_secret(const QString& plaintext,
                                     QString* password, QString* url,
                                     QString* note) {
  if (plaintext.isEmpty()) return false;
  const QJsonDocument doc = QJsonDocument::fromJson(plaintext.toUtf8());
  if (!doc.isObject()) return false;
  const QJsonObject o = doc.object();
  *password = o.value(QStringLiteral("password")).toString();
  *url = o.value(QStringLiteral("url")).toString();
  *note = o.value(QStringLiteral("note")).toString();
  return true;
}

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
  const QString wrapped = vault::wrap_dek(new_kek_, new_dek_);
  if (new_salt_.isEmpty() || wrapped.isEmpty()) {
    set_status(QStringLiteral("新材料生成失败"), true);
    return false;
  }
  client_->rekey_group_vault(gid_, new_salt_, new_iters_, wrapped);
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
    client_->rekey_group_vault(gid_, new_salt_, new_iters_,
                               vault::wrap_dek(new_kek_, new_dek_));
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
