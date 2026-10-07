#include "group_ci_dialog.hpp"

#include <QColor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonDocument>
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
// 列表条目角色：流水线名（runs 面同构取值）
constexpr int kRoleName = Qt::UserRole + 2;

QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms)
      .toString(QStringLiteral("MM-dd HH:mm"));
}

QString status_label(const QString& status) {
  return status == QStringLiteral("success") ? QStringLiteral("成功")
                                             : QStringLiteral("失败");
}
} // namespace

GroupCiDialog::GroupCiDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群 CI/CD"));
  resize(620, 600);
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
  // 红绿灯列表：●绿=最近成功、●红=最近失败、●灰=未跑过
  connect(client_, &FilesClient::ci_listed, this,
          [this](const QJsonArray& pipelines) {
            pipelines_->clear();
            for (const auto& v : pipelines) {
              const QJsonObject o = v.toObject();
              const QString name =
                  o.value(QStringLiteral("name")).toString();
              auto* it = new QListWidgetItem(pipelines_);
              if (o.contains(QStringLiteral("last_status"))) {
                const QString st =
                    o.value(QStringLiteral("last_status")).toString();
                it->setText(
                    QStringLiteral("%1 %2 · %3\n最近：%4 · 触发 %5 · %6")
                        .arg(QStringLiteral("●"), name,
                             o.value(QStringLiteral("description"))
                                 .toString(),
                             status_label(st),
                             o.value(QStringLiteral("last_actor")).toString(),
                             fmt_time(static_cast<qint64>(
                                 o.value(QStringLiteral("last_ts_ms"))
                                     .toDouble()))));
                it->setForeground(
                    st == QStringLiteral("success") ? QColor(Qt::darkGreen)
                                                    : QColor(Qt::red));
              } else {
                it->setText(QStringLiteral("● %1 · %2\n最近：未跑过")
                                .arg(name,
                                     o.value(QStringLiteral("description"))
                                         .toString()));
                it->setForeground(QColor(Qt::gray));
              }
              it->setData(kRoleName, name);
            }
            if (pipelines.isEmpty()) {
              auto* it = new QListWidgetItem(pipelines_);
              it->setText(QStringLiteral("（本群暂无流水线——管理员可新建）"));
              it->setFlags(Qt::NoItemFlags);
            }
            set_status(QStringLiteral("共 %1 条流水线").arg(pipelines.size()),
                       false);
          });
  connect(client_, &FilesClient::ci_runs_listed, this,
          [this](const QJsonArray& runs) {
            runs_->clear();
            for (const auto& v : runs) {
              const QJsonObject o = v.toObject();
              auto* it = new QListWidgetItem(runs_);
              it->setText(QStringLiteral("#%1 %2 · %3 · 触发 %4 · %5")
                              .arg(QString::number(static_cast<qint64>(
                                       o.value(QStringLiteral("id"))
                                           .toDouble())),
                                   o.value(QStringLiteral("pipeline"))
                                       .toString(),
                                   status_label(o.value(QStringLiteral("status"))
                                                    .toString()),
                                   o.value(QStringLiteral("actor")).toString(),
                                   fmt_time(static_cast<qint64>(
                                       o.value(QStringLiteral("ts_ms"))
                                           .toDouble()))));
              it->setForeground(o.value(QStringLiteral("status"))
                                        .toString() ==
                                    QStringLiteral("success")
                                    ? QColor(Qt::darkGreen)
                                    : QColor(Qt::red));
            }
            if (runs.isEmpty()) {
              auto* it = new QListWidgetItem(runs_);
              it->setText(QStringLiteral("（无运行记录）"));
              it->setFlags(Qt::NoItemFlags);
            }
          });
  connect(client_, &FilesClient::ci_triggered, this,
          [this](qint64 run_id, const QString& status) {
            set_status(QStringLiteral("已触发（run #%1，结果：%2）——"
                                      "卡片已回群")
                           .arg(QString::number(run_id),
                                status_label(status)),
                       false);
            refresh();
          });
  connect(client_, &FilesClient::ci_pipeline_set, this, [this] {
    set_status(QStringLiteral("流水线定义已更新"), false);
    refresh();
  });
  connect(client_, &FilesClient::tool_actions_set, this, [this] {
    set_status(QStringLiteral("已开放成员触发（ci/trigger 进白名单）"), false);
  });
  // R25-4 凭据面：保存/删除后重拉掩码状态（只动 cred_state_ 标签＝持久
  // 面；回包里没有 value，界面永远显示不出凭据值）
  connect(client_, &FilesClient::tool_cred_saved, this,
          [this](qint64, const QString&) {
            set_status(QStringLiteral("凭据已更新（服务端加密保存，客户端不留存）"),
                       false);
            client_->tool_cred_list(gid_);
          });
  connect(client_, &FilesClient::tool_cred_removed, this,
          [this](qint64, const QString&) {
            set_status(QStringLiteral("凭据已删除"), false);
            client_->tool_cred_list(gid_);
          });
  connect(client_, &FilesClient::tool_credentials_listed, this,
          [this](const QJsonArray& creds) {
            for (const auto& v : creds) {
              const QJsonObject o = v.toObject();
              if (o.value(QStringLiteral("tool")).toString() !=
                  QStringLiteral("ci")) {
                continue;
              }
              cred_state_->setText(
                  QStringLiteral("ci 凭据：已配置（由 %1 于 %2 更新）")
                      .arg(o.value(QStringLiteral("updated_by")).toString(),
                           fmt_time(static_cast<qint64>(
                               o.value(QStringLiteral("updated_ms"))
                                   .toDouble()))));
              return;
            }
            cred_state_->setText(QStringLiteral("ci 凭据：未配置"));
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            // 凭据状态静默拉取：非管理员 403 是常态（成员看不到凭据），
            // 不刷错误状态打断成员的红绿灯走查
            if (op == QStringLiteral("group-credential.list")) return;
            set_status(QStringLiteral("操作失败[%1]（%2）：%3")
                           .arg(op, status > 0 ? QString::number(status)
                                               : QStringLiteral("网络"),
                                error),
                       true);
          });
}

