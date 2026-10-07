#include "group_memo_dialog.hpp"

#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>

#include <engine/collab/files_client.hpp>

namespace memex::client {

namespace {
// 列表条目角色：id / 标题 / 正文（历史子对话框同构取值）
constexpr int kRoleId = Qt::UserRole + 2;
constexpr int kRoleTitle = Qt::UserRole + 3;
constexpr int kRoleContent = Qt::UserRole + 4;

QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms)
      .toString(QStringLiteral("MM-dd HH:mm"));
}

// R24-4 UI 边界：疑似密码识别（关键词或「password= xxx」类赋值形态）。
// 只提示不拦截——硬禁言会误伤「WiFi 密码请看密码箱」这类合法正文。
bool looks_like_secret(const QString& text) {
  static const QRegularExpression re(
      QStringLiteral("密码|口令|pass(word|wd)?\\s*[=:]|token\\s*[=:]|"
                     "api[_-]?key\\s*[=:]|secret\\s*[=:]|bearer\\s+\\S"),
      QRegularExpression::CaseInsensitiveOption);
  return re.match(text).hasMatch();
}
} // namespace

GroupMemoDialog::GroupMemoDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群备忘录"));
  resize(600, 560);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接：%1@%2:%3")
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
  connect(client_, &FilesClient::group_memo_listed, this,
          [this](const QJsonArray& memos, bool open_edit) {
            open_edit_->blockSignals(true);
            open_edit_->setChecked(open_edit);
            open_edit_->blockSignals(false);
            stream_->clear();
            for (const auto& v : memos) {
              const QJsonObject o = v.toObject();
              const qint64 id = static_cast<qint64>(
                  o.value(QStringLiteral("id")).toDouble());
              const QString title =
                  o.value(QStringLiteral("title")).toString();
              const QString content =
                  o.value(QStringLiteral("content")).toString();
              auto* it = new QListWidgetItem(stream_);
              it->setText(QStringLiteral("%1 · %2 · %3\n%4")
                              .arg(title,
                                   o.value(QStringLiteral("author"))
                                       .toString(),
                                   fmt_time(static_cast<qint64>(
                                       o.value(QStringLiteral("updated_ms"))
                                           .toDouble())),
                                   content));
              it->setData(kRoleId, id);
              it->setData(kRoleTitle, title);
              it->setData(kRoleContent, content);
            }
            if (memos.isEmpty()) {
              auto* it = new QListWidgetItem(stream_);
              it->setText(QStringLiteral("（群备忘录暂无条目）"));
              it->setFlags(Qt::NoItemFlags);
            }
            if (secret_hint_) {
              // R24-4：疑似密码提示随刷新挂住（不被「共 N 条」冲掉）
              set_status(QStringLiteral("共 %1 条（⚠ 疑似密码内容：密码请放群密码箱"
                                        "——备忘录明文共享）")
                             .arg(memos.size()),
                         true);
            } else {
              set_status(QStringLiteral("共 %1 条").arg(memos.size()), false);
            }
          });
  connect(client_, &FilesClient::group_memo_saved, this, [this](qint64) {
    const bool was_edit = editing_id_ > 0;
    end_edit();
    set_status(was_edit ? QStringLiteral("条目已保存")
                        : QStringLiteral("条目已新建"),
               false);
    refresh();
  });
  connect(client_, &FilesClient::group_memo_deleted, this, [this](qint64) {
    set_status(QStringLiteral("条目已删除（修订史一并清除）"), false);
    refresh();
  });
  connect(client_, &FilesClient::group_memo_rolled_back, this, [this](qint64) {
    set_status(QStringLiteral("已回滚（回滚即一次编辑，落了新修订笔）"), false);
    refresh();
  });
  connect(client_, &FilesClient::group_memo_open_edit_set, this,
          [this](bool open) {
            open_edit_->blockSignals(true);
            open_edit_->setChecked(open);
            open_edit_->blockSignals(false);
            set_status(open ? QStringLiteral("已开放全员编辑")
                            : QStringLiteral("已收回编辑权（仅群主/管理员）"),
                       false);
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败[%1]（%2）：%3")
                           .arg(op, status > 0 ? QString::number(status)
                                               : QStringLiteral("网络"),
                                error),
                       true);
            // 开关设置被拒：勾态回滚（勾选框展示的必须与服务端现值一致）
            if (op == QStringLiteral("group-memo.open-edit")) {
              open_edit_->blockSignals(true);
              open_edit_->setChecked(!open_edit_->isChecked());
              open_edit_->blockSignals(false);
            }
          });
}

