// R24-2 群备忘录窗口（实现，布局按 prototypes/desktop-group-memo.html：
// 左 430px 列表列（搜索/刷新/只看我建的），右内联编辑卡（标题+正文+动作行）
// ＋内联修订历史面板）。服务端裁决一切判权（写=群主/管理员或开放编辑时
// 全员，删恒归群主/管理员）——客户端只提交与展示，不自造规则。
#include "group_memo_dialog.hpp"
#include "collab_defaults.hpp"

#include <QCheckBox>
#include <QColor>
#include <QDateTime>
#include <QFrame>
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
// 列表条目角色：id / 标题 / 正文 / 作者（「只看我建的」过滤取值）
constexpr int kRoleId = Qt::UserRole + 2;
constexpr int kRoleTitle = Qt::UserRole + 3;
constexpr int kRoleContent = Qt::UserRole + 4;
constexpr int kRoleAuthor = Qt::UserRole + 5;

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

// 原型 conn-row/.stat 面板条：底色＋分隔线（样式挂容器不挂子件）
QFrame* panel_row(QWidget* parent, bool top_line) {
  auto* f = new QFrame(parent);
  f->setFrameShape(QFrame::NoFrame);
  f->setStyleSheet(top_line
                       ? QStringLiteral("QFrame{background:palette(base);"
                                        "border-top:1px solid palette(mid);}")
                       : QStringLiteral("QFrame{background:palette(base);"
                                        "border-bottom:1px solid palette(mid);}"));
  return f;
}

// 原型 10.5~13px 区块标题/说明字号
QString head_style() {
  return QStringLiteral("font-size:13px;font-weight:700;");
}
QString note_style() {
  return QStringLiteral("font-size:10.5px;font-weight:600;color:palette(mid);");
}
} // namespace