void GroupCiDialog::build_ui() {
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

  // —— 流水线红绿灯列表 ——
  pipelines_ = new QListWidget(this);
  pipelines_->setAlternatingRowColors(true);
  pipelines_->setWordWrap(true);
  root->addWidget(pipelines_, 2);

  // —— 触发行（参数 JSON 可选；触发可回溯到人＝服务端留痕）——
  auto* trig_row = new QHBoxLayout;
  params_ = new QLineEdit(this);
  params_->setPlaceholderText(
      QStringLiteral("参数 JSON（可选；{\"fail\":true} 演练失败——stub 执行器）"));
  btn_trigger_ = new QPushButton(QStringLiteral("触发构建"), this);
  btn_trigger_->setEnabled(false);
  trig_row->addWidget(params_, 1);
  trig_row->addWidget(btn_trigger_);
  root->addLayout(trig_row);

  // —— run 历史（谁触发可回溯）——
  runs_ = new QListWidget(this);
  runs_->setAlternatingRowColors(true);
  runs_->setWordWrap(true);
  root->addWidget(runs_, 2);

  // —— 管理行（增删/开白名单=群主/管理员，服务端裁决；越权由状态行明示）——
  auto* mgr_row = new QHBoxLayout;
  pl_name_ = new QLineEdit(this);
  pl_name_->setPlaceholderText(QStringLiteral("流水线名"));
  pl_name_->setMaximumWidth(160);
  pl_desc_ = new QLineEdit(this);
  pl_desc_->setPlaceholderText(QStringLiteral("描述（可选）"));
  btn_add_ = new QPushButton(QStringLiteral("新建/更新"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除"), this);
  btn_open_ = new QPushButton(QStringLiteral("开放成员触发"), this);
  btn_open_->setToolTip(
      QStringLiteral("把 ci/trigger 写进本群工具白名单（默认关闭；"
                     "开放后全体成员可触发——入群即授权"));
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  mgr_row->addWidget(pl_name_);
  mgr_row->addWidget(pl_desc_, 1);
  mgr_row->addWidget(btn_add_);
  mgr_row->addWidget(btn_delete_);
  mgr_row->addWidget(btn_open_);
  mgr_row->addWidget(btn_refresh_);
  root->addLayout(mgr_row);

  // —— 凭据行（R25-4：值只此一次发往服务端；界面永只显示掩码状态）——
  auto* cred_row = new QHBoxLayout;
  cred_value_ = new QLineEdit(this);
  cred_value_->setEchoMode(QLineEdit::Password);
  cred_value_->setPlaceholderText(
      QStringLiteral("ci 工具凭据值（仅设置时发送一次；界面不回显）"));
  cred_state_ = new QLabel(QStringLiteral("ci 凭据：未配置"), this);
  btn_cred_set_ = new QPushButton(QStringLiteral("设置凭据"), this);
  btn_cred_del_ = new QPushButton(QStringLiteral("删除凭据"), this);
  btn_cred_set_->setToolTip(QStringLiteral(
      "把凭据加密保存到服务端（仅群主/管理员；代理调用时服务端内存内"
      "解密，客户端永不取回）"));
  btn_cred_del_->setToolTip(QStringLiteral("删除本群 ci 工具凭据"));
  cred_row->addWidget(cred_value_, 1);
  cred_row->addWidget(cred_state_);
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
  connect(pipelines_, &QListWidget::itemSelectionChanged, this, [this] {
    btn_trigger_->setEnabled(pipelines_->currentItem() != nullptr &&
                             pipelines_->currentItem()->flags() &
                                 Qt::ItemIsEnabled);
  });
  connect(btn_trigger_, &QPushButton::clicked, this,
          [this] { trigger_selected(); });
  connect(btn_add_, &QPushButton::clicked, this, [this] {
    submit_pipeline(pl_name_->text().trimmed(), pl_desc_->text().trimmed());
  });
  connect(btn_open_, &QPushButton::clicked, this,
          [this] { open_trigger_whitelist(); });
  connect(btn_delete_, &QPushButton::clicked, this, [this] {
    const QString name = pl_name_->text().trimmed();
    if (name.isEmpty()) {
      set_status(QStringLiteral("删除须先填流水线名"), true);
      return;
    }
    if (QMessageBox::question(this, QStringLiteral("删除流水线"),
                              QStringLiteral("删除流水线「%1」？（run 历史"
                                             "保留）")
                                  .arg(name)) != QMessageBox::Yes) {
      return;
    }
    submit_pipeline(name, QString(), true);
  });
  // 破坏性动作二次确认只挂按钮路径（程序化 set_credential/delete_credential
  // 不弹框，测试直接走）
  connect(btn_cred_set_, &QPushButton::clicked, this, [this] {
    if (cred_value_->text().isEmpty()) {
      set_status(QStringLiteral("先填凭据值再设置"), true);
      return;
    }
    if (QMessageBox::question(this, QStringLiteral("设置凭据"),
                              QStringLiteral("更新本群 ci 工具凭据？"
                                             "（服务端加密保存，覆盖旧值）")) !=
        QMessageBox::Yes) {
      return;
    }
    set_credential(cred_value_->text());
    cred_value_->clear();
  });
  connect(btn_cred_del_, &QPushButton::clicked, this, [this] {
    if (QMessageBox::question(this, QStringLiteral("删除凭据"),
                              QStringLiteral("删除本群 ci 工具凭据？"
                                             "（代理调用将无法携带认证）")) !=
        QMessageBox::Yes) {
      return;
    }
    delete_credential();
  });
}

void GroupCiDialog::connect_to(const QString& host, quint16 files_port,
                               const QString& account,
                               const QString& password) {
  client_->login(host, files_port, account, password);
}

bool GroupCiDialog::is_connected() const { return client_->is_logged_in(); }

void GroupCiDialog::set_group(quint64 gid, const QString& group_name) {
  gid_ = gid;
  setWindowTitle(QStringLiteral("群 CI/CD — %1").arg(group_name));
  pipelines_->clear();
  runs_->clear();
  if (gid_ > 0 && client_->is_logged_in()) refresh();
}

void GroupCiDialog::refresh() {
  if (gid_ == 0 || !client_->is_logged_in()) return;
  client_->ci_list(gid_);
  const auto* it = pipelines_->currentItem();
  client_->ci_runs(gid_, it ? it->data(kRoleName).toString() : QString());
  // 掩码状态静默拉取（成员 403 被 request_failed 过滤，不打断红绿灯面）
  client_->tool_cred_list(gid_);
}

bool GroupCiDialog::trigger_selected() {
  const auto* it = pipelines_->currentItem();
  if (!it) {
    set_status(QStringLiteral("先选中要触发的流水线"), true);
    return false;
  }
  QJsonObject params;
  const QString raw = params_->text().trimmed();
  if (!raw.isEmpty()) {
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject()) {
      set_status(QStringLiteral("参数须为 JSON 对象"), true);
      return false;
    }
    params = doc.object();
  }
  client_->ci_trigger(gid_, it->data(kRoleName).toString(), params);
  return true;
}

bool GroupCiDialog::submit_pipeline(const QString& name,
                                    const QString& description, bool remove) {
  if (name.isEmpty()) {
    set_status(QStringLiteral("流水线名不能为空"), true);
    return false;
  }
  client_->ci_set_pipeline(gid_, name, description, remove);
  return true;
}

bool GroupCiDialog::open_trigger_whitelist() {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  client_->set_tool_actions(gid_, QStringLiteral("ci"),
                            {QStringLiteral("trigger")});
  return true;
}

bool GroupCiDialog::set_credential(const QString& value) {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  if (value.isEmpty()) {
    set_status(QStringLiteral("凭据值不能为空"), true);
    return false;
  }
  client_->tool_cred_set(gid_, QStringLiteral("ci"), value);
  return true;
}

bool GroupCiDialog::delete_credential() {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  client_->tool_cred_delete(gid_, QStringLiteral("ci"));
  return true;
}

QString GroupCiDialog::status_text() const { return status_->text(); }

int GroupCiDialog::pipeline_count() const { return pipelines_->count(); }

int GroupCiDialog::runs_count() const { return runs_->count(); }

QString GroupCiDialog::credential_state_text() const {
  return cred_state_->text();
}

void GroupCiDialog::set_status(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(
      error ? QStringLiteral("color: #c62828;")
            : QStringLiteral(""));
}

} // namespace memex::client
