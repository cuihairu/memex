#include "group_pack_dialog.hpp"
#include "collab_defaults.hpp"
#include "theme.hpp"

#include <QColor>
#include <QColor>
#include <QDateTime>
#include <QFileDialog>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <engine/collab/files_client.hpp>

namespace memex::client {

namespace {
constexpr int kRoleName = Qt::UserRole + 2;
constexpr int kRoleVersion = Qt::UserRole + 3;

QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms)
      .toString(QStringLiteral("MM-dd HH:mm"));
}
} // namespace

GroupPackDialog::GroupPackDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群打包"));
  resize(1000, 640);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接：%1@%2:%3")
                   .arg(client_->account(), host_->text(), port_->text()),
               false);
    conn_badge_->setText(QStringLiteral("已连接"));
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
  connect(client_, &FilesClient::pack_listed, this,
          [this](const QJsonArray& artifacts) {
            artifacts_->clear();
            for (const auto& v : artifacts) {
              const QJsonObject o = v.toObject();
              const QString name =
                  o.value(QStringLiteral("name")).toString();
              const QString version =
                  o.value(QStringLiteral("version")).toString();
              auto* it = new QListWidgetItem(artifacts_);
              it->setText(
                  QStringLiteral("%1 %2 · %3\n出产物 %4 · %5")
                      .arg(name, version,
                           o.value(QStringLiteral("note")).toString(),
                           o.value(QStringLiteral("created_by")).toString(),
                           fmt_time(static_cast<qint64>(
                               o.value(QStringLiteral("created_ms"))
                                   .toDouble()))));
              it->setData(kRoleName, name);
              it->setData(kRoleVersion, version);
            }
            if (artifacts.isEmpty()) {
              auto* it = new QListWidgetItem(artifacts_);
              it->setText(QStringLiteral("（暂无产物——打包须白名单开放）"));
              it->setFlags(Qt::NoItemFlags);
            }
            set_status(QStringLiteral("共 %1 件产物").arg(artifacts.size()),
                       false);
          });
  connect(client_, &FilesClient::pack_built, this, [this] {
    set_status(QStringLiteral("打包完成（stub 执行器；卡片已回群）"), false);
    refresh();
  });
  connect(client_, &FilesClient::pack_deleted, this, [this] {
    set_status(QStringLiteral("产物已删除"), false);
    refresh();
  });
  connect(client_, &FilesClient::tool_actions_set, this, [this] {
    set_status(QStringLiteral("已开放成员打包（pack/build 进白名单）"), false);
  });
  connect(client_, &FilesClient::group_exported, this,
          [this](const QJsonObject& snapshot) {
            snapshot_->setPlainText(
                QJsonDocument(snapshot).toJson(QJsonDocument::Indented));
            btn_save_as_->setEnabled(true);
            set_status(QStringLiteral("快照已导出（密文面不进快照；"
                                      "导出动作已留痕）"),
                       false);
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

void GroupPackDialog::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(12, 12, 12, 12);
  root->setSpacing(8);

  // —— 连接区（登录成功后整体停用；原型 conn-row 右上「已连接」徽章＋
  //    「群号 N」位）——
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
  gid_tag_ = new QLabel(this);
  conn_badge_ = new QLabel(QStringLiteral("未连接"), this);
  conn_badge_->setObjectName(QStringLiteral("pack_conn_badge"));
  conn_row->addWidget(new QLabel(QStringLiteral("服务器"), this));
  conn_row->addWidget(host_);
  conn_row->addWidget(new QLabel(QStringLiteral("文件面端口"), this));
  conn_row->addWidget(port_);
  conn_row->addWidget(new QLabel(QStringLiteral("账号"), this));
  conn_row->addWidget(account_);
  conn_row->addWidget(new QLabel(QStringLiteral("口令"), this));
  conn_row->addWidget(password_);
  conn_row->addWidget(btn_connect_);
  conn_row->addWidget(gid_tag_);
  conn_row->addStretch(1);
  conn_row->addWidget(conn_badge_);
  root->addLayout(conn_row);

  // —— 双栏 split（原型 .split）：左出产物列＋右快照导出列 ——
  auto* split = new QHBoxLayout;
  split->setSpacing(8);
  auto* left = new QWidget(this);
  auto* left_lay = new QVBoxLayout(left);
  left_lay->setContentsMargins(0, 0, 0, 0);
  left_lay->setSpacing(8);

  // —— 出产物 sec-head（原型 .sec-h：粗题＋灰副题）——
  auto* art_head = new QHBoxLayout;
  {
    auto* t = new QLabel(QStringLiteral("出产物"), left);
    QFont f = t->font();
    f.setBold(true);
    t->setFont(f);
    art_head->addWidget(t);
    art_head->addWidget(new QLabel(
        QStringLiteral("一键打包 · 群文件面归档 · 打包动作留痕"), left));
  }
  art_head->addStretch(1);
  left_lay->addLayout(art_head);

  // —— 产物台账 ——
  artifacts_ = new QListWidget(left);
  artifacts_->setAlternatingRowColors(true);
  artifacts_->setWordWrap(true);
  left_lay->addWidget(artifacts_, 1);

  // —— 白名单说明条（原型 .note）——
  {
    auto* n = new QLabel(
        QStringLiteral("打包须白名单开放（群主可切「开放成员打包」）；产物进"
                       "群文件面，受群配额与钉选管理。"),
        left);
    n->setWordWrap(true);
    left_lay->addWidget(n);
  }

  // —— 打包行（原型 .frow：产物名/版本/备注＋打包；白名单闸动作；删除恒
  //    归群主/管理员）——
  auto* build_row = new QHBoxLayout;
  name_ = new QLineEdit(left);
  name_->setPlaceholderText(QStringLiteral("产物名（如 memex-client）"));
  version_ = new QLineEdit(left);
  version_->setPlaceholderText(QStringLiteral("版本（v0.9.3）"));
  version_->setMaximumWidth(110);
  note_ = new QLineEdit(left);
  note_->setPlaceholderText(QStringLiteral("备注（可选）"));
  btn_build_ = new QPushButton(QStringLiteral("打包"), left);
  build_row->addWidget(name_, 1);
  build_row->addWidget(version_);
  build_row->addWidget(note_, 1);
  build_row->addWidget(btn_build_);
  left_lay->addLayout(build_row);

  // —— 动作行（原型 .btnrow：删除选中/开放成员打包/刷新＋危险动作注）——
  auto* act_row = new QHBoxLayout;
  btn_delete_ = new QPushButton(QStringLiteral("删除选中"), left);
  btn_delete_->setEnabled(false);
  btn_open_ = new QPushButton(QStringLiteral("开放成员打包"), left);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), left);
  act_row->addWidget(btn_delete_);
  act_row->addWidget(btn_open_);
  act_row->addWidget(btn_refresh_);
  act_row->addWidget(new QLabel(
      QStringLiteral("删除产物＝危险动作（可配二次确认）"), left), 1);
  left_lay->addLayout(act_row);
  split->addWidget(left, 1);

  // —— 右列（原型 aside.col：快照导出面；密文面永不进导出）——
  auto* right = new QWidget(this);
  auto* right_lay = new QVBoxLayout(right);
  right_lay->setContentsMargins(0, 0, 0, 0);
  right_lay->setSpacing(8);
  {
    auto* h = new QHBoxLayout;
    auto* t = new QLabel(QStringLiteral("群配置快照导出"), right);
    QFont f = t->font();
    f.setBold(true);
    t->setFont(f);
    h->addWidget(t);
    h->addWidget(new QLabel(
        QStringLiteral("只读 JSON · 脱敏（凭据只出状态不出值）"), right));
    h->addStretch(1);
    right_lay->addLayout(h);
  }
  snapshot_ = new QPlainTextEdit(right);
  snapshot_->setReadOnly(true);
  snapshot_->setPlaceholderText(
      QStringLiteral("（群主/管理员可导出：成员/白名单/流水线/密码箱存在性"
                     "——不含任何密文）"));
  right_lay->addWidget(snapshot_, 1);
  {
    auto* row = new QHBoxLayout;
    btn_export_ = new QPushButton(QStringLiteral("导出群配置快照…"), right);
    btn_save_as_ = new QPushButton(QStringLiteral("另存为文件…"), right);
    btn_save_as_->setEnabled(false);
    row->addWidget(btn_export_);
    row->addWidget(btn_save_as_);
    row->addStretch(1);
    right_lay->addLayout(row);
  }
  {
    auto* n = new QLabel(
        QStringLiteral("导出动作与 CI/CD 打包同源留痕（谁 / 何时 / 哪个群）；"
                       "快照仅含配置面，不含消息归档——消息导出走「导出群记"
                       "录…」。"),
        right);
    n->setWordWrap(true);
    right_lay->addWidget(n);
  }
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
  connect(btn_build_, &QPushButton::clicked, this, [this] {
    build_artifact(name_->text().trimmed(), version_->text().trimmed(),
                   note_->text().trimmed());
  });
  connect(artifacts_, &QListWidget::itemSelectionChanged, this, [this] {
    btn_delete_->setEnabled(artifacts_->currentItem() != nullptr &&
                            artifacts_->currentItem()->flags() &
                                Qt::ItemIsEnabled);
  });
  connect(btn_delete_, &QPushButton::clicked, this,
          [this] { delete_selected(); });
  connect(btn_open_, &QPushButton::clicked, this,
          [this] { open_build_whitelist(); });
  connect(btn_export_, &QPushButton::clicked, this,
          [this] { export_config(); });
  // 快照另存为文件（纯本地落盘：内容就是上面只读 JSON 面的现文）
  connect(btn_save_as_, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("快照另存为"),
        QStringLiteral("group-snapshot.json"),
        QStringLiteral("JSON (*.json)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      f.write(snapshot_->toPlainText().toUtf8());
      set_status(QStringLiteral("快照已另存为 %1").arg(path), false);
    } else {
      set_status(QStringLiteral("另存失败：无法写 %1").arg(path), true);
    }
  });
}

