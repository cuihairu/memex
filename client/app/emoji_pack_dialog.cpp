// 表情包云素材管理（需求批②）——实现说明见头文件。
#include "emoji_pack_dialog.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <engine/collab/files_client.hpp>

namespace memex::client {

namespace {
constexpr int kRoleId = Qt::UserRole + 1;
constexpr int kRoleName = Qt::UserRole + 2;
constexpr int kRoleSize = Qt::UserRole + 3;
} // namespace

EmojiPackDialog::EmojiPackDialog(QWidget* parent, const QString& local_dir)
    : QDialog(parent), local_dir_(local_dir) {
  setWindowTitle(QStringLiteral("云表情包（需求批②）"));
  setMinimumSize(520, 420);
  client_ = new FilesClient(this);
  connect(client_, &FilesClient::logged_in, this, [this] {
    btn_connect_->setEnabled(false);
    set_status(QStringLiteral("已连接（%1）——素材随账号走，换机即得")
                   .arg(client_->account()));
    refresh();
  });
  connect(client_, &FilesClient::login_failed, this, [this](const QString& r) {
    btn_connect_->setEnabled(true);
    set_status(QStringLiteral("连接失败：%1").arg(r), true);
  });
  connect(client_, &FilesClient::emoji_uploaded, this,
          [this](qint64) { refresh(); });
  connect(client_, &FilesClient::emoji_listed, this,
          [this](const QJsonArray& assets) { populate(assets); });
  connect(client_, &FilesClient::emoji_deleted, this,
          [this](qint64) { refresh(); });
  connect(client_, &FilesClient::emoji_downloaded, this,
          [this](const QString& path) {
            set_status(QStringLiteral("已下载到本地：%1（表情面板可见）")
                           .arg(QFileInfo(path).fileName()));
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败[%1]（%2）：%3")
                           .arg(op,
                                status > 0
                                    ? QString::number(status)
                                    : QStringLiteral("网络"),
                                error),
                       true);
          });

  build_ui();
}

void EmojiPackDialog::build_ui() {
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

  // —— 素材清单 ——
  assets_ = new QListWidget(this);
  assets_->setAlternatingRowColors(true);
  root->addWidget(assets_, 1);

  // —— 动作行 ——
  auto* act_row = new QHBoxLayout;
  btn_upload_ = new QPushButton(QStringLiteral("上传表情（GIF/PNG/JPG）…"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除选中"), this);
  btn_download_ = new QPushButton(QStringLiteral("下载到本地表情目录"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  btn_delete_->setEnabled(false);
  btn_download_->setEnabled(false);
  act_row->addWidget(btn_upload_);
  act_row->addWidget(btn_delete_);
  act_row->addWidget(btn_download_);
  act_row->addStretch(1);
  act_row->addWidget(btn_refresh_);
  root->addLayout(act_row);

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
  connect(btn_upload_, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择表情图片"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif)"));
    if (path.isEmpty()) return;
    upload_from(path);
  });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  connect(assets_, &QListWidget::itemSelectionChanged, this, [this] {
    const bool has = assets_->currentItem() != nullptr;
    btn_delete_->setEnabled(has);
    btn_download_->setEnabled(has);
  });
  connect(btn_delete_, &QPushButton::clicked, this, [this] {
    auto* it = assets_->currentItem();
    if (!it) return;
    if (QMessageBox::question(
            this, QStringLiteral("删除"),
            QStringLiteral("删除表情「%1」？（仅本人素材，不影响已发送）")
                .arg(it->data(kRoleName).toString())) != QMessageBox::Yes) {
      return;
    }
    client_->emoji_delete(it->data(kRoleId).toLongLong());
  });
  connect(btn_download_, &QPushButton::clicked, this,
          [this] { download_selected(); });
}

void EmojiPackDialog::connect_to(const QString& host, quint16 files_port,
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
  set_status(QStringLiteral("连接中…"));
  client_->login(host, files_port, acc, pass);
}

bool EmojiPackDialog::is_connected() const { return client_->is_logged_in(); }

bool EmojiPackDialog::upload_from(const QString& file_path,
                                  const QString& name) {
  if (!client_->is_logged_in()) {
    set_status(QStringLiteral("未连接（先连接文件面再上传）"), true);
    return false;
  }
  const QFileInfo fi(file_path);
  if (!fi.isFile()) {
    set_status(QStringLiteral("文件不存在：%1").arg(file_path), true);
    return false;
  }
  const QString n = name.isEmpty() ? fi.fileName() : name;
  set_status(QStringLiteral("上传中：%1").arg(n));
  client_->emoji_upload(file_path, n);
  return true;
}

void EmojiPackDialog::refresh() { client_->emoji_list(); }

void EmojiPackDialog::download_selected() {
  auto* it = assets_->currentItem();
  if (!it) return;
  if (local_dir_.isEmpty()) {
    set_status(QStringLiteral("未配置本地表情目录（经主窗打开）"), true);
    return;
  }
  client_->emoji_download(it->data(kRoleId).toLongLong(),
                          it->data(kRoleName).toString(), local_dir_);
}

QString EmojiPackDialog::status_text() const { return status_->text(); }

int EmojiPackDialog::asset_count() const { return assets_->count(); }

QListWidget* EmojiPackDialog::list() { return assets_; }

void EmojiPackDialog::populate(const QJsonArray& assets) {
  assets_->clear();
  for (const auto& v : assets) {
    const QJsonObject o = v.toObject();
    const qint64 id = static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
    const QString name = o.value(QStringLiteral("name")).toString();
    const double size = o.value(QStringLiteral("size")).toDouble();
    auto* it = new QListWidgetItem(
        QStringLiteral("%1（%2 KB）").arg(
            name, QString::number(size / 1024.0, 'f', 1)),
        assets_);
    it->setData(kRoleId, id);
    it->setData(kRoleName, name);
    it->setData(kRoleSize,
                static_cast<qlonglong>(size));
  }
  set_status(assets.isEmpty()
                 ? QStringLiteral("清单为空（上传 GIF/PNG/JPG 收进个人素材库）")
                 : QStringLiteral("共 %1 个表情").arg(assets.size()));
}

void EmojiPackDialog::set_status(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(error ? QStringLiteral("color:#c0392b;")
                               : QString());
}

} // namespace memex::client