void GroupMemoDialog::build_ui() {
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

  // —— 搜索 + 开放编辑开关（开关勾选=全员可写；权限由服务端裁决）——
  auto* search_row = new QHBoxLayout;
  search_ = new QLineEdit(this);
  search_->setPlaceholderText(QStringLiteral("搜标题或正文…（回车搜索，清空＝全部）"));
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  search_row->addWidget(search_, 1);
  search_row->addWidget(btn_refresh_);
  root->addLayout(search_row);
  open_edit_ = new QCheckBox(
      QStringLiteral("开放全员编辑（默认仅群主/管理员可写；删除不开放）"),
      this);
  root->addWidget(open_edit_);

  // —— 条目列表 ——
  stream_ = new QListWidget(this);
  stream_->setAlternatingRowColors(true);
  stream_->setWordWrap(true);
  root->addWidget(stream_, 1);

  // —— 动作行 ——
  auto* act_row = new QHBoxLayout;
  btn_new_ = new QPushButton(QStringLiteral("新建条目"), this);
  btn_edit_ = new QPushButton(QStringLiteral("编辑"), this);
  btn_history_ = new QPushButton(QStringLiteral("历史与回滚…"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除"), this);
  btn_edit_->setEnabled(false);
  btn_history_->setEnabled(false);
  btn_delete_->setEnabled(false);
  btn_delete_->setToolTip(QStringLiteral("删除恒归群主/管理员（开放编辑开放的是写，不是删）"));
  act_row->addWidget(btn_new_);
  act_row->addWidget(btn_edit_);
  act_row->addWidget(btn_history_);
  act_row->addWidget(btn_delete_);
  act_row->addStretch(1);
  root->addLayout(act_row);

  // —— 状态行 ——
  status_ = new QLabel(QStringLiteral("未连接（与协作面同源账号；文件面端口独立）"),
                       this);
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
  connect(search_, &QLineEdit::returnPressed, this, [this] { refresh(); });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });

  connect(stream_, &QListWidget::itemSelectionChanged, this, [this] {
    const bool has = stream_->currentItem() != nullptr;
    btn_edit_->setEnabled(has);
    btn_history_->setEnabled(has);
    btn_delete_->setEnabled(has);
  });
  // 手工路径：编辑走模态子对话框（标题+正文两字段）
  connect(btn_edit_, &QPushButton::clicked, this, [this] {
    const auto* it = stream_->currentItem();
    if (!it) return;
    QString title = it->data(kRoleTitle).toString();
    QString content = it->data(kRoleContent).toString();
    if (!edit_dialog(&title, &content, title, content)) return;
    if (!confirm_secret_memo(title, content)) return;
    secret_hint_ = looks_like_secret(title) || looks_like_secret(content);
    client_->save_group_memo(gid_, title.trimmed(), content, it->data(kRoleId).toLongLong());
  });
  connect(btn_new_, &QPushButton::clicked, this, [this] {
    QString title, content;
    if (!edit_dialog(&title, &content, QString(), QString())) return;
    if (!confirm_secret_memo(title, content)) return;
    secret_hint_ = looks_like_secret(title) || looks_like_secret(content);
    client_->save_group_memo(gid_, title.trimmed(), content, 0);
  });
  connect(btn_history_, &QPushButton::clicked, this,
          [this] { open_history(); });
  connect(btn_delete_, &QPushButton::clicked, this, [this] {
    const auto* it = stream_->currentItem();
    if (!it) return;
    if (QMessageBox::question(this, QStringLiteral("删除"),
                              QStringLiteral("删除这条备忘录？（修订史一并清除）")) !=
        QMessageBox::Yes) {
      return;
    }
    client_->delete_group_memo(gid_, it->data(kRoleId).toLongLong());
  });
}