void GroupPackDialog::connect_to(const QString& host, quint16 files_port,
                                 const QString& account,
                                 const QString& password) {
  client_->login(host, files_port, account, password);
}

bool GroupPackDialog::is_connected() const { return client_->is_logged_in(); }

void GroupPackDialog::set_group(quint64 gid, const QString& group_name) {
  gid_ = gid;
  setWindowTitle(QStringLiteral("群打包 / 导出 — %1").arg(group_name));
  gid_tag_->setText(gid_ > 0
                        ? QStringLiteral("群号 %1").arg(QString::number(gid_))
                        : QString());
  artifacts_->clear();
  snapshot_->clear();
  btn_save_as_->setEnabled(false);
  if (gid_ > 0 && client_->is_logged_in()) refresh();
}

void GroupPackDialog::refresh() {
  if (gid_ == 0 || !client_->is_logged_in()) return;
  client_->pack_list(gid_);
}

bool GroupPackDialog::build_artifact(const QString& name,
                                     const QString& version,
                                     const QString& note) {
  if (name.isEmpty() || version.isEmpty()) {
    set_status(QStringLiteral("产物名与版本不能为空"), true);
    return false;
  }
  client_->pack_build(gid_, name, version, note);
  return true;
}

bool GroupPackDialog::delete_selected() {
  const auto* it = artifacts_->currentItem();
  if (!it) {
    set_status(QStringLiteral("先选中要删除的产物"), true);
    return false;
  }
  if (QMessageBox::question(this, QStringLiteral("删除产物"),
                            QStringLiteral("删除产物「%1 %2」？（打包留痕"
                                           "保留）")
                                .arg(it->data(kRoleName).toString(),
                                     it->data(kRoleVersion).toString())) !=
      QMessageBox::Yes) {
    return false;
  }
  client_->pack_delete(gid_, it->data(kRoleName).toString(),
                       it->data(kRoleVersion).toString());
  return true;
}

bool GroupPackDialog::open_build_whitelist() {
  if (gid_ == 0 || !client_->is_logged_in()) return false;
  client_->set_tool_actions(gid_, QStringLiteral("pack"),
                            {QStringLiteral("build")});
  return true;
}

void GroupPackDialog::export_config() {
  if (gid_ == 0 || !client_->is_logged_in()) return;
  client_->group_export(gid_);
}

QString GroupPackDialog::status_text() const { return status_->text(); }

int GroupPackDialog::artifact_count() const { return artifacts_->count(); }

QString GroupPackDialog::snapshot_text() const {
  return snapshot_->toPlainText();
}

void GroupPackDialog::set_status(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(
      error ? QStringLiteral("color:%1;")
                  .arg(ThemeManager::instance().tokens().danger.name())
            : QStringLiteral(""));
}

} // namespace memex::client
