#include "file_assistant.hpp"

#include <QColor>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <engine/collab/files_client.hpp>

namespace memex::client {

namespace {
// 列表条目角色：type / id / 内容（文件名或备忘录正文）
constexpr int kRoleType = Qt::UserRole + 1;   // "memo" | "file"
constexpr int kRoleId = Qt::UserRole + 2;     // memo id / file id
constexpr int kRoleText = Qt::UserRole + 3;   // memo 正文 / 文件名

QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms)
      .toString(QStringLiteral("MM-dd HH:mm"));
}

QString fmt_size(qint64 n) {
  if (n < 1024) return QStringLiteral("%1 B").arg(n);
  if (n < 1024 * 1024) return QStringLiteral("%.1f KB").arg(n / 1024.0);
  return QStringLiteral("%.1f MB").arg(n / (1024.0 * 1024.0));
}
} // namespace

FileAssistantDialog::FileAssistantDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("文件助手"));
  resize(560, 520);
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
  connect(client_, &FilesClient::inbox_listed, this,
          [this](const QJsonArray& items) { populate(items); });
  connect(client_, &FilesClient::memo_created, this, [this](qint64) {
    end_edit();
    set_status(QStringLiteral("备忘录已存"), false);
    refresh();
  });
  connect(client_, &FilesClient::memo_updated, this, [this](qint64) {
    end_edit();
    set_status(QStringLiteral("备忘录已更新"), false);
    refresh();
  });
  connect(client_, &FilesClient::memo_deleted, this, [this](qint64) {
    set_status(QStringLiteral("备忘录已删除"), false);
    refresh();
  });
  connect(client_, &FilesClient::file_deleted, this, [this](qint64) {
    set_status(QStringLiteral("文件已删除"), false);
    refresh();
  });
  connect(client_, &FilesClient::upload_finished, this,
          [this](qint64, bool second) {
            set_status(second ? QStringLiteral("已入收件箱（同内容秒传，未重复占额）")
                              : QStringLiteral("已入收件箱"),
                       false);
            refresh();
          });
  connect(client_, &FilesClient::download_finished, this,
          [this](const QString& path) {
            set_status(QStringLiteral("已保存：%1").arg(path), false);
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败[%1]（%2）：%3")
                           .arg(op, status > 0 ? QString::number(status)
                                               : QStringLiteral("网络"),
                                error),
                       true);
          });
}