GroupMemoDialog::GroupMemoDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("群备忘录"));
  resize(960, 660);  // 原型 .dialog 960×660
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
            update_edit_note();
            stream_->clear();
            for (const auto& v : memos) {
              const QJsonObject o = v.toObject();
              const qint64 id = static_cast<qint64>(
                  o.value(QStringLiteral("id")).toDouble());
              const QString title =
                  o.value(QStringLiteral("title")).toString();
              const QString content =
                  o.value(QStringLiteral("content")).toString();
              const QString author =
                  o.value(QStringLiteral("author")).toString();
              // R24-4：疑似密码条目行内 warnhint（原型 .warnhint 口径）
              const QString warn = looks_like_secret(title) ||
                                           looks_like_secret(content)
                                       ? QStringLiteral(
                                             "\n⚠ 疑似包含凭据：密码请进群密码"
                                             "箱（备忘录为明文共享）")
                                       : QString();
              auto* it = new QListWidgetItem(stream_);
              it->setText(QStringLiteral("%1 · %2 · %3\n%4%5")
                              .arg(title, author,
                                   fmt_time(static_cast<qint64>(
                                       o.value(QStringLiteral("updated_ms"))
                                           .toDouble())),
                                   content, warn));
              it->setData(kRoleId, id);
              it->setData(kRoleTitle, title);
              it->setData(kRoleContent, content);
              it->setData(kRoleAuthor, author);
            }
            if (memos.isEmpty()) {
              auto* it = new QListWidgetItem(stream_);
              it->setText(QStringLiteral("（群备忘录暂无条目）"));
              it->setFlags(Qt::NoItemFlags);
            }
            apply_mine_filter();
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
    close_history();  // 原型：回滚落笔即收起历史面板
    set_status(QStringLiteral("已回滚（回滚即一次编辑，落了新修订笔）"), false);
    refresh();
  });
  connect(client_, &FilesClient::group_memo_open_edit_set, this,
          [this](bool open) {
            open_edit_->blockSignals(true);
            open_edit_->setChecked(open);
            open_edit_->blockSignals(false);
            update_edit_note();
            set_status(open ? QStringLiteral("已开放全员编辑")
                            : QStringLiteral("已收回编辑权（仅群主/管理员）"),
                       false);
          });
  // 内联历史面板：连接挂本窗口（面板常驻隐藏，展开即填充）
  connect(client_, &FilesClient::group_memo_history_fetched, this,
          [this](const QJsonArray& revisions) {
            if (!hist_panel_ || hist_panel_->isHidden()) return;
            populate_history(revisions);
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
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // —— conn-row（原型：服务器地址/文件端口/账号/口令/群号 + 连接）——
  auto* conn = panel_row(this, false);
  auto* conn_row = new QHBoxLayout(conn);
  conn_row->setContentsMargins(16, 9, 16, 9);
  conn_row->setSpacing(8);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const auto conn_label = [this](const QString& t) {
    auto* lb = new QLabel(t, this);
    lb->setStyleSheet(QStringLiteral("font-size:12px;"));
    return lb;
  };
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
  gid_chip_ = new QLabel(QStringLiteral("—"), this);
  gid_chip_->setStyleSheet(QStringLiteral(
      "font-weight:600;border:1px solid palette(mid);border-radius:6px;"
      "padding:3px 9px;font-size:12px;"));
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn_row->addWidget(conn_label(QStringLiteral("服务器地址")));
  conn_row->addWidget(host_);
  conn_row->addWidget(conn_label(QStringLiteral("文件端口")));
  conn_row->addWidget(port_);
  conn_row->addWidget(conn_label(QStringLiteral("账号")));
  conn_row->addWidget(account_);
  conn_row->addWidget(conn_label(QStringLiteral("口令")));
  conn_row->addWidget(password_);
  conn_row->addWidget(conn_label(QStringLiteral("群号")));
  conn_row->addWidget(gid_chip_);
  conn_row->addStretch(1);
  conn_row->addWidget(btn_connect_);
  root->addWidget(conn);

  // —— split：左列表列 430px ｜ 右编辑卡列 ——
  auto* split = new QHBoxLayout;
  split->setSpacing(0);
  root->addLayout(split, 1);

  // 左列（原型 .list-col：面板底＋右分隔线）
  auto* list_col = new QFrame(this);
  list_col->setFrameShape(QFrame::NoFrame);
  list_col->setStyleSheet(QStringLiteral(
      "QFrame{background:palette(base);border-right:1px solid palette(mid);}"));
  list_col->setFixedWidth(430);
  auto* list_lay = new QVBoxLayout(list_col);
  list_lay->setContentsMargins(0, 0, 0, 0);
  list_lay->setSpacing(0);
  split->addWidget(list_col);

  // bar：搜索 + 刷新 + 只看我建的 + 开放全员编辑
  auto* bar = new QHBoxLayout;
  bar->setContentsMargins(12, 9, 12, 9);
  bar->setSpacing(8);
  search_ = new QLineEdit(this);
  search_->setPlaceholderText(
      QStringLiteral("搜标题或正文…（回车搜索，清空＝全部）"));
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  mine_only_ = new QCheckBox(QStringLiteral("只看我建的"), this);
  open_edit_ = new QCheckBox(
      QStringLiteral("开放全员编辑（默认仅群主/管理员可写；删除不开放）"),
      this);
  open_edit_->setStyleSheet(note_style());
  bar->addWidget(search_, 1);
  bar->addWidget(btn_refresh_);
  bar->addWidget(mine_only_);
  list_lay->addLayout(bar);
  list_lay->addWidget(open_edit_, 0, Qt::AlignLeft | Qt::AlignVCenter);

  // 条目列表（原型 .melist：卡片行 + 行距）
  stream_ = new QListWidget(this);
  stream_->setWordWrap(true);
  stream_->setSpacing(4);
  list_lay->addWidget(stream_, 1);

  // 右列（原型 .edit-col：标题行 + 标题/正文卡 + 动作行 + 历史面板）
  auto* edit_col = new QVBoxLayout;
  edit_col->setContentsMargins(14, 12, 14, 12);
  edit_col->setSpacing(10);
  split->addLayout(edit_col, 1);

  auto* eh = new QHBoxLayout;
  card_title_ = new QLabel(QStringLiteral("备忘录条目"), this);
  card_title_->setStyleSheet(head_style());
  edit_head_note_ = new QLabel(this);
  edit_head_note_->setStyleSheet(note_style());
  eh->addWidget(card_title_);
  eh->addStretch(1);
  eh->addWidget(edit_head_note_);
  edit_col->addLayout(eh);

  auto* frow = new QHBoxLayout;
  auto* tlb = new QLabel(QStringLiteral("标题"), this);
  tlb->setFixedWidth(30);
  tlb->setStyleSheet(QStringLiteral("font-size:12px;color:palette(mid);"));
  title_edit_ = new QLineEdit(this);
  title_edit_->setPlaceholderText(QStringLiteral("标题（必填）"));
  frow->addWidget(tlb);
  frow->addWidget(title_edit_, 1);
  edit_col->addLayout(frow);

  content_edit_ = new QPlainTextEdit(this);
  content_edit_->setPlaceholderText(
      QStringLiteral("正文（必填；可放代码块、连接信息、值班表等共享知识）"));
  content_edit_->setMinimumHeight(120);
  edit_col->addWidget(content_edit_, 1);

  // 动作行（原型 .btnrow；「保存」为四键外的功能键：提交卡片现值）
  auto* btnrow = new QHBoxLayout;
  btnrow->setSpacing(8);
  btn_new_ = new QPushButton(QStringLiteral("新建条目"), this);
  btn_save_ = new QPushButton(QStringLiteral("保存"), this);
  btn_edit_ = new QPushButton(QStringLiteral("编辑"), this);
  btn_history_ = new QPushButton(QStringLiteral("历史与回滚…"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除"), this);
  btn_save_->setEnabled(false);
  btn_edit_->setEnabled(false);
  btn_history_->setEnabled(false);
  btn_delete_->setEnabled(false);
  btn_delete_->setToolTip(
      QStringLiteral("删除恒归群主/管理员（开放编辑开放的是写，不是删）"));
  btnrow->addWidget(btn_new_);
  btnrow->addWidget(btn_save_);
  btnrow->addWidget(btn_edit_);
  btnrow->addWidget(btn_history_);
  btnrow->addWidget(btn_delete_);
  btnrow->addStretch(1);
  edit_col->addLayout(btnrow);

  // 内联修订历史面板（原型 .hist：默认收起，选中条目展开）
  hist_panel_ = new QFrame(this);
  hist_panel_->setStyleSheet(QStringLiteral(
      "QFrame{background:palette(base);border:1px solid palette(mid);"
      "border-radius:10px;}"));
  hist_panel_->hide();
  auto* hist_lay = new QVBoxLayout(hist_panel_);
  hist_lay->setContentsMargins(0, 0, 0, 0);
  hist_lay->setSpacing(0);
  auto* hh = new QHBoxLayout;
  hh->setContentsMargins(12, 8, 12, 8);
  hist_head_ = new QLabel(QStringLiteral("条目修订历史"), hist_panel_);
  hist_head_->setStyleSheet(
      QStringLiteral("font-size:12px;font-weight:700;"));
  auto* hh_note = new QLabel(
      QStringLiteral("开放成员编辑时每笔修改留痕，可回滚"), hist_panel_);
  hh_note->setStyleSheet(note_style());
  hh->addWidget(hist_head_);
  hh->addSpacing(8);
  hh->addWidget(hh_note);
  hh->addStretch(1);
  hist_lay->addLayout(hh);
  hist_list_ = new QListWidget(hist_panel_);
  hist_list_->setWordWrap(true);
  hist_lay->addWidget(hist_list_, 1);
  auto* hist_btns = new QHBoxLayout;
  hist_btns->setContentsMargins(12, 8, 12, 8);
  hist_btns->setSpacing(8);
  btn_roll_ = new QPushButton(QStringLiteral("回滚到选中这笔"), hist_panel_);
  btn_roll_->setEnabled(false);
  auto* btn_hist_close = new QPushButton(QStringLiteral("关闭"), hist_panel_);
  hist_btns->addWidget(btn_roll_);
  hist_btns->addStretch(1);
  hist_btns->addWidget(btn_hist_close);
  hist_lay->addLayout(hist_btns);
  edit_col->addWidget(hist_panel_, 1);

  // 底注（原型 edit-col 尾注）
  auto* foot = new QLabel(
      QStringLiteral("群公告（R24-1）在本群会话置顶区维护，含 @全员 强提醒与"
                     "编辑历史（群右键「公告编辑历史…」）；管理员默认维护备忘录"
                     "，开放成员编辑后每笔修改留痕。"),
      this);
  foot->setWordWrap(true);
  foot->setStyleSheet(
      QStringLiteral("font-size:10.5px;color:palette(mid);"));
  edit_col->addWidget(foot);

  // —— 状态行（原型 .stat 底条）——
  auto* stat = panel_row(this, true);
  auto* stat_lay = new QHBoxLayout(stat);
  stat_lay->setContentsMargins(16, 6, 16, 6);
  status_ = new QLabel(
      QStringLiteral("未连接（与协作面同源账号；文件面端口独立）"), stat);
  status_->setWordWrap(true);
  status_->setStyleSheet(QStringLiteral("font-size:11.5px;"));
  stat_lay->addWidget(status_);
  root->addWidget(stat);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(resolve_conn_host(host_->text().trimmed()),
               static_cast<quint16>(port_->text().toUInt()),
               account_->text().trimmed(), password_->text());
  });
  connect(password_, &QLineEdit::returnPressed, this, [this] {
    if (btn_connect_->isEnabled()) btn_connect_->click();
  });
  connect(search_, &QLineEdit::returnPressed, this, [this] { refresh(); });
  connect(btn_refresh_, &QPushButton::clicked, this, [this] { refresh(); });
  connect(mine_only_, &QCheckBox::toggled, this, [this] { apply_mine_filter(); });
  connect(open_edit_, &QCheckBox::toggled, this, [this](bool on) {
    if (client_->is_logged_in()) client_->set_group_memo_open_edit(gid_, on);
  });

  connect(stream_, &QListWidget::itemSelectionChanged, this, [this] {
    const auto* it = stream_->currentItem();
    const bool has = it != nullptr && it->flags() != Qt::NoItemFlags;
    btn_edit_->setEnabled(has);
    btn_history_->setEnabled(has);
    btn_delete_->setEnabled(has);
    if (has) {
      // 选中即进卡片（查看态；「编辑」才落编辑目标）
      end_edit();
      title_edit_->setText(it->data(kRoleTitle).toString());
      content_edit_->setPlainText(it->data(kRoleContent).toString());
      card_title_->setText(
          QStringLiteral("备忘录条目 · %1").arg(it->data(kRoleTitle).toString()));
    }
    update_save_enabled();
    update_edit_note();
  });
  connect(title_edit_, &QLineEdit::textChanged, this,
          [this] { update_save_enabled(); });
  connect(content_edit_, &QPlainTextEdit::textChanged, this,
          [this] { update_save_enabled(); });

  // 新建：清卡进新条目态（保存即新建，编辑目标归零）
  connect(btn_new_, &QPushButton::clicked, this, [this] {
    end_edit();
    title_edit_->clear();
    content_edit_->clear();
    card_title_->setText(QStringLiteral("备忘录条目 · 新建"));
    title_edit_->setFocus();
    update_save_enabled();
    update_edit_note();
  });
  // 保存：提交卡片现值（submit_entry 程序化入口同源；手工路径先过
  // R24-4 疑似密码二次确认——程序化入口只挂提示不吊测试）
  connect(btn_save_, &QPushButton::clicked, this, [this] {
    const QString t = title_edit_->text();
    const QString c = content_edit_->toPlainText();
    if (confirm_secret_memo(t, c)) submit_entry(t, c);
  });
  // 编辑：选中条目进编辑态（再进一次＝取消，与测试入口同源）
  connect(btn_edit_, &QPushButton::clicked, this, [this] { edit_selected(); });
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
  connect(hist_list_, &QListWidget::itemSelectionChanged, this, [this] {
    btn_roll_->setEnabled(hist_list_->currentItem() != nullptr);
  });
  connect(btn_roll_, &QPushButton::clicked, this,
          [this] { rollback_selected_rev(); });
  connect(btn_hist_close, &QPushButton::clicked, this, [this] {
    close_history();
  });
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
  gid_chip_->setText(QString::number(gid));
  end_edit();
  close_history();
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
    set_status(QStringLiteral("已取消编辑"), false);
    return true;
  }
  editing_id_ = it->data(kRoleId).toLongLong();
  title_edit_->setText(it->data(kRoleTitle).toString());
  content_edit_->setPlainText(it->data(kRoleContent).toString());
  update_edit_note(QStringLiteral("条目 #%1").arg(editing_id_));
  set_status(QStringLiteral("正在编辑条目 #%1（改完「保存」；再点编辑＝取消）")
                 .arg(editing_id_),
             false);
  update_save_enabled();
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
  hist_head_->setText(QStringLiteral("条目修订历史（#%1）").arg(hist_memo_id_));
  hist_list_->clear();
  btn_roll_->setEnabled(false);
  hist_panel_->show();
  client_->group_memo_history(hist_memo_id_);
}

