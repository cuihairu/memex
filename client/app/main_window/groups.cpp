// 群聊（LAY-002 拆分）：群列表/常用联系人数据、群右栏、@ 菜单、
// 群对话框族与免服务端临时群。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QDateEdit>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFontMetrics>
#include <QGroupBox>
#include <QIcon>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QJsonObject>
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
#include "assist_dialog.hpp"
#include "audit_dialog.hpp"
#include "archive_console.hpp"
#include "office_map_dialog.hpp"
#include <QMessageBox>
#include <QRegularExpression>
#include <QTimer>
#include <utility>

#include "theme.hpp"
namespace memex::client {

// 建群（服务端群）：群名＋成员多选（组织架构账号＋手输补位）
void MainWindow::create_group_dialog() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("建群需登录协作态（群消息经服务端扇出并归档）"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("新建群聊"));
  dlg.resize(380, 520);
  auto* form = new QFormLayout(&dlg);
  auto* name = new QLineEdit(&dlg);
  name->setPlaceholderText(QStringLiteral("例如：研发部日常"));
  form->addRow(QStringLiteral("群名"), name);
  // 群组-1：主题（一句话）＋目的（多行说明）均可留空（定案①跳过＝
  // 直接建群；空主题落空串＝群名下方不显示该行）
  auto* topic = new QLineEdit(&dlg);
  topic->setPlaceholderText(QStringLiteral("一句话主题（可空）"));
  form->addRow(QStringLiteral("主题"), topic);
  auto* purpose = new QTextEdit(&dlg);
  purpose->setPlaceholderText(QStringLiteral("群目的说明（可空）"));
  purpose->setFixedHeight(64);
  form->addRow(QStringLiteral("目的"), purpose);
  auto* members = new QListWidget(&dlg);
  const QString me = collab_engine_.account();
  for (const QString& a : org_accounts()) {
    if (a == me) continue;
    auto* item = new QListWidgetItem(a, members);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  form->addRow(QStringLiteral("成员"), members);
  auto* extra = new QLineEdit(&dlg);
  extra->setPlaceholderText(QStringLiteral("其他账号，逗号分隔（可空）"));
  form->addRow(QString(), extra);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  const QString gname = name->text().trimmed();
  if (gname.isEmpty()) {
    show_status(QStringLiteral("建群取消：群名不能为空"));
    return;
  }
  QStringList picked;
  for (int i = 0; i < members->count(); ++i) {
    if (members->item(i)->checkState() == Qt::Checked) {
      picked << members->item(i)->text();
    }
  }
  for (const QString& part :
       extra->text().split(QChar(','), Qt::SkipEmptyParts)) {
    const QString a = part.trimmed();
    if (!a.isEmpty() && !picked.contains(a)) picked << a;
  }
  collab_engine_.create_group(gname, picked, topic->text().trimmed(),
                              purpose->toPlainText().trimmed());
  show_status(QStringLiteral("建群请求已发送（%1 名成员）").arg(picked.size()));
}

// 打开群会话（group_key 形如 "group:7"）
void MainWindow::open_group(const QString& group_key) {
  if (group_key.isEmpty()) return;
  const quint64 gid =
      group_key.mid(QStringLiteral("group:").size()).toULongLong();
  if (!groups_.contains(gid)) {
    show_status(QStringLiteral("群 %1 不在列表（可能已退群／解散）").arg(gid));
    return;
  }
  open_chat(QStringLiteral("group"), group_key);
}

// 常用联系人到达（T4.5）：缓存并刷新列表（星标置顶排序在 refresh 里按此算）
void MainWindow::apply_favs(const QString& fav_json) {
  last_fav_json_ = fav_json;
  refresh_devices();
}

QString MainWindow::fav_json() const { return last_fav_json_; }

bool MainWindow::fav_is_starred_(const QString& peer) const {
  nlohmann::json j = nlohmann::json::parse(last_fav_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) return false;
  for (const auto& e : j) {
    if (e.value("peer", std::string{}) == peer.toStdString()) {
      return e.value("starred", false);
    }
  }
  return false;
}

qint64 MainWindow::fav_last_ms_(const QString& peer) const {
  nlohmann::json j = nlohmann::json::parse(last_fav_json_.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) return 0;
  for (const auto& e : j) {
    if (e.value("peer", std::string{}) == peer.toStdString()) {
      return e.value("last_ms", 0LL);
    }
  }
  return 0;
}

void MainWindow::toggle_current_fav_star() {
  if (current_kind_ != QStringLiteral("collab") &&
      current_kind_ != QStringLiteral("group")) {
    show_status(QStringLiteral("常用联系人星标仅适用于协作会话／服务端群"));
    return;
  }
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("未登录协作态，常用联系人不可用"));
    return;
  }
  const QString peer = current_peer_;
  if (peer.isEmpty()) return;
  collab_engine_.fav_cmd(fav_is_starred_(peer) ? QStringLiteral("unstar")
                                               : QStringLiteral("star"),
                         peer);
}

