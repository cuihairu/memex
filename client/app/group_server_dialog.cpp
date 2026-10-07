#include "group_server_dialog.hpp"

#include <QColor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSettings>
#include <QPushButton>
#include <QVBoxLayout>

#include <engine/collab/files_client.hpp>

namespace memex::client {

namespace {

QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms)
      .toString(QStringLiteral("MM-dd HH:mm"));
}

// 指标格式化：mb=MB 数值；total<=0 ＝尚未打点（未打过心跳）
QString fmt_pair(double used, double total) {
  if (total <= 0) return QStringLiteral("尚未上报");
  return QStringLiteral("%1/%2 MB")
      .arg(QString::number(used, 'f', 0), QString::number(total, 'f', 0));
}

} // namespace

GroupServerDialog::GroupServerDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群服务器"));
  resize(640, 560);
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
  // 列表：●绿=agent 在线（90s 新鲜度窗）、●红=失联或从未打点；详情=
  // 最近一拍 CPU/内存/磁盘/负载＋谁登记＋最近心跳时刻
  connect(client_, &FilesClient::servers_listed, this,
          [this](const QJsonArray& arr) {
            servers_->clear();
            for (const auto& v : arr) {
              const QJsonObject o = v.toObject();
              const bool online = o.value(QStringLiteral("online")).toBool();
              const double cpu =
                  o.value(QStringLiteral("cpu_percent")).toDouble();
              const qint64 seen = static_cast<qint64>(
                  o.value(QStringLiteral("last_seen_ms")).toDouble());
              auto* it = new QListWidgetItem(servers_);
              it->setText(
                  QStringLiteral("%1 %2 · %3\nCPU %4 · 内存 %5 · 磁盘 %6 · "
                                 "负载 %7\n最近心跳 %8 · 登记人 %9")
                      .arg(QStringLiteral("●"),
                           o.value(QStringLiteral("name")).toString(),
                           o.value(QStringLiteral("host")).toString(),
                           cpu < 0 ? QStringLiteral("尚未上报")
                                   : QString::number(cpu, 'f', 1) +
                                         QStringLiteral("%"),
                           fmt_pair(
                               o.value(QStringLiteral("mem_used_mb"))
                                   .toDouble(),
                               o.value(QStringLiteral("mem_total_mb"))
                                   .toDouble()),
                           fmt_pair(
                               o.value(QStringLiteral("disk_used_mb"))
                                   .toDouble(),
                               o.value(QStringLiteral("disk_total_mb"))
                                   .toDouble()),
                           QString::number(
                               o.value(QStringLiteral("load1")).toDouble(),
                               'f', 2),
                           seen == 0 ? QStringLiteral("从未")
                                     : fmt_time(seen),
                           o.value(QStringLiteral("enrolled_by"))
                               .toString()));
              it->setForeground(online ? QColor(Qt::darkGreen)
                                       : QColor(Qt::red));
            }
            if (arr.isEmpty()) {
              auto* it = new QListWidgetItem(servers_);
              it->setText(QStringLiteral(
                  "（本群暂无登记服务器——管理员可登记，服务器上跑"
                  " memex_agent 凭令牌心跳）"));
              it->setFlags(Qt::NoItemFlags);
            }
            set_status(QStringLiteral("共 %1 台服务器").arg(arr.size()),
                       false);
          });
  connect(client_, &FilesClient::server_enrolled, this,
          [this](qint64, qint64 id, const QString& token) {
            token_->setText(token);
            set_status(QStringLiteral("已登记 #%1（令牌只显示这一次——"
                                      "重登记会作废旧令牌）")
                           .arg(QString::number(id)),
                       false);
            client_->server_list(gid_);
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

void GroupServerDialog::build_ui() {
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

  // —— 服务器列表（红绿灯＋状态详情）——
  servers_ = new QListWidget(this);
  servers_->setAlternatingRowColors(true);
  servers_->setWordWrap(true);
  root->addWidget(servers_, 2);

  // —— 登记行（群主/管理员；重登记=轮换令牌须确认）——
  auto* enroll_row = new QHBoxLayout;
  srv_name_ = new QLineEdit(this);
  srv_name_->setPlaceholderText(QStringLiteral("服务器名（如 web-1）"));
  srv_name_->setMaximumWidth(180);
  srv_host_ = new QLineEdit(this);
  srv_host_->setPlaceholderText(QStringLiteral("地址（如 10.0.0.9）"));
  btn_enroll_ = new QPushButton(QStringLiteral("登记服务器"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  enroll_row->addWidget(srv_name_);
  enroll_row->addWidget(srv_host_, 1);
  enroll_row->addWidget(btn_enroll_);
  enroll_row->addWidget(btn_refresh_);
  root->addLayout(enroll_row);

  // —— 注册令牌（登记后一次性显示；喂给 agent --token）——
  token_ = new QLineEdit(this);
  token_->setReadOnly(true);
  token_->setPlaceholderText(
      QStringLiteral("注册令牌（登记后生成一次；重登记作废旧令牌）"));
  root->addWidget(token_);

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
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  connect(btn_enroll_, &QPushButton::clicked, this, [this] {
    if (srv_name_->text().trimmed().isEmpty() ||
        srv_host_->text().trimmed().isEmpty()) {
      set_status(QStringLiteral("登记须填服务器名与地址"), true);
      return;
    }
    // 轮换语义须确认：同登记作废旧令牌（在线的旧 agent 立即失联）
    if (QMessageBox::question(
            this, QStringLiteral("登记服务器"),
            QStringLiteral("登记「%1」？（同名重登记会作废旧令牌——"
                           "旧 agent 将失联）")
                .arg(srv_name_->text().trimmed())) != QMessageBox::Yes) {
      return;
    }
    enroll_server(srv_name_->text().trimmed(), srv_host_->text().trimmed());
    srv_name_->clear();
    srv_host_->clear();
  });
}

void GroupServerDialog::connect_to(const QString& host, quint16 files_port,
                                   const QString& account,
                                   const QString& password) {
  client_->login(host, files_port, account, password);
}

bool GroupServerDialog::is_connected() const { return client_->is_logged_in(); }

void GroupServerDialog::set_group(quint64 gid, const QString& group_name) {
  gid_ = gid;
  setWindowTitle(QStringLiteral("群服务器 — %1").arg(group_name));
  servers_->clear();
  token_->clear();
  if (gid_ > 0 && client_->is_logged_in()) refresh();
}

void GroupServerDialog::refresh() {
  if (gid_ == 0 || !client_->is_logged_in()) return;
  client_->server_list(gid_);
}

bool GroupServerDialog::enroll_server(const QString& name,
                                      const QString& host) {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  if (name.isEmpty() || host.isEmpty()) {
    set_status(QStringLiteral("登记须填服务器名与地址"), true);
    return false;
  }
  client_->server_enroll(gid_, name, host);
  return true;
}

QString GroupServerDialog::status_text() const { return status_->text(); }

int GroupServerDialog::server_count() const { return servers_->count(); }

QString GroupServerDialog::enroll_token_text() const { return token_->text(); }

void GroupServerDialog::set_status(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(
      error ? QStringLiteral("color: #c62828;")
            : QStringLiteral(""));
}

} // namespace memex::client