void GroupMemoDialog::populate_history(const QJsonArray& revisions) {
  hist_list_->clear();
  for (const auto& v : revisions) {
    const QJsonObject o = v.toObject();
    const QString content = o.value(QStringLiteral("content")).toString();
    auto* item = new QListWidgetItem(hist_list_);
    item->setText(QStringLiteral("%1　%2　《%3》\n%4")
                      .arg(QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(
                               o.value(QStringLiteral("ts_ms")).toDouble()))
                               .toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                           o.value(QStringLiteral("editor")).toString(),
                           o.value(QStringLiteral("title")).toString(),
                           content));
    item->setData(kRoleId,
                  static_cast<qint64>(
                      o.value(QStringLiteral("id")).toDouble()));
  }
  if (revisions.isEmpty()) {
    auto* item = new QListWidgetItem(hist_list_);
    item->setText(QStringLiteral("暂无修订记录"));
    item->setFlags(Qt::NoItemFlags);
  }
}

void GroupMemoDialog::close_history() {
  if (!hist_panel_) return;
  hist_panel_->hide();
  hist_list_->clear();
  btn_roll_->setEnabled(false);
  hist_memo_id_ = 0;
}

void GroupMemoDialog::rollback_selected_rev() {
  const auto* cur = hist_list_ ? hist_list_->currentItem() : nullptr;
  if (!cur || hist_memo_id_ == 0) return;
  client_->rollback_group_memo(hist_memo_id_,
                               cur->data(kRoleId).toLongLong());
  // 回滚成功信号（group_memo_rolled_back）收起面板并刷新列表
}