// 标题+正文编辑子对话框（手工新建/编辑共用；OK 回传两字段）
bool GroupMemoDialog::edit_dialog(QString* title, QString* content,
                                  const QString& init_title,
                                  const QString& init_content) {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("备忘录条目"));
  dlg.resize(480, 360);
  auto* layout = new QVBoxLayout(&dlg);
  auto* title_edit = new QLineEdit(init_title, &dlg);
  title_edit->setPlaceholderText(QStringLiteral("标题（必填）"));
  auto* content_edit = new QPlainTextEdit(init_content, &dlg);
  content_edit->setPlaceholderText(
      QStringLiteral("正文（必填；可放代码块、连接信息、值班表等共享知识）"));
  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                           &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(title_edit);
  layout->addWidget(content_edit, 1);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return false;
  *title = title_edit->text().trimmed();
  *content = content_edit->toPlainText();
  return !title->isEmpty() && !content->isEmpty();
}

void GroupMemoDialog::connect_to(const QString& host, quint16 files_port,
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

bool GroupMemoDialog::is_connected() const { return client_->is_logged_in(); }

void GroupMemoDialog::set_group(quint64 gid, const QString& group_name) {
  if (gid_ == gid) return;
  gid_ = gid;
  setWindowTitle(QStringLiteral("群备忘录（%1）").arg(group_name));
  end_edit();
  stream_->clear();
  if (client_->is_logged_in()) refresh();
}

bool GroupMemoDialog::submit_entry(const QString& title, const QString& content) {
  if (title.trimmed().isEmpty() || content.trimmed().isEmpty()) return false;
  // R24-4 UI 边界：疑似密码只提示不拦（密码箱才是归宿；提示随刷新挂住）
  secret_hint_ = looks_like_secret(title) || looks_like_secret(content);
  if (secret_hint_) {
    set_status(QStringLiteral("疑似密码内容：密码请放群密码箱（全程密文＋查看留痕）；"
                              "备忘录为明文共享，慎放敏感值"),
               true);
  }
  client_->save_group_memo(gid_, title.trimmed(), content, editing_id_);
  return true;
}

// 手工路径疑似密码二次确认（程序化入口只挂提示不吊测试）
bool GroupMemoDialog::confirm_secret_memo(const QString& title,
                                          const QString& content) {
  if (!looks_like_secret(title) && !looks_like_secret(content)) return true;
  return QMessageBox::warning(this, QStringLiteral("疑似密码"),
                              QStringLiteral("内容疑似密码/凭据：密码请放群密码箱"
                                             "（密文留痕），备忘录是明文共享。"
                                             "仍要保存到备忘录？"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) == QMessageBox::Yes;
}

bool GroupMemoDialog::edit_selected() {
  const auto* it = stream_->currentItem();
  if (!it || it->flags() == Qt::NoItemFlags) return false;
  if (editing_id_ == it->data(kRoleId).toLongLong()) {
    end_edit(); // 再进一次＝取消编辑
    return true;
  }
  editing_id_ = it->data(kRoleId).toLongLong();
  set_status(QStringLiteral("正在编辑条目 #%1（submit_entry 保存；再点编辑＝取消）")
                 .arg(editing_id_),
             false);
  return true;
}

void GroupMemoDialog::refresh() {
  if (gid_ == 0 || !client_->is_logged_in()) return;
  client_->list_group_memos(gid_, search_->text().trimmed());
}

void GroupMemoDialog::open_history() {
  const auto* it = stream_->currentItem();
  if (!it || it->flags() == Qt::NoItemFlags) return;
  hist_memo_id_ = it->data(kRoleId).toLongLong();

  auto* dlg = new QDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(QStringLiteral("条目修订历史（#%1）").arg(hist_memo_id_));
  dlg->resize(560, 420);
  auto* layout = new QVBoxLayout(dlg);
  hist_list_ = new QListWidget(dlg);
  hist_list_->setWordWrap(true);
  layout->addWidget(hist_list_, 1);
  connect(dlg, &QObject::destroyed, this, [this] { hist_list_ = nullptr; });

  auto* btn_roll = new QPushButton(QStringLiteral("回滚到选中这笔"), dlg);
  btn_roll->setEnabled(false);
  layout->addWidget(btn_roll);
  auto* btn_close = new QPushButton(QStringLiteral("关闭"), dlg);
  layout->addWidget(btn_close);
  connect(btn_close, &QPushButton::clicked, dlg, &QDialog::close);
  connect(hist_list_, &QListWidget::itemSelectionChanged, dlg,
          [btn_roll, hist_list = hist_list_] {
            btn_roll->setEnabled(hist_list->currentItem() != nullptr);
          });
  connect(btn_roll, &QPushButton::clicked, this,
          [this] { rollback_selected_rev(); });
  // 修订史填充（连接挂 dlg 生命周期：窗销毁连接自断）
  connect(client_, &FilesClient::group_memo_history_fetched, dlg,
          [this](const QJsonArray& revisions) {
            if (!hist_list_) return;
            hist_list_->clear();
            for (const auto& v : revisions) {
              const QJsonObject o = v.toObject();
              const QString content =
                  o.value(QStringLiteral("content")).toString();
              auto* item = new QListWidgetItem(hist_list_);
              item->setText(QStringLiteral("%1　%2　《%3》\n%4")
                                .arg(fmt_time(static_cast<qint64>(
                                         o.value(QStringLiteral("ts_ms"))
                                             .toDouble())),
                                     o.value(QStringLiteral("editor"))
                                         .toString(),
                                     o.value(QStringLiteral("title"))
                                         .toString(),
                                     content));
              item->setData(
                  kRoleId,
                  static_cast<qint64>(
                      o.value(QStringLiteral("id")).toDouble()));
            }
            if (revisions.isEmpty()) {
              auto* item = new QListWidgetItem(hist_list_);
              item->setText(QStringLiteral("暂无修订记录"));
              item->setFlags(Qt::NoItemFlags);
            }
          });
  connect(client_, &FilesClient::group_memo_rolled_back, dlg, [dlg](qint64) {
    dlg->close();
  });
  client_->group_memo_history(hist_memo_id_);
  dlg->show(); // 非模态：信号驱动填充，不吊死交互
}

void GroupMemoDialog::rollback_selected_rev() {
  const auto* cur = hist_list_ ? hist_list_->currentItem() : nullptr;
  if (!cur || hist_memo_id_ == 0) return;
  client_->rollback_group_memo(hist_memo_id_,
                               cur->data(kRoleId).toLongLong());
  // 回滚成功信号（group_memo_rolled_back）关历史窗并刷新列表
}

QString GroupMemoDialog::status_text() const { return status_->text(); }

bool GroupMemoDialog::open_edit_checked() const {
  return open_edit_->isChecked();
}

int GroupMemoDialog::stream_count() const { return stream_->count(); }

int GroupMemoDialog::history_count() const {
  return hist_list_ ? hist_list_->count() : -1;
}

void GroupMemoDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : stream_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

void GroupMemoDialog::end_edit() {
  editing_id_ = 0;
}

} // namespace memex::client