// 群列表数据到达：解析入 groups_，刷新列表与当前群会话标题
void MainWindow::apply_groups(const QString& groups_json) {
  last_groups_json_ = groups_json;
  nlohmann::json j = nlohmann::json::parse(groups_json.toStdString(),
                                           nullptr, false);
  if (j.is_discarded() || !j.is_array()) {
    show_status(QStringLiteral("群列表数据解析失败"));
    return;
  }
  groups_.clear();
  for (const auto& g : j) {
    GroupEntry e;
    e.name = QString::fromStdString(g.value("name", std::string{}));
    e.owner = QString::fromStdString(g.value("owner", std::string{}));
    e.announcement =
        QString::fromStdString(g.value("announcement", std::string{}));
    e.topic = QString::fromStdString(g.value("topic", std::string{}));
    e.purpose = QString::fromStdString(g.value("purpose", std::string{}));
    if (g.contains("members")) {
      for (const auto& m : g["members"]) {
        e.members << QString::fromStdString(m.get<std::string>());
      }
    }
    groups_.insert(g.value("group_id", 0), std::move(e));
  }
  // 当前打开的群：标题/meta/右栏随最新数据刷新；群没了则收尾提示
  if (current_kind_ == QStringLiteral("group")) {
    if (GroupEntry* g = current_group()) {
      chat_title_->setText(g->name);
      chat_title_->setToolTip(
          QStringLiteral("群 %1").arg(current_group_id()));
      chat_meta_->setText(group_meta_text(*g));
      update_group_panel();
      update_banner(); // 公告变化→横幅随换（群公告占横幅口径）
    } else {
      append_system_line(
          QStringLiteral("该群已退出或已解散：本会话仅供查看历史"));
    }
  }
  refresh_devices();
}

quint64 MainWindow::current_group_id() const {
  if (current_kind_ != QStringLiteral("group")) return 0;
  return current_peer_.mid(QStringLiteral("group:").size()).toULongLong();
}

MainWindow::GroupEntry* MainWindow::current_group() {
  const quint64 gid = current_group_id();
  auto it = groups_.find(gid);
  return gid > 0 && it != groups_.end() ? &it.value() : nullptr;
}

// 通讯录账号→显示名（org 未下发＝空）：群右栏成员行与群主名用
QString MainWindow::roster_display(const QString& account) const {
  const QJsonDocument od = QJsonDocument::fromJson(last_org_json_.toUtf8());
  if (!od.isObject()) return QString();
  for (const auto& m :
       od.object().value(QStringLiteral("members")).toArray()) {
    const QJsonObject mo = m.toObject();
    if (mo.value(QStringLiteral("account")).toString() == account) {
      const QString d = mo.value(QStringLiteral("display_name")).toString();
      if (!d.isEmpty()) return d;
    }
  }
  return QString();
}

// 群会话头 meta 行（原型 desktop-group.html .ch-meta「N 人 · 群主：X ·
// 全量归档」）；主题/目的为群组-1 功能面，非空缀尾（原型未覆盖，保留）
QString MainWindow::group_meta_text(const GroupEntry& g) const {
  QString owner = roster_display(g.owner);
  if (owner.isEmpty()) owner = g.owner;
  QString meta = QStringLiteral("%1 人 · 群主：%2 · 全量归档")
                     .arg(g.members.size())
                     .arg(owner);
  if (!g.topic.isEmpty()) {
    meta += QStringLiteral("　·　主题：%1").arg(g.topic);
  }
  if (!g.purpose.isEmpty()) {
    QString p = g.purpose;
    p.replace(QChar('\n'), QStringLiteral("／"));
    meta += QStringLiteral("　·　目的：%1").arg(p);
  }
  return meta;
}

