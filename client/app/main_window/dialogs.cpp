// 对话框入口（LAY-002 拆分）：协作登录、连接/离开锁屏/网络远程
// 设置、统一设置窗、文件助手与个人资料签名。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include "collab_defaults.hpp"

#include <QDialogButtonBox>
#include <QIcon>
#include "file_assistant.hpp"
#include "group_memo_dialog.hpp"
#include "group_ci_dialog.hpp"
#include "group_pack_dialog.hpp"
#include "group_server_dialog.hpp"
#include "task_dialog.hpp"
#include "approval_dialog.hpp"
#include "report_dialog.hpp"
#include "group_tools_dialog.hpp"
#include "brand_settings_page.hpp"
#include "settings_window.hpp"
#include "away_settings.hpp"
#include "net_remote_settings.hpp"
#include "assist_dialog.hpp"
#include "audit_dialog.hpp"
#include "archive_console.hpp"
#include "office_map_dialog.hpp"
#include <QShortcut>
#include "brand_kit.hpp"


#include "theme.hpp"
#include "theme_settings_page.hpp"
namespace memex::client {

void MainWindow::show_collab_login_dialog() {
  QDialog dlg(this);
  const BrandKit& kit = BrandKit::instance();
  // 品牌随换：标题与顶部品牌区（公司名/logo/登录页文案；无牌=默认标）
  dlg.setWindowTitle(kit.company_name().isEmpty()
                         ? QStringLiteral("登录协作态")
                         : QStringLiteral("%1 · 登录").arg(
                               kit.company_name()));
  auto* lay = new QVBoxLayout(&dlg);
  if (kit.branded()) {
    auto* brand_area = new QWidget(&dlg);
    auto* brand_lay = new QHBoxLayout(brand_area);
    brand_lay->setContentsMargins(0, 0, 0, 0);
    auto* logo = new QLabel(brand_area);
    if (!kit.logo().isNull()) {
      logo->setPixmap(kit.logo().scaled(
          40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
      logo->setPixmap(kit.window_icon().pixmap(40, 40));
    }
    auto* name = new QLabel(kit.company_name(), brand_area);
    QFont bf = name->font();
    bf.setBold(true);
    bf.setPointSize(bf.pointSize() + 2);
    name->setFont(bf);
    brand_lay->addWidget(logo);
    brand_lay->addWidget(name);
    brand_lay->addStretch();
    lay->addWidget(brand_area);
    if (!kit.slogan().isEmpty()) {
      auto* slogan = new QLabel(kit.slogan(), &dlg);
      lay->addWidget(slogan);
    }
  }
  auto* form = new QFormLayout;
  lay->addLayout(form);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* host = new QLineEdit(
      settings.value(QStringLiteral("host"), default_collab_host())
          .toString(),
      &dlg);
  auto* port = new QLineEdit(
      settings.value(QStringLiteral("port"), default_collab_port()).toString(),
      &dlg);
  auto* files_port = new QLineEdit(
      settings.value(QStringLiteral("files_port"), default_files_port())
          .toString(),
      &dlg);
  auto* account = new QLineEdit(
      settings.value(QStringLiteral("account")).toString(), &dlg);
  auto* password = new QLineEdit(&dlg);
  password->setEchoMode(QLineEdit::Password);
  form->addRow(QStringLiteral("服务器地址"), host);
  form->addRow(QStringLiteral("端口"), port);
  form->addRow(QStringLiteral("文件口"), files_port);
  form->addRow(QStringLiteral("账号"), account);
  form->addRow(QStringLiteral("口令"), password);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  if (account->text().trimmed().isEmpty()) return;
  settings.setValue(QStringLiteral("host"), host->text().trimmed());
  settings.setValue(QStringLiteral("port"), port->text().trimmed());
  settings.setValue(QStringLiteral("files_port"),
                    files_port->text().trimmed());
  settings.setValue(QStringLiteral("account"), account->text().trimmed());
  // 品牌拉取（免鉴权 GET 不依赖登录成败；accept 后 host/文件口已知，
  // 比登录成功后拉取更早见效——设计稿 §4 差异如实注明）；host 先过
  // 解析覆盖（部署批 424），品牌与登录同源
  const QString resolved_host = resolve_conn_host(host->text().trimmed());
  BrandKit::instance().fetch(
      resolved_host, static_cast<quint16>(files_port->text().toUInt()));
  login_collab(resolved_host,
               static_cast<quint16>(port->text().toUInt()),
               account->text().trimmed(), password->text());
}

// 连接设置（部署批 424）：域名→IP 解析覆盖编辑。规则串 host_overrides
// （空=不覆盖，公网解析不受影响）；hosts/内网 DNS 等标准做法见 README
// 「公网接入：Cloudflare tunnel」节——此处只管本机客户端覆盖项。
// 离开与锁屏设置（选项菜单与统一设置窗·隐私页共用入口；懒建＋关即毁，
// 同 shortcut/conn 弹窗口径）
void MainWindow::show_away_dialog() {
  if (!away_dialog_) {
    away_dialog_ = new AwayLockSettingsDialog(this, this);
    away_dialog_->setAttribute(Qt::WA_DeleteOnClose);
  }
  away_dialog_->show();
  away_dialog_->raise();
  away_dialog_->activateWindow();
}

// 网络与远程设置（选项菜单与统一设置窗·隐私页共用入口）
void MainWindow::show_net_dialog() {
  if (!net_dialog_) {
    net_dialog_ = new NetRemoteSettingsDialog(this, this);
    net_dialog_->setAttribute(Qt::WA_DeleteOnClose);
  }
  net_dialog_->show();
  net_dialog_->raise();
  net_dialog_->activateWindow();
}

void MainWindow::show_conn_settings_dialog() {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("连接设置"));
  auto* lay = new QVBoxLayout(&dlg);
  auto* form = new QFormLayout;
  lay->addLayout(form);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* overrides = new QLineEdit(
      settings.value(QStringLiteral("host_overrides")).toString(), &dlg);
  overrides->setPlaceholderText(
      QStringLiteral("server.memex.cuihairu.site=192.168.5.5"));
  overrides->setToolTip(QStringLiteral(
      "域名=IP，多条用分号分隔；留空=不覆盖（公网走正常解析）。"
      "命中后该域名的连接直连内网 IP，不过 tunnel。"));
  form->addRow(QStringLiteral("域名解析覆盖"), overrides);
  auto* hint = new QLabel(
      QStringLiteral("仅影响本机连接目标（优先于系统 DNS）；局域网也可用 "
                     "hosts 或内网 DNS 强制解析——见 README 接入文档。"),
      &dlg);
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color: #7d7169;"));
  lay->addWidget(hint);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                       QDialogButtonBox::Cancel, &dlg);
  lay->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  const QString rules = overrides->text().trimmed();
  if (rules.isEmpty()) {
    settings.remove(QStringLiteral("host_overrides")); // 默认不覆盖：清项
  } else {
    settings.setValue(QStringLiteral("host_overrides"), rules);
  }
}

void MainWindow::show_file_assistant() {
  if (!file_assistant_) {
    file_assistant_ = new FileAssistantDialog(this);
    file_assistant_->setAttribute(Qt::WA_DeleteOnClose);
  }
  file_assistant_->show();
  file_assistant_->raise();
  file_assistant_->activateWindow();
}

void MainWindow::show_settings_window() {
  if (!settings_window_) {
    settings_window_ = new SettingsWindow(this);
    settings_window_->setAttribute(Qt::WA_DeleteOnClose);
  }
  settings_window_->show();
  settings_window_->raise();
  settings_window_->activateWindow();
}

// —— 需求批⑪ 个性签名：设置对话框（设置菜单入口；登录门＝签名存服务端档案）——
void MainWindow::show_profile_dialog() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("个人资料需登录协作态（签名存服务端档案）"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("个人资料 · 个性签名"));
  auto* layout = new QVBoxLayout(&dlg);
  auto* hint = new QLabel(
      QStringLiteral("签名在会话列表悬浮可见；留空保存＝清除。"), &dlg);
  layout->addWidget(hint);
  // 需求批⑫：我的头像（已设走缩放缓存，未设走默认头像组）＋更换入口
  {
    auto* avatar_row = new QHBoxLayout;
    auto* avatar_label = new QLabel(&dlg);
    avatar_label->setAlignment(Qt::AlignCenter);
    avatar_label->setPixmap(avatar_for(collab_engine_.account(),
                                       member_avatar_ver_.value(
                                           collab_engine_.account()), 64));
    avatar_row->addWidget(avatar_label);
    auto* btn_avatar = new QPushButton(QStringLiteral("更换头像…"), &dlg);
    avatar_row->addWidget(btn_avatar);
    avatar_row->addStretch(1);
    layout->addLayout(avatar_row);
    // 状态行红字明示（既有 request_failed 通道的对话框内显影；窗口销毁
    // 即断连，不影响主窗状态栏通道）
    auto* avatar_status = new QLabel(&dlg);
    avatar_status->setStyleSheet(
        QStringLiteral("color:%1;")
            .arg(ThemeManager::instance().tokens().danger.name()));
    avatar_status->setWordWrap(true);
    layout->addWidget(avatar_status);
    const QPointer<QLabel> status_guard(avatar_status);
    const QPointer<QLabel> pixmap_guard(avatar_label);
    connect(files_client(), &FilesClient::request_failed, &dlg,
            [status_guard](const QString& op, int status,
                           const QString& error) {
              if (!status_guard) return;
              status_guard->setText(
                  QStringLiteral("头像操作失败[%1]（%2）：%3")
                      .arg(op,
                           status > 0 ? QString::number(status)
                                      : QStringLiteral("网络"),
                           error));
            });
    connect(files_client(), &FilesClient::avatar_uploaded, &dlg,
            [this, status_guard, pixmap_guard](int, qint64) {
              if (!status_guard) return;
              // 收尾档完成时 ver 已入缓存（主窗先连），本机先见即刷
              if (avatar_upload_queue_.isEmpty() && avatar_new_ver_ > 0 &&
                  pixmap_guard) {
                pixmap_guard->setPixmap(
                    avatar_for(collab_engine_.account(), avatar_new_ver_,
                               64));
                status_guard->setText(QStringLiteral("头像已更新"));
              }
            });
    connect(btn_avatar, &QPushButton::clicked, &dlg,
            [this] { change_avatar(); });
  }
  // 需求批⑩：我的在线时长（滚动 24h/7d/30d 并集；org 数据到达即有值）
  const QString my_online = member_online_.value(collab_engine_.account());
  if (!my_online.isEmpty()) {
    layout->addWidget(new QLabel(my_online, &dlg));
  }
  auto* edit = new QLineEdit(&dlg);
  edit->setMaxLength(120); // 服务端同口径长度门
  edit->setText(member_signatures_.value(collab_engine_.account()));
  edit->setPlaceholderText(QStringLiteral("写一句个性签名（上限 120 字）"));
  layout->addWidget(edit);
  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                           &dlg);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, [this, &dlg, edit] {
    apply_signature(edit->text().trimmed());
    dlg.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(buttons);
  dlg.exec();
}

// 发送签名设置（带参＝测试直调不弹框；回执经 profile_result 异步到）
void MainWindow::apply_signature(const QString& signature) {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("设置签名需登录协作态"));
    return;
  }
  collab_engine_.set_signature(signature);
  show_status(QStringLiteral("签名设置请求已发送"));
}

QString MainWindow::own_signature() const {
  return member_signatures_.value(collab_engine_.account());
}

} // namespace memex::client