void FileAssistantDialog::build_ui() {
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

  // —— 收件箱混排流 ——
  stream_ = new QListWidget(this);
  stream_->setAlternatingRowColors(true);
  root->addWidget(stream_, 1);

  // —— 条目动作行（选中备忘录：编辑/删除；选中文件：下载/删除）——
  auto* act_row = new QHBoxLayout;
  btn_edit_ = new QPushButton(QStringLiteral("编辑备忘录"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除"), this);
  btn_download_ = new QPushButton(QStringLiteral("下载到本地…"), this);
  btn_edit_->setEnabled(false);
  btn_delete_->setEnabled(false);
  btn_download_->setEnabled(false);
  act_row->addWidget(btn_edit_);
  act_row->addWidget(btn_delete_);
  act_row->addWidget(btn_download_);
  act_row->addStretch(1);
  root->addLayout(act_row);

  // —— 输入行：备忘录正文 + 上传入口 ——
  auto* input_row = new QHBoxLayout;
  input_ = new QLineEdit(this);
  input_->setPlaceholderText(
      QStringLiteral("写点什么给自己（备忘录）…"));
  btn_submit_ = new QPushButton(QStringLiteral("存备忘录"), this);
  btn_upload_ = new QPushButton(QStringLiteral("发文件给自己…"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  input_row->addWidget(input_, 1);
  input_row->addWidget(btn_submit_);
  input_row->addWidget(btn_upload_);
  input_row->addWidget(btn_refresh_);
  root->addLayout(input_row);

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
  // 口令回车即连
  connect(password_, &QLineEdit::returnPressed, this, [this] {
    if (btn_connect_->isEnabled()) btn_connect_->click();
  });
  // 正文回车即存
  connect(input_, &QLineEdit::returnPressed, this, [this] { submit_memo(); });
  connect(btn_submit_, &QPushButton::clicked, this, [this] { submit_memo(); });
  connect(btn_upload_, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要发给自己的文件"));
    if (path.isEmpty()) return;
    client_->upload_inbox(path);
  });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });

  connect(stream_, &QListWidget::itemSelectionChanged, this, [this] {
    const auto* it = stream_->currentItem();
    const bool has = it != nullptr;
    const QString type = has ? it->data(kRoleType).toString() : QString();
    btn_edit_->setEnabled(has && type == QStringLiteral("memo"));
    btn_delete_->setEnabled(has);
    btn_download_->setEnabled(has && type == QStringLiteral("file"));
  });
  connect(btn_edit_, &QPushButton::clicked, this,
          [this] { edit_selected(); });
  connect(btn_delete_, &QPushButton::clicked, this, [this] {
    const auto* it = stream_->currentItem();
    if (!it) return;
    const QString type = it->data(kRoleType).toString();
    const qint64 id = it->data(kRoleId).toLongLong();
    if (QMessageBox::question(
            this, QStringLiteral("删除"),
            type == QStringLiteral("memo")
                ? QStringLiteral("删除这条备忘录？")
                : QStringLiteral("删除这个收件箱文件？")) !=
        QMessageBox::Yes) {
      return;
    }
    if (type == QStringLiteral("memo")) {
      client_->delete_memo(id);
    } else {
      client_->delete_file(id);
    }
  });
  connect(btn_download_, &QPushButton::clicked, this, [this] {
    const auto* it = stream_->currentItem();
    if (!it || it->data(kRoleType).toString() != QStringLiteral("file")) return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择保存目录"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (dir.isEmpty()) return;
    client_->download_file(it->data(kRoleId).toLongLong(),
                           it->data(kRoleText).toString(), dir);
  });
}

void FileAssistantDialog::connect_to(const QString& host, quint16 files_port,
                                     const QString& acc,
                                     const QString& pass) {
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

bool FileAssistantDialog::is_connected() const {
  return client_->is_logged_in();
}

bool FileAssistantDialog::submit_text(const QString& text) {
  input_->setText(text);
  return submit_memo();
}

bool FileAssistantDialog::edit_selected() {
  const auto* it = stream_->currentItem();
  if (!it || it->data(kRoleType).toString() != QStringLiteral("memo")) {
    return false;
  }
  if (editing_id_ == it->data(kRoleId).toLongLong()) {
    end_edit(); // 再进一次＝取消编辑
    return true;
  }
  editing_id_ = it->data(kRoleId).toLongLong();
  input_->setText(it->data(kRoleText).toString());
  input_->setFocus();
  btn_submit_->setText(QStringLiteral("保存修改"));
  btn_edit_->setText(QStringLiteral("取消编辑"));
  set_status(QStringLiteral("正在编辑备忘录 #%1（改完回车或点「保存修改」）")
                 .arg(editing_id_),
             false);
  return true;
}

bool FileAssistantDialog::submit_memo() {
  const QString text = input_->text().trimmed();
  if (text.isEmpty()) return false;
  if (editing_id_ > 0) {
    client_->update_memo(editing_id_, text);
  } else {
    client_->create_memo(text);
  }
  return true;
}

void FileAssistantDialog::refresh() { client_->list_inbox(); }

QString FileAssistantDialog::status_text() const { return status_->text(); }

int FileAssistantDialog::stream_count() const { return stream_->count(); }

void FileAssistantDialog::populate(const QJsonArray& items) {
  stream_->clear();
  for (const auto& v : items) {
    const QJsonObject o = v.toObject();
    const QString type = o.value(QStringLiteral("type")).toString();
    auto* it = new QListWidgetItem(stream_);
    if (type == QStringLiteral("memo")) {
      const qint64 id =
          static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
      const QString content =
          o.value(QStringLiteral("content")).toString();
      it->setText(QStringLiteral("备忘录 · %1\n%2")
                      .arg(fmt_time(
                          static_cast<qint64>(o.value(QStringLiteral(
                                                   "updated_ms"))
                                               .toDouble())),
                          content));
      it->setData(kRoleType, type);
      it->setData(kRoleId, id);
      it->setData(kRoleText, content);
    } else if (type == QStringLiteral("file")) {
      const qint64 id =
          static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
      it->setText(QStringLiteral("文件 · %1 · %2 · %3")
                      .arg(o.value(QStringLiteral("file_name")).toString(),
                           fmt_size(static_cast<qint64>(
                               o.value(QStringLiteral("file_size"))
                                   .toDouble())),
                           fmt_time(static_cast<qint64>(
                               o.value(QStringLiteral("upload_ts"))
                                   .toDouble()))));
      it->setData(kRoleType, type);
      it->setData(kRoleId, id);
      it->setData(kRoleText, o.value(QStringLiteral("file_name")).toString());
    }
  }
  if (items.isEmpty()) {
    auto* it = new QListWidgetItem(stream_);
    it->setText(QStringLiteral("（收件箱为空：写条备忘录，或把文件发给自己）"));
    it->setFlags(Qt::NoItemFlags);
  }
}

void FileAssistantDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（恢复用空参刷新路径重设）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : stream_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

void FileAssistantDialog::end_edit() {
  editing_id_ = 0;
  input_->clear();
  btn_submit_->setText(QStringLiteral("存备忘录"));
  btn_edit_->setText(QStringLiteral("编辑备忘录"));
}

} // namespace memex::client