// 群右栏（原型 desktop-group.html .infopanel）：群成员（≤6 行＋「查看全部」
// ＝群资料窗）、群文件（会话窗口内的文件卡——文件传输不落消息库，会话内存
// 态如实口径）、尾卡归档说明虚线框。重建式刷新：数据与主题变化都重摆。
void MainWindow::update_group_panel() {
  if (!group_panel_ || !group_panel_lay_) return;
  const bool group_session = current_kind_ == QStringLiteral("group") &&
                             chat_open_ && collab_engine_.is_logged_in();
  const GroupEntry* g = group_session ? current_group() : nullptr;
  if (!g) {
    group_panel_->hide();
    return;
  }
  group_panel_->setVisible(!group_panel_hidden_);
  while (group_panel_lay_->count() > 0) {
    QLayoutItem* it = group_panel_lay_->takeAt(0);
    if (it->widget()) it->widget()->deleteLater();
    delete it;
  }
  const ThemeTokens& t = tk();
  auto add_h4 = [this, &t](const QString& text) {
    auto* h = new QLabel(text, group_panel_);
    h->setStyleSheet(QStringLiteral("font-size:12px; font-weight:700; "
                                    "color:%1;")
                         .arg(t.text_muted.name()));
    group_panel_lay_->addWidget(h);
  };

  // 群成员（原型 .member-row：28px 圆角 8 头像＋名＋.role 群主胶囊）
  add_h4(QStringLiteral("群成员 · %1").arg(g->members.size()));
  const QString me = collab_engine_.account();
  int shown = 0;
  for (const QString& acc : g->members) {
    if (shown >= 6) break;
    ++shown;
    QString display = roster_display(acc);
    if (display.isEmpty()) display = acc;
    auto* row = new QWidget(group_panel_);
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto* av = new QLabel(avatar_initial(display), row);
    av->setFixedSize(28, 28);
    av->setAlignment(Qt::AlignCenter);
    av->setStyleSheet(QStringLiteral("background:%1; color:%2; "
                                     "font-weight:700; font-size:11px; "
                                     "border-radius:8px;")
                          .arg(avatar_hue(display).name(),
                               t.on_brand.name()));
    auto* nm = new QLabel(acc == me
                              ? QStringLiteral("%1（我）").arg(display)
                              : display,
                          row);
    nm->setStyleSheet(
        QStringLiteral("font-size:12.5px; color:%1;").arg(t.text.name()));
    lay->addWidget(av);
    lay->addWidget(nm, 1);
    if (acc == g->owner) {
      auto* role = new QLabel(QStringLiteral("群主"), row);
      role->setStyleSheet(
          QStringLiteral("color:%1; background:%2; font-size:10.5px; "
                         "border-radius:999px; padding:1px 7px;")
              .arg(t.brand_text.name(), t.brand_wash.name()));
      lay->addWidget(role, 0, Qt::AlignRight);
    }
    group_panel_lay_->addWidget(row);
  }
  if (g->members.size() > 6) {
    // 「查看全部 N 人」：开群资料窗（成员全集在彼处）
    auto* more = new QPushButton(
        QStringLiteral("查看全部 %1 人").arg(g->members.size()),
        group_panel_);
    more->setFlat(true);
    more->setCursor(Qt::PointingHandCursor);
    more->setStyleSheet(
        QStringLiteral("color:%1; font-size:11.5px; padding:6px 0;")
            .arg(t.text_muted.name()));
    connect(more, &QPushButton::clicked, this,
            [this, gid = current_group_id()] { group_profile_dialog(gid); });
    group_panel_lay_->addWidget(more, 0, Qt::AlignHCenter);
  }

  // 群文件（原型：📄 文件名行×3）——取本会话窗口内最近 3 个文件卡
  add_h4(QStringLiteral("群文件"));
  QStringList files;
  for (const ChatRow& r : chat_rows_) {
    if (r.file_card) files << r.text;
  }
  if (files.isEmpty()) {
    auto* empty = new QLabel(
        QStringLiteral("暂无群文件（会话内收发的文件在此列出）"),
        group_panel_);
    empty->setWordWrap(true);
    empty->setStyleSheet(
        QStringLiteral("font-size:11px; color:%1;").arg(t.text_muted.name()));
    group_panel_lay_->addWidget(empty);
  } else {
    const QFontMetrics fm(font());
    for (auto it = files.crbegin();
         it != files.crend() && it - files.crbegin() < 3; ++it) {
      auto* f = new QLabel(QStringLiteral("%1  %2").arg(
                               file_icon_char(*it),
                               fm.elidedText(*it, Qt::ElideRight, 170)),
                           group_panel_);
      f->setStyleSheet(
          QStringLiteral("font-size:12px; color:%1;").arg(t.text.name()));
      group_panel_lay_->addWidget(f);
    }
  }
  group_panel_lay_->addStretch();
  // 归档说明虚线卡（原型 .infopanel 尾卡：11px 三行说明）
  auto* card = new QLabel(
      QStringLiteral("群消息与群文件元数据全量归档，可按群成员与关键词检"
                     "索。直连临时群（见列表）不经服务端、不归档。"),
      group_panel_);
  card->setWordWrap(true);
  card->setStyleSheet(
      QStringLiteral("border:1px dashed %1; border-radius:10px; "
                     "padding:10px 12px; font-size:11px; color:%2;")
          .arg(t.surface_alt.name(), t.text_muted.name()));
  group_panel_lay_->addWidget(card);
}

// @ 成员选择菜单（原型 desktop-group.html「@ 唤起成员选择」）：输入「@词头」
// 时弹成员菜单（显示名按词头过滤，≤12 项）；无词头/非群会话/无成员＝收起。
// 选中回填「@显示名 」并停用菜单（词头消失，textChanged 自动收）。
void MainWindow::rebuild_at_menu(const QString& text) {
  if (!at_menu_) return;
  const bool group_ok =
      current_kind_ == QStringLiteral("group") &&
      collab_engine_.is_logged_in();
  static const QRegularExpression tail_re(
      QStringLiteral("@([A-Za-z0-9_.\\-\\x{4e00}-\\x{9fa5}]*)$"));
  const auto m = tail_re.match(text);
  const GroupEntry* g = group_ok ? current_group() : nullptr;
  if (!g || !m.hasMatch()) {
    at_menu_->hide();
    return;
  }
  const QString partial = m.captured(1);
  const int at_idx = m.capturedStart(1) - 1;
  const QString me = collab_engine_.account();
  at_menu_->clear();
  int added = 0;
  for (const QString& acc : g->members) {
    if (acc == me) continue; // @ 自己无意义
    QString d = roster_display(acc);
    if (d.isEmpty()) d = acc;
    if (!partial.isEmpty() &&
        !d.contains(partial, Qt::CaseInsensitive) &&
        !acc.contains(partial, Qt::CaseInsensitive)) {
      continue;
    }
    QAction* act =
        at_menu_->addAction(QStringLiteral("%1（%2）").arg(d, acc));
    connect(act, &QAction::triggered, this, [this, d, at_idx] {
      QString cur = input_box_->text();
      cur = cur.left(at_idx) + QStringLiteral("@") + d + QStringLiteral(" ");
      input_box_->setText(cur);
      input_box_->setCursorPosition(cur.size());
      input_box_->setFocus();
      at_menu_->hide();
    });
    if (++added >= 12) break;
  }
  if (added == 0) {
    at_menu_->hide();
    return;
  }
  if (!at_menu_->isVisible()) {
    at_menu_->adjustSize();
    const QPoint pos = input_box_->mapToGlobal(QPoint(0, 0));
    at_menu_->popup(QPoint(pos.x(),
                           pos.y() - at_menu_->sizeHint().height() - 4));
  }
}

