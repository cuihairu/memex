#include "group_server_dialog.hpp"

#include <QColor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
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

// 会话时长（收尾留痕）：秒取整
QString fmt_duration(qint64 duration_ms) {
  return QStringLiteral("%1 秒").arg(QString::number(duration_ms / 1000));
}

// 尽力而为唤起本地终端跑 ssh（桌面会话才试；offscreen/无显示环境不动）。
// 唤不起也无妨——命令框恒有命令可手动复制。
bool desktop_session_present() {
  return !qEnvironmentVariableIsEmpty("DISPLAY") ||
         !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

bool spawn_ssh_terminal(const QString& host) {
  const QString cmd = QStringLiteral("ssh %1").arg(host);
  for (const QString& term :
       {QStringLiteral("x-terminal-emulator"), QStringLiteral("xterm")}) {
    if (QProcess::startDetached(term, {QStringLiteral("-e"), cmd})) {
      return true;
    }
  }
  return false;
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
              it->setData(Qt::UserRole,
                          static_cast<qint64>(
                              o.value(QStringLiteral("id")).toDouble()));
              const bool cred =
                  o.value(QStringLiteral("cred")).toBool();
              it->setText(
                  QStringLiteral("%1 %2 · %3\nCPU %4 · 内存 %5 · 磁盘 %6 · "
                                 "负载 %7\n最近心跳 %8 · 登记人 %9 · 凭据 %10")
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
                               .toString())
                      .arg(cred ? QStringLiteral("已配置（由 %1 更新）")
                                      .arg(o.value(
                                               QStringLiteral(
                                                   "cred_updated_by"))
                                               .toString())
                                : QStringLiteral("未配置")));
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
  // —— SSH 会话（R26-3）：短票签发→即时兑现→本地唤起/命令框→收尾留痕 ——
  connect(client_, &FilesClient::session_requested, this,
          [this](qint64, qint64, const QString& ticket, qint64) {
            // 票明文只在此瞬时经手：立即兑现（60s TTL 一次性；不落任何框）
            client_->session_redeem(ticket);
            set_status(QStringLiteral("短票已签发，正在兑现…"), false);
          });
  connect(client_, &FilesClient::session_redeemed, this,
          [this](qint64 session_id, qint64, const QString& server_name,
                 const QString& host) {
            open_session_id_ = session_id;
            // 命令恒落框（唤不起终端也能手动复制）；桌面会话才尝试唤起
            cmd_->setText(QStringLiteral("ssh %1").arg(host));
            const bool spawned =
                desktop_session_present() && spawn_ssh_terminal(host);
            set_status(QStringLiteral("会话 #%1 已兑现：%2（%3）——%4")
                           .arg(QString::number(session_id), server_name,
                                host,
                                spawned ? QStringLiteral("本地终端已唤起")
                                        : QStringLiteral(
                                              "命令已给出（手动执行）")),
                       false);
            client_->session_list(gid_);
          });
  connect(client_, &FilesClient::session_closed, this,
          [this](qint64 session_id) {
            open_session_id_ = 0;
            set_status(QStringLiteral("会话 #%1 已收尾").arg(
                           QString::number(session_id)),
                       false);
            client_->session_list(gid_);
          });
  // 留痕列表（只更新列表不动状态行——防瞬态覆盖）
  connect(client_, &FilesClient::sessions_listed, this,
          [this](const QJsonArray& arr) {
            sessions_->clear();
            for (const auto& v : arr) {
              const QJsonObject o = v.toObject();
              const bool redeemed =
                  o.value(QStringLiteral("redeemed")).toBool();
              const bool open = o.value(QStringLiteral("open")).toBool();
              const QString state =
                  !open ? QStringLiteral("已收尾 · 时长 %1")
                              .arg(fmt_duration(static_cast<qint64>(
                                  o.value(QStringLiteral("duration_ms"))
                                      .toDouble())))
                        : (redeemed ? QStringLiteral("进行中")
                                    : QStringLiteral("未用短票"));
              const qint64 opened = static_cast<qint64>(
                  o.value(QStringLiteral("opened_ms")).toDouble());
              auto* it = new QListWidgetItem(sessions_);
              it->setText(
                  QStringLiteral("#%1 %2 · %3 · %4 · %5 · %6 · %7")
                      .arg(QString::number(
                               static_cast<qint64>(
                                   o.value(QStringLiteral("id")).toDouble()),
                               10),
                           o.value(QStringLiteral("server_name")).toString(),
                           o.value(QStringLiteral("host")).toString(),
                           o.value(QStringLiteral("protocol")).toString(),
                           o.value(QStringLiteral("actor")).toString(),
                           opened == 0 ? QStringLiteral("—") : fmt_time(opened),
                           state));
            }
            if (arr.isEmpty()) {
              auto* it = new QListWidgetItem(sessions_);
              it->setText(QStringLiteral(
                  "（本群暂无接入留痕——选中服务器点「发起 SSH 会话」）"));
              it->setFlags(Qt::NoItemFlags);
            }
          });
  // 凭据落点（明文永不出现在任何框/日志——只刷新掩码态）
  connect(client_, &FilesClient::server_cred_saved, this,
          [this](qint64, qint64) {
            set_status(QStringLiteral("凭据已存（服务端加密；客户端零凭据）"),
                       false);
            srv_cred_->clear();
            refresh();
          });
  connect(client_, &FilesClient::server_cred_removed, this,
          [this](qint64, qint64) {
            set_status(QStringLiteral("凭据已删"), false);
            refresh();
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

  // —— SSH 会话区（R26-3：选中服务器→发起；短票不出框、命令框可复制）——
  auto* session_row = new QHBoxLayout;
  btn_session_ = new QPushButton(QStringLiteral("发起 SSH 会话"), this);
  btn_close_ = new QPushButton(QStringLiteral("关闭会话"), this);
  session_row->addWidget(btn_session_);
  session_row->addWidget(btn_close_);
  session_row->addStretch(1);
  root->addLayout(session_row);

  cmd_ = new QLineEdit(this);
  cmd_->setReadOnly(true);
  cmd_->setPlaceholderText(QStringLiteral(
      "本地命令（会话兑现后给出；唤不起终端时手动执行）"));
  root->addWidget(cmd_);

  sessions_ = new QListWidget(this);
  sessions_->setAlternatingRowColors(true);
  sessions_->setWordWrap(true);
  root->addWidget(sessions_, 1);

  // —— 服务器凭据行（R26-4：仅群主/管理员；作用于选中行；密码态输入）——
  auto* cred_row = new QHBoxLayout;
  srv_cred_ = new QLineEdit(this);
  srv_cred_->setEchoMode(QLineEdit::Password);
  srv_cred_->setPlaceholderText(QStringLiteral(
      "所选服务器凭据（口令/密钥——只送服务端加密，客户端不留存）"));
  btn_cred_set_ = new QPushButton(QStringLiteral("存凭据"), this);
  btn_cred_del_ = new QPushButton(QStringLiteral("删凭据"), this);
  cred_row->addWidget(srv_cred_, 1);
  cred_row->addWidget(btn_cred_set_);
  cred_row->addWidget(btn_cred_del_);
  root->addLayout(cred_row);

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
  connect(btn_session_, &QPushButton::clicked, this,
          [this] { request_session(); });
  connect(btn_close_, &QPushButton::clicked, this,
          [this] { close_session(); });
  connect(btn_cred_set_, &QPushButton::clicked, this,
          [this] { set_server_credential(srv_cred_->text()); });
  connect(btn_cred_del_, &QPushButton::clicked, this,
          [this] { delete_server_credential(); });
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
  sessions_->clear();
  cmd_->clear();
  open_session_id_ = 0;
  if (gid_ > 0 && client_->is_logged_in()) refresh();
}

void GroupServerDialog::refresh() {
  if (gid_ == 0 || !client_->is_logged_in()) return;
  client_->server_list(gid_);
  client_->session_list(gid_);
}

bool GroupServerDialog::request_session() {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  const auto* it = servers_->currentItem();
  const qint64 sid = it ? it->data(Qt::UserRole).toLongLong() : 0;
  if (sid <= 0) {
    set_status(QStringLiteral("先在列表中选中一台服务器"), true);
    return false;
  }
  client_->session_request(gid_, sid);
  return true;
}

bool GroupServerDialog::close_session() {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  if (open_session_id_ == 0) {
    set_status(QStringLiteral("没有进行中的会话"), true);
    return false;
  }
  client_->session_close(gid_, open_session_id_);
  return true;
}

// 选中行的服务器 id（0=无有效选中）
namespace {
qint64 selected_server_id(const QListWidget* list) {
  const auto* it = list->currentItem();
  return it ? it->data(Qt::UserRole).toLongLong() : 0;
}
} // namespace

bool GroupServerDialog::set_server_credential(const QString& value) {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  const qint64 sid = selected_server_id(servers_);
  if (sid <= 0) {
    set_status(QStringLiteral("先在列表中选中一台服务器"), true);
    return false;
  }
  if (value.isEmpty()) {
    set_status(QStringLiteral("凭据不能为空"), true);
    return false;
  }
  client_->server_cred_set(gid_, sid, value);
  return true;
}

bool GroupServerDialog::delete_server_credential() {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  const qint64 sid = selected_server_id(servers_);
  if (sid <= 0) {
    set_status(QStringLiteral("先在列表中选中一台服务器"), true);
    return false;
  }
  client_->server_cred_delete(gid_, sid);
  return true;
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

int GroupServerDialog::session_count() const { return sessions_->count(); }

QString GroupServerDialog::session_command_text() const {
  return cmd_->text();
}

void GroupServerDialog::set_status(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(
      error ? QStringLiteral("color: #c62828;")
            : QStringLiteral(""));
}

} // namespace memex::client