QString GroupMemoDialog::status_text() const { return status_->text(); }

bool GroupMemoDialog::open_edit_checked() const {
  return open_edit_->isChecked();
}

int GroupMemoDialog::stream_count() const { return stream_->count(); }

int GroupMemoDialog::history_count() const {
  // 未展开/已收起＝-1（isHidden 与父窗可见性无关，离屏测试可判）
  return hist_list_ && !hist_panel_->isHidden() ? hist_list_->count() : -1;
}

void GroupMemoDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : stream_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

void GroupMemoDialog::end_edit() { editing_id_ = 0; }

// 「只看我建的」：客户端过滤（作者≠当前账号的行隐藏，不重发请求）
void GroupMemoDialog::apply_mine_filter() {
  const bool mine = mine_only_->isChecked();
  for (int i = 0; i < stream_->count(); ++i) {
    auto* it = stream_->item(i);
    const bool placeholder = it->flags() == Qt::NoItemFlags;
    stream_->setRowHidden(
        i, mine && !placeholder && it->data(kRoleAuthor).toString() !=
                                       client_->account());
  }
}

// 「保存」可用：卡片两字段非空（连接与否交服务端 401 兜底提示）
void GroupMemoDialog::update_save_enabled() {
  btn_save_->setEnabled(!title_edit_->text().trimmed().isEmpty() &&
                        !content_edit_->toPlainText().trimmed().isEmpty());
}

// 卡片头注（原型 .eh 右注）：条目 #N / 新建 · 开放成员编辑（已开/未开）
void GroupMemoDialog::update_edit_note(const QString& override_text) {
  if (!edit_head_note_) return;
  QString base = override_text;
  if (base.isEmpty()) {
    base = editing_id_ > 0
               ? QStringLiteral("条目 #%1").arg(editing_id_)
               : QStringLiteral("条目 #—");
  }
  edit_head_note_->setText(
      QStringLiteral("%1 · 开放成员编辑（%2）")
          .arg(base,
               open_edit_->isChecked() ? QStringLiteral("已开")
                                       : QStringLiteral("未开")));
}

} // namespace memex::client