// 发起单聊（原型 side-head + 钮）：候选＝组织架构成员（未拉到回落
// 会话表∪在线表，去重去自己），选中即开该协作会话
void MainWindow::show_new_chat_menu() {
  if (!collab_engine_.is_logged_in()) return;
  QStringList accounts;
  const QJsonDocument od = QJsonDocument::fromJson(last_org_json_.toUtf8());
  if (od.isObject()) {
    for (const auto& m :
         od.object().value(QStringLiteral("members")).toArray()) {
      accounts << m.toObject().value(QStringLiteral("account")).toString();
    }
  }
  if (accounts.isEmpty()) {
    accounts = collab_peers_.values() + online_accounts_.values();
  }
  accounts.removeAll(collab_engine_.account());
  accounts.removeDuplicates();
  accounts.sort();
  if (accounts.isEmpty()) {
    show_status(QStringLiteral("暂无可发起会话的联系人（组织架构未下发）"));
    return;
  }
  QMenu menu(this);
  for (const QString& a : accounts) {
    QAction* act = menu.addAction(a);
    connect(act, &QAction::triggered, this, [this, a] {
      open_chat(QStringLiteral("collab"), a);
    });
  }
  menu.exec(new_chat_btn_->mapToGlobal(
      QPoint(0, new_chat_btn_->height())));
}

// 群条目右键菜单：服务端群（拉人／公告／退群）与临时群（解散）
void MainWindow::show_group_menu(const QPoint& pos) {
  auto* item = device_list_->itemAt(pos);
  if (!item) return;
  const QString kind = item->data(Qt::UserRole + 1).toString();
  const QString id = item->data(Qt::UserRole).toString();
  QMenu menu(this);
  if (kind == QStringLiteral("collab") || kind == QStringLiteral("group")) {
    // T4.5：常用联系人星标／取消（group kind 的 id 形如 "group:N"，直接用）
    const bool starred = fav_is_starred_(id);
    auto* act_star = menu.addAction(
        starred ? QStringLiteral("取消星标") : QStringLiteral("星标（置顶）"));
    connect(act_star, &QAction::triggered, this, [this, id, starred] {
      if (!collab_engine_.is_logged_in()) {
        show_status(QStringLiteral("未登录协作态，常用联系人不可用"));
        return;
      }
      collab_engine_.fav_cmd(starred ? QStringLiteral("unstar")
                                     : QStringLiteral("star"),
                             id);
    });
    // 需求批⑦：会话级已读回执开关（显式 true/false 覆盖全局缺省）
    auto* act_receipt =
        menu.addAction(QStringLiteral("发送已读回执（本会话）"));
    act_receipt->setCheckable(true);
    act_receipt->setChecked(read_receipts_enabled(id));
    connect(act_receipt, &QAction::toggled, this,
            [this, id](bool on) { set_read_receipts_enabled(id, on); });
    menu.addSeparator();
  }
  if (kind == QStringLiteral("group")) {
    const quint64 gid = id.mid(QStringLiteral("group:").size()).toULongLong();
    auto* act_invite = menu.addAction(QStringLiteral("拉人进群…"));
    auto* act_ann = menu.addAction(QStringLiteral("设置群公告…"));
    auto* act_profile = menu.addAction(QStringLiteral("设置群资料…"));
    auto* act_hist = menu.addAction(QStringLiteral("公告编辑历史…"));
    auto* act_memo = menu.addAction(QStringLiteral("群备忘录…"));
    auto* act_vault = menu.addAction(QStringLiteral("群密码箱…"));
    auto* act_ci = menu.addAction(QStringLiteral("CI/CD 流水线…"));
    auto* act_pack = menu.addAction(QStringLiteral("打包与导出…"));
    auto* act_record = menu.addAction(QStringLiteral("导出群记录…"));
    auto* act_server = menu.addAction(QStringLiteral("服务器…"));
    menu.addSeparator();
    auto* act_leave = menu.addAction(QStringLiteral("退出群聊"));
    connect(act_invite, &QAction::triggered, this,
            [this, gid] { group_invite_dialog(gid); });
    connect(act_ann, &QAction::triggered, this,
            [this, gid] { group_announce_dialog(gid); });
    connect(act_profile, &QAction::triggered, this,
            [this, gid] { group_profile_dialog(gid); });
    connect(act_hist, &QAction::triggered, this,
            [this, gid] { group_announce_history_dialog(gid); });
    connect(act_memo, &QAction::triggered, this, [this, gid] {
      // R24-2 群备忘录：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_memo_) {
        group_memo_ = new GroupMemoDialog(this);
      }
      group_memo_->set_group(gid, it != groups_.end() ? it->name
                                                     : QStringLiteral("群 %1").arg(gid));
      group_memo_->show();
      group_memo_->raise();
      group_memo_->activateWindow();
    });
    connect(act_vault, &QAction::triggered, this, [this, gid] {
      // R24-3 群密码箱：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_vault_) {
        group_vault_ = new GroupVaultDialog(this);
      }
      group_vault_->set_group(gid, it != groups_.end() ? it->name
                                                       : QStringLiteral("群 %1").arg(gid));
      group_vault_->show();
      group_vault_->raise();
      group_vault_->activateWindow();
    });
    connect(act_ci, &QAction::triggered, this, [this, gid] {
      // R25-2 群 CI/CD：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_ci_) {
        group_ci_ = new GroupCiDialog(this);
      }
      group_ci_->set_group(gid, it != groups_.end() ? it->name
                                                    : QStringLiteral("群 %1").arg(gid));
      group_ci_->show();
      group_ci_->raise();
      group_ci_->activateWindow();
    });
    connect(act_pack, &QAction::triggered, this, [this, gid] {
      // R25-3 打包/导出：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_pack_) {
        group_pack_ = new GroupPackDialog(this);
      }
      group_pack_->set_group(gid, it != groups_.end() ? it->name
                                                      : QStringLiteral("群 %1").arg(gid));
      group_pack_->show();
      group_pack_->raise();
      group_pack_->activateWindow();
    });
    connect(act_record, &QAction::triggered, this,
            [this, gid] { group_record_export_dialog(gid); });
    connect(act_server, &QAction::triggered, this, [this, gid] {
      // R26-2 群服务器面板：独立文件面会话窗口（懒建复用，换群重拉）
      const auto it = groups_.find(gid);
      if (!group_server_) {
        group_server_ = new GroupServerDialog(this);
      }
      group_server_->set_group(gid, it != groups_.end() ? it->name
                                                        : QStringLiteral("群 %1").arg(gid));
      group_server_->show();
      group_server_->raise();
      group_server_->activateWindow();
    });
    connect(act_leave, &QAction::triggered, this, [this, gid] {
      if (QMessageBox::question(
              this, QStringLiteral("退出群聊"),
              QStringLiteral("确认退出该群？（群主退群＝解散该群）")) !=
          QMessageBox::Yes) {
        return;
      }
      collab_engine_.leave_group(gid);
    });
    menu.exec(device_list_->mapToGlobal(pos));
  } else if (kind == QStringLiteral("dgroup")) {
    auto* act_del = menu.addAction(QStringLiteral("解散临时群"));
    connect(act_del, &QAction::triggered, this, [this, id] {
      dgroup_members_.remove(id);
      if (current_peer_ == id) {
        current_kind_ = QStringLiteral("direct");
        current_peer_.clear();
      }
      refresh_devices();
      show_status(QStringLiteral("临时群已解散（本就不进归档，无服务端动作）"));
    });
    menu.exec(device_list_->mapToGlobal(pos));
  }
}

// 拉人进群（成员数据源同建群；服务端校验账号存在且不在群里）
void MainWindow::group_invite_dialog(quint64 group_id) {
  if (!groups_.contains(group_id)) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("拉人进群（群 %1）").arg(group_id));
  dlg.resize(380, 480);
  auto* layout = new QVBoxLayout(&dlg);
  auto* members = new QListWidget(&dlg);
  const QString me = collab_engine_.account();
  const QStringList in_group = groups_.value(group_id).members;
  for (const QString& a : org_accounts()) {
    if (a == me || in_group.contains(a)) continue;
    auto* item = new QListWidgetItem(a, members);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
  }
  auto* extra = new QLineEdit(&dlg);
  extra->setPlaceholderText(QStringLiteral("其他账号，逗号分隔（可空）"));
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(members);
  layout->addWidget(extra);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList picked;
  for (int i = 0; i < members->count(); ++i) {
    if (members->item(i)->checkState() == Qt::Checked) {
      picked << members->item(i)->text();
    }
  }
  for (const QString& part :
       extra->text().split(QChar(','), Qt::SkipEmptyParts)) {
    const QString a = part.trimmed();
    if (!a.isEmpty() && !picked.contains(a)) picked << a;
  }
  if (picked.isEmpty()) {
    show_status(QStringLiteral("拉人取消：未选择账号"));
    return;
  }
  collab_engine_.invite_group(group_id, picked);
  show_status(QStringLiteral("拉人请求已发送（%1 人）").arg(picked.size()));
}

// 群公告（群主/管理员可设，R24-1 权限放宽；服务端校验并回执结果）
void MainWindow::group_announce_dialog(quint64 group_id) {
  const auto it = groups_.find(group_id);
  if (it == groups_.end()) return;
  bool ok = false;
  const QString text = QInputDialog::getMultiLineText(
      this, QStringLiteral("设置群公告（群 %1）").arg(group_id),
      QStringLiteral("群主/管理员可设；留空＝清除公告"), it->announcement, &ok);
  if (!ok) return;
  collab_engine_.announce_group(group_id, text.trimmed());
  show_status(QStringLiteral("群公告设置请求已发送（全员将收到重要级通知）"));
}

// 群资料（群组-1 主题/目的）编辑：权沿群公告先例（群主/管理员，服务端
// 裁决回执）；两字段整组覆盖＝留空即清除。回执落定后重拉群列表对齐。
void MainWindow::group_profile_dialog(quint64 group_id) {
  const auto it = groups_.find(group_id);
  if (it == groups_.end()) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("群资料（群 %1）").arg(group_id));
  dlg.resize(420, 300);
  auto* form = new QFormLayout(&dlg);
  auto* topic = new QLineEdit(it->topic, &dlg);
  topic->setPlaceholderText(QStringLiteral("一句话主题（可空）"));
  form->addRow(QStringLiteral("主题"), topic);
  auto* purpose = new QTextEdit(it->purpose, &dlg);
  purpose->setPlaceholderText(QStringLiteral("群目的说明（可空）"));
  form->addRow(QStringLiteral("目的"), purpose);
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted) return;
  collab_engine_.set_group_profile(group_id, topic->text().trimmed(),
                                   purpose->toPlainText().trimmed());
  show_status(QStringLiteral("群资料设置请求已发送（群主/管理员可设）"));
}

// 群组-2 群记录导出（定案②全员：本群成员即可导，服务端判权；定案③留痕：
// 服务端一次导出落 audit_reads 一行；定案④ Markdown .md）：区间＋AI 档两
// 选项→逐页拉取（keyset 游标续传、可中断、触顶 truncated 不静默截尾）→
// 拼接后存 .md（含「AI 未启用」降级注明——模型未接线）。
void MainWindow::group_record_export_dialog(quint64 group_id) {
  const auto it = groups_.find(group_id);
  if (it == groups_.end()) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("导出群记录（群 %1 · %2）")
                         .arg(QString::number(group_id), it->name));
  dlg.resize(980, 620);
  auto* root = new QVBoxLayout(&dlg);
  root->setContentsMargins(12, 12, 12, 12);
  root->setSpacing(8);
  // 双栏 split（原型 .split）：左导出区间表单列＋右产物结构参考列
  auto* split = new QHBoxLayout;
  split->setSpacing(8);
  auto* form_col = new QWidget(&dlg);
  form_col->setFixedWidth(360);
  auto* form_col_lay = new QVBoxLayout(form_col);
  form_col_lay->setContentsMargins(0, 0, 0, 0);
  form_col_lay->setSpacing(8);

  // 区间头（原型 .eh）
  {
    auto* eh = new QLabel(QStringLiteral("导出区间（群右键「导出群记录…」）"),
                          form_col);
    QFont f = eh->font();
    f.setBold(true);
    eh->setFont(f);
    form_col_lay->addWidget(eh);
  }
  auto* form = new QFormLayout();
  form->setLabelAlignment(Qt::AlignLeft);
  auto* from_edit =
      new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7), form_col);
  from_edit->setCalendarPopup(true);
  from_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  form->addRow(QStringLiteral("起始时间（含）"), from_edit);
  auto* to_edit = new QDateTimeEdit(QDateTime::currentDateTime(), form_col);
  to_edit->setCalendarPopup(true);
  to_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  form->addRow(QStringLiteral("截止时间（含）"), to_edit);
  form_col_lay->addLayout(form);
  auto* cb_ai = new QCheckBox(
      QStringLiteral("生成 AI 总结（未接线则自动降级为普通文档）"), form_col);
  form_col_lay->addWidget(cb_ai);

  // 进度区（原型 .progress：进度文＋可中断续传注）
  auto* progress = new QLabel(QStringLiteral("尚未导出"), form_col);
  progress->setWordWrap(true);
  form_col_lay->addWidget(progress);
  form_col_lay->addWidget(new QLabel(
      QStringLiteral("可中断：中断后按 after_id 续传，不重导已导页"),
      form_col));

  // 按钮行（原型 .btnrow：开始导出／保存为 Markdown…／关闭）
  auto* buttons = new QDialogButtonBox(form_col);
  auto* btn_start = buttons->addButton(QStringLiteral("开始导出"),
                                       QDialogButtonBox::ActionRole);
  auto* btn_save = buttons->addButton(QStringLiteral("保存为 Markdown…"),
                                      QDialogButtonBox::ActionRole);
  auto* btn_close =
      buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
  btn_save->setEnabled(false);
  form_col_lay->addWidget(buttons);
  Q_UNUSED(btn_close);

  // 导出完成态卡（原型 .card2：完成口径＋truncated 续传游标）
  auto* done_card = new QGroupBox(QStringLiteral("导出完成态"), form_col);
  auto* done_lay = new QVBoxLayout(done_card);
  done_lay->setSpacing(2);
  auto* done_lbl = new QLabel(QStringLiteral("（尚未导出）"), done_card);
  done_lbl->setWordWrap(true);
  done_lay->addWidget(done_lbl);
  form_col_lay->addWidget(done_card);

  // 权限/审计说明条（原型 .note）
  {
    auto* n = new QLabel(
        QStringLiteral("权限：群成员均可导出，非成员 403。\n审计：每次导出写"
                       "一条审计行（谁 / 何时 / 哪个群 / 区间 / 条数 / AI 标"
                       "记），管理台可查。"),
        form_col);
    n->setWordWrap(true);
    form_col_lay->addWidget(n);
  }
  form_col_lay->addStretch(1);
  split->addWidget(form_col);

  // 右列（原型 .ref-col：产物结构参考；导出完成后载真实 md 预览）
  auto* ref_col = new QWidget(&dlg);
  auto* ref_lay = new QVBoxLayout(ref_col);
  ref_lay->setContentsMargins(0, 0, 0, 0);
  ref_lay->setSpacing(8);
  {
    auto* h = new QHBoxLayout;
    auto* t = new QLabel(
        QStringLiteral("导出产物结构（Markdown，非本窗）"), ref_col);
    QFont f = t->font();
    f.setBold(true);
    t->setFont(f);
    h->addWidget(t);
    h->addWidget(new QLabel(
        QStringLiteral("三段固定 · 撤回带标记 · 不静默截断"), ref_col));
    h->addStretch(1);
    ref_lay->addLayout(h);
  }
  auto* doc = new QPlainTextEdit(ref_col);
  doc->setReadOnly(true);
  doc->setPlaceholderText(QStringLiteral(
      "（导出完成后此处载入真实产物预览；结构＝# 群记录导出 / ## 参与者 / "
      "## 时间范围 / ## 聊天记录 三段固定；撤回行带（撤回）标记；触顶尾"
      "部 truncated 标注＋续传 after_id）"));
  ref_lay->addWidget(doc, 1);
  {
    auto* box = new QGroupBox(QStringLiteral("与群配置快照的区别"), ref_col);
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(2);
    auto* n = new QLabel(
        QStringLiteral("群配置快照（「群打包 / 导出」）＝配置面（工具白名单"
                       " / 凭据状态 / 成员表，不含消息）；本导出＝消息归档正"
                       "文（参与者 + 时间范围 + 聊天记录），两者互不覆盖。"),
        box);
    n->setWordWrap(true);
    lay->addWidget(n);
    ref_lay->addWidget(box);
  }
  split->addWidget(ref_col, 1);
  root->addLayout(split, 1);

  // 导出会话状态（逐页累积；随对话框销毁自动作废）
  struct ExportState {
    quint64 gid = 0;
    qint64 from_ms = 0;
    qint64 to_ms = 0;
    bool with_ai = false;
    qint64 fetched = 0;
    qint64 after_id = 0;
    int pages = 0;
    QString md;
  };
  QSharedPointer<ExportState> st(new ExportState());

  auto finish = [&](const QString& msg, bool can_save) {
    progress->setText(msg);
    done_lbl->setText(msg);
    if (can_save && !st->md.isEmpty()) {
      doc->setPlainText(st->md); // 右栏载真实产物预览（非示意文）
    }
    btn_start->setEnabled(true);
    btn_save->setEnabled(can_save);
  };
  auto begin_export = [&]() {
    progress->setText(QStringLiteral("导出中…（第 1 页）"));
    files_client()->group_record_export(st->gid, st->from_ms, st->to_ms,
                                       st->with_ai, 0, 0);
  };
  auto start = [&]() {
    btn_start->setEnabled(false);
    btn_save->setEnabled(false);
    st->gid = group_id;
    st->from_ms = from_edit->dateTime().toMSecsSinceEpoch();
    st->to_ms = to_edit->dateTime().toMSecsSinceEpoch();
    st->with_ai = cb_ai->isChecked();
    st->fetched = 0;
    st->after_id = 0;
    st->pages = 0;
    st->md.clear();
    if (!files_ensure_login()) {
      finish(QStringLiteral("文件面未连接（files_port 未配置或协作未登录）"),
             false);
      return;
    }
    if (!files_client()->is_logged_in()) {
      progress->setText(QStringLiteral("文件面连接中…"));
      // 异步登录：logged_in 到后开拉（SingleShot＝一次导出只触发一次）
      QObject::connect(files_client(), &FilesClient::logged_in, &dlg,
                       [&]() { begin_export(); }, Qt::SingleShotConnection);
      QObject::connect(files_client(), &FilesClient::login_failed, &dlg,
                       [this, &dlg, finish](const QString& err) {
                         finish(QStringLiteral("文件面登录失败：%1").arg(err),
                                false);
                       });
      return;
    }
    begin_export();
  };
  auto save = [&]() {
    const QString suggested = QStringLiteral("group-%1-record.md").arg(st->gid);
    const QString path = QFileDialog::getSaveFileName(
        &dlg, QStringLiteral("保存群记录"), suggested,
        QStringLiteral("Markdown（.md）;;所有文件"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
      show_status(QStringLiteral("保存失败：%1").arg(path));
      return;
    }
    f.write(st->md.toUtf8());
    f.close();
    show_status(QStringLiteral("群记录已保存（%1）").arg(path));
  };

  QObject::connect(btn_start, &QPushButton::clicked, &dlg,
                   [&]() { start(); });
  QObject::connect(btn_save, &QPushButton::clicked, &dlg, [&]() { save(); });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg,
                   &QDialog::reject);
  // 单页回包：拼 md＋续页（游标续传；触顶 truncated 即收尾，不静默截尾）
  QObject::connect(files_client(), &FilesClient::group_record_export_page, &dlg,
                   [this, &dlg, st, progress, &finish, &save](
                       const QJsonObject& page) {
                     if (page.value(QStringLiteral("gid")).toVariant().toULongLong() !=
                         st->gid) {
                       return; // 其他群的回执不接
                     }
                     const QString md =
                         page.value(QStringLiteral("md")).toString();
                     st->md += md;
                     st->pages += 1;
                     st->fetched += page.value(QStringLiteral("count")).toInt();
                     const bool has_more =
                         page.value(QStringLiteral("has_more")).toBool();
                     const bool truncated =
                         page.value(QStringLiteral("truncated")).toBool();
                     const qint64 next_id =
                         page.value(QStringLiteral("next_after_id"))
                             .toVariant()
                             .toLongLong();
                     if (has_more && !truncated && next_id > 0) {
                       progress->setText(
                           QStringLiteral("导出中…（第 %1 页，已 %2 条）")
                               .arg(st->pages + 1)
                               .arg(st->fetched));
                       files_client()->group_record_export(
                           st->gid, st->from_ms, st->to_ms, st->with_ai,
                           next_id, st->fetched);
                       return;
                     }
                     QString msg = QStringLiteral("导出完成（%1 页 %2 条）")
                                       .arg(st->pages)
                                       .arg(st->fetched);
                     if (truncated) {
                       msg += QStringLiteral("（已达单次上限，尾部 truncated 标"
                                            "注；续传用 after_id=%1）")
                                    .arg(next_id);
                     }
                     finish(msg, true);
                     if (st->with_ai &&
                         !page.value(QStringLiteral("ai_enabled")).toBool()) {
                       // 降级口径：AI 未接线＝普通文档＋「AI 未启用」注明
                       show_status(QStringLiteral(
                           "AI 未启用：已导出为普通文档（模型未接线/网关不可达"
                           "自动降级）"));
                     }
                     save();
                   });
  QObject::connect(files_client(), &FilesClient::group_record_export_failed,
                   &dlg, [&dlg, st, finish](const QString& err) {
                     finish(QStringLiteral("导出失败：%1").arg(err), false);
                   });
  dlg.exec();
}

// 公告编辑历史（R24-1）：请求后等专用回执（超时兜底），倒序列出
// 谁/何时/改成了什么（content 空串＝该次为清除）。
void MainWindow::group_announce_history_dialog(quint64 group_id) {
  if (!groups_.contains(group_id)) return;
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("公告编辑历史（群 %1）").arg(group_id));
  dlg.resize(520, 400);
  auto* layout = new QVBoxLayout(&dlg);
  auto* list = new QListWidget(&dlg);
  list->setWordWrap(true);
  layout->addWidget(list);
  auto* close_btn =
      new QPushButton(QStringLiteral("关闭"), &dlg);
  layout->addWidget(close_btn);
  QObject::connect(close_btn, &QPushButton::clicked, &dlg, &QDialog::accept);

  QEventLoop loop;
  auto conn = connect(
      &collab_engine_, &CollabEngine::announcement_history_received, &dlg,
      [&loop, &list, group_id](quint64 gid, const QString& history_json) {
        if (gid != group_id) return; // 其他群的回执继续等
        const auto arr = nlohmann::json::parse(
            history_json.toStdString(), nullptr, false);
        if (arr.is_discarded() || !arr.is_array()) {
          auto* item = new QListWidgetItem(QStringLiteral("历史解析失败"), list);
          item->setForeground(Qt::gray);
        } else if (arr.empty()) {
          new QListWidgetItem(QStringLiteral("暂无编辑记录"), list);
        } else {
          for (const auto& h : arr) {
            const QDateTime ts =
                QDateTime::fromMSecsSinceEpoch(h.value("ts_ms", 0));
            const QString content =
                QString::fromStdString(h.value("content", std::string{}));
            new QListWidgetItem(
                QStringLiteral("%1　%2　设为：%3")
                    .arg(ts.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                         QString::fromStdString(h.value("editor", std::string{})),
                         content.isEmpty() ? QStringLiteral("（清除公告）")
                                           : content),
                list);
          }
        }
        loop.quit();
      });
  collab_engine_.announce_history(group_id);
  // 超时兜底：服务端 3 秒无回执提示后关（连接异常等极端情形不吊死模态）
  QTimer::singleShot(3000, &loop, &QEventLoop::quit);
  loop.exec();
  QObject::disconnect(conn);
  dlg.exec();
}

// 免服务端临时群（直连态）：从已发现设备多选，发送＝逐设备点对点扇出。
// 不经服务端——不进归档（界面分组与提示条明示）。
void MainWindow::dgroup_dialog() {
  const auto peers = direct_engine_.peers();
  if (peers.isEmpty()) {
    show_status(QStringLiteral("未发现局域网设备，无法建临时群"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("新建临时群（点对点扇出，不进归档）"));
  dlg.resize(400, 440);
  auto* layout = new QVBoxLayout(&dlg);
  auto* list = new QListWidget(&dlg);
  for (const Peer& p : peers) {
    const QString id = QString::fromStdString(p.device_id);
    const QString name =
        p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    auto* item = new QListWidgetItem(
        QStringLiteral("%1\n%2 · TCP %3")
            .arg(name, p.address.toString(), QString::number(p.tcp_port)),
        list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Unchecked);
    item->setData(Qt::UserRole, id);
  }
  auto* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  layout->addWidget(list);
  layout->addWidget(buttons);
  if (dlg.exec() != QDialog::Accepted) return;
  QStringList picked;
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->checkState() == Qt::Checked) {
      picked << list->item(i)->data(Qt::UserRole).toString();
    }
  }
  if (picked.isEmpty()) {
    show_status(QStringLiteral("建临时群取消：未选择设备"));
    return;
  }
  const QString id = QStringLiteral("dgroup:%1").arg(next_dgroup_++);
  dgroup_members_.insert(id, picked);
  refresh_devices();
  open_chat(QStringLiteral("dgroup"), id);
  show_status(QStringLiteral("临时群已建立（%1 台设备）· 不进归档").arg(picked.size()));
}

QString MainWindow::dgroup_title(const QString& dgroup_id) const {
  return QStringLiteral("临时群 %1")
      .arg(dgroup_id.mid(QStringLiteral("dgroup:").size()));
}

} // namespace memex::client
