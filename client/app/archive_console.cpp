// 管理台·归档检索与导出（原型 desktop-search.html）。
// 颜色全走主题令牌（ThemeManager），不留字面量；文案照原型，近似项见
// 台账（人员显示名/FILE 传输元数据行/分页等——如实不造假）。
#include "archive_console.hpp"

#include <QCheckBox>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStringConverter>
#include <QTextStream>
#include <QTime>
#include <QVBoxLayout>

#include "collab_defaults.hpp"
#include "engine/collab/files_client.hpp"
#include "theme.hpp"

namespace memex::client {

namespace {
constexpr int kSpan = 5;  // 查看上下文：命中行前后各 5 行
}  // namespace

ArchiveConsole::ArchiveConsole(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Memex 管理台 · 归档检索与导出"));
  resize(980, 700);
  setMinimumSize(880, 560);
  client_ = new FilesClient(this);
  build_ui();
  restyle();

  // 主题切换重染（与主窗同法：令牌全量重跑，含表头/胶囊/命中行富文本）
  connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
          &ArchiveConsole::restyle);

  connect(client_, &FilesClient::logged_in, this, [this] {
    conn_row_->hide();
    badge_->setText(QStringLiteral("● 协作服务 %1").arg(client_->account()));
    btn_search_->setEnabled(true);
    btn_export_->setEnabled(true);
    set_status(QStringLiteral("已连接（%1）——检索与查阅日志均须 auditor 有效角色")
                   .arg(client_->account()));
    client_->fetch_audit_reads();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::audit_searched, this,
          [this](const QJsonArray& messages) {
            populate_results(messages);
            client_->fetch_audit_reads();  // 每次检索落一条——顺带对齐台账
          });
  connect(client_, &FilesClient::audit_reads_listed, this,
          &ArchiveConsole::populate_reads);
  connect(client_, &FilesClient::audit_context_fetched, this,
          [this](const QString& msg_id, const QJsonArray& messages) {
            // 上下文弹窗：会话窗口（升序），命中行品牌色加粗
            auto* view = new QDialog(this);
            view->setObjectName(QStringLiteral("context_view"));
            view->setAttribute(Qt::WA_DeleteOnClose);
            view->setWindowTitle(
                QStringLiteral("查看上下文 · %1…").arg(msg_id.left(8)));
            view->resize(560, 360);
            auto* lay = new QVBoxLayout(view);
            auto* head = new QLabel(
                QStringLiteral("该消息所在会话（前后各 %1 行，升序）：")
                    .arg(kSpan),
                view);
            lay->addWidget(head);
            for (const auto& v : messages) {
              const auto m = v.toObject();
              const QString when = QDateTime::fromMSecsSinceEpoch(
                                       static_cast<qint64>(
                                           m.value(QStringLiteral("ts_ms"))
                                               .toDouble()))
                                       .toString(
                                           QStringLiteral("MM-dd HH:mm:ss"));
              const bool hit =
                  m.value(QStringLiteral("msg_id")).toString() == msg_id;
              const bool recalled =
                  m.value(QStringLiteral("recalled")).toBool();
              auto* line = new QLabel(
                  QStringLiteral("%1  %2 → %3%4  %5")
                      .arg(when, m.value(QStringLiteral("from")).toString(),
                           m.value(QStringLiteral("to")).toString(),
                           recalled
                               ? QStringLiteral("（已撤回 · 原文留存）")
                               : QString(),
                           m.value(QStringLiteral("text")).toString()),
                  view);
              line->setWordWrap(true);
              lay->addWidget(line);
              if (hit) {
                const auto& t = ThemeManager::instance().tokens();
                line->setStyleSheet(
                    QStringLiteral("font-weight:700; color:%1;")
                        .arg(t.brand_text.name()));
              }
            }
            view->show();
          });
  connect(client_, &FilesClient::audit_recall_event_fetched, this,
          [this](const QString& msg_id, bool exists, const QString& by,
                 qint64 ts_ms) {
            auto* view = new QDialog(this);
            view->setObjectName(QStringLiteral("recall_view"));
            view->setAttribute(Qt::WA_DeleteOnClose);
            view->setWindowTitle(
                QStringLiteral("撤回事件 · %1…").arg(msg_id.left(8)));
            auto* lay = new QVBoxLayout(view);
            const QString when =
                ts_ms > 0
                    ? QDateTime::fromMSecsSinceEpoch(ts_ms).toString(
                          QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                    : QString();
            auto* line = new QLabel(
                exists
                    ? QStringLiteral("撤回人：%1\n撤回时刻：%2\n（原文保留在"
                                     "归档，标记「已撤回 · 原文留存」）")
                          .arg(by.isEmpty()
                                   ? QStringLiteral("（未记）")
                                   : by,
                               when)
                    : QStringLiteral("该消息未撤回（无撤回事件）"),
                view);
            lay->addWidget(line);
            view->show();
          });
  connect(client_, &FilesClient::audit_exported, this,
          [this](const QJsonArray& messages, const QString& op,
                 qint64 exported_ms) {
            // 证据文件本机写盘（路径在操作者手里；服务端只回行＋留痕）
            if (last_export_path_.isEmpty()) return;
            QFile out(last_export_path_);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
              set_status(
                  QStringLiteral("证据文件无法写入：%1").arg(last_export_path_),
                  true);
              return;
            }
            const QString filters =
                QStringLiteral("账号=%1 关键词=%2")
                    .arg(account_->text().trimmed(),
                         keyword_->text().trimmed());
            QTextStream ts(&out);
            ts.setEncoding(QStringConverter::Utf8);
            ts << "# Memex 归档导出（证据包）\n"
               << "# 导出时间(ms)：" << exported_ms << "\n"
               << "# 操作者：" << op << "\n"
               << "# 过滤条件："
               << (filters.trimmed().isEmpty()
                       ? QStringLiteral("（全部）")
                       : filters)
               << "\n";
            for (const auto& v : messages) {
              const auto m = v.toObject();
              ts << m.value(QStringLiteral("msg_id")).toString() << '\t'
                 << m.value(QStringLiteral("from")).toString() << '\t'
                 << m.value(QStringLiteral("to")).toString() << '\t'
                 << m.value(QStringLiteral("type")).toString() << '\t'
                 << (m.value(QStringLiteral("recalled")).toBool()
                         ? QStringLiteral("已撤回")
                         : QStringLiteral("正常"))
                 << '\t'
                 << QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(
                                                   m.value(QStringLiteral(
                                                               "ts_ms"))
                                                               .toDouble()))
                        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                 << '\t' << m.value(QStringLiteral("text")).toString()
                 << '\n';
            }
            ts << "共 " << messages.size() << " 条\n";
            out.close();
            evidence_->setText(
                QStringLiteral("导出留证（证据包已生成：%1 · %2 条 · 含时间"
                               "戳与操作人 %3）")
                    .arg(last_export_path_)
                        .arg(messages.size())
                        .arg(op));
            set_status(QStringLiteral("已导出 %1 条至 %2").arg(
                QString::number(messages.size()), last_export_path_));
            client_->fetch_audit_reads();  // audit.message.export 已落——对齐
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

QString ArchiveConsole::status_text() const { return status_->text(); }

void ArchiveConsole::build_ui() {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 12, 16, 12);
  root->setSpacing(8);

  // 头行：审计岗胶囊＋连接状态徽章（原型 titlebar 释义位）
  auto* head = new QHBoxLayout;
  badge_ = new QLabel(QStringLiteral("○ 未连接"), this);
  badge_->setObjectName(QStringLiteral("conn_badge"));
  head->addWidget(badge_);
  head->addStretch();
  auto* role = new QLabel(QStringLiteral("归档与检索 · 审计岗"), this);
  role->setObjectName(QStringLiteral("role_pill"));
  head->addWidget(role);
  root->addLayout(head);

  // 连接行（未连接时呈现；连上即收起——徽章接管状态位）
  conn_row_ = new QWidget(this);
  conn_row_->setObjectName(QStringLiteral("conn_row"));
  auto* conn = new QHBoxLayout(conn_row_);
  conn->setContentsMargins(0, 0, 0, 0);
  host_ = new QLineEdit(conn_row_);
  host_->setPlaceholderText(QStringLiteral("服务器地址"));
  port_ = new QLineEdit(conn_row_);
  port_->setPlaceholderText(QStringLiteral("文件端口"));
  port_->setMaximumWidth(90);
  conn_account_ = new QLineEdit(conn_row_);
  conn_account_->setPlaceholderText(QStringLiteral("账号"));
  conn_account_->setMaximumWidth(110);
  conn_password_ = new QLineEdit(conn_row_);
  conn_password_->setPlaceholderText(QStringLiteral("口令"));
  conn_password_->setEchoMode(QLineEdit::Password);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), conn_row_);
  conn->addWidget(host_);
  conn->addWidget(port_);
  conn->addWidget(conn_account_);
  conn->addWidget(conn_password_);
  conn->addWidget(btn_connect_);
  root->addWidget(conn_row_);
  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(resolve_conn_host(host_->text()), port_->text().toUShort(),
               conn_account_->text(), conn_password_->text());
  });

  // 标题＋口径副行（原型 console-title/console-sub 文案照抄）
  auto* title = new QLabel(QStringLiteral("归档检索与导出"), this);
  title->setObjectName(QStringLiteral("console_title"));
  root->addWidget(title);
  auto* sub = new QLabel(
      QStringLiteral("按人员、时间、关键词检索协作态归档。撤回消息保留原文，"
                     "并单独可查撤回事件。每次查阅行为自动记入查阅日志。"),
      this);
  sub->setWordWrap(true);
  sub->setObjectName(QStringLiteral("console_sub"));
  root->addWidget(sub);

  // 过滤行（原型 filters：人员胶囊／时间胶囊／关键词宽胶囊＋精确匹配）
  auto* filters = new QHBoxLayout;
  account_ = new QLineEdit(this);
  account_->setPlaceholderText(QStringLiteral("人员账号（可空＝全部）"));
  account_->setMaximumWidth(170);
  filters->addWidget(account_);
  use_window_ = new QCheckBox(QStringLiteral("时间窗"), this);
  filters->addWidget(use_window_);
  since_ = new QDateEdit(QDate::currentDate().addDays(-7), this);
  since_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  since_->setCalendarPopup(true);
  until_ = new QDateEdit(QDate::currentDate(), this);
  until_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  until_->setCalendarPopup(true);
  filters->addWidget(since_);
  filters->addWidget(new QLabel(QStringLiteral("至"), this));
  filters->addWidget(until_);
  keyword_ = new QLineEdit(this);
  keyword_->setPlaceholderText(
      QStringLiteral("关键词（可空＝全部；多词空格分隔，全部命中才出）"));
  filters->addWidget(keyword_, 1);
  // 精确匹配胶囊＝服务端真实语义说明（子串精确命中，无模糊扩展）——
  // 静态说明非开关，样式照原型 pill-outline
  auto* exact = new QLabel(QStringLiteral("精确匹配"), this);
  exact->setObjectName(QStringLiteral("exact_pill"));
  filters->addWidget(exact);
  btn_search_ = new QPushButton(QStringLiteral("检索"), this);
  btn_search_->setObjectName(QStringLiteral("btn_search"));
  btn_search_->setEnabled(false);
  filters->addWidget(btn_search_);
  root->addLayout(filters);
  connect(btn_search_, &QPushButton::clicked, this, [this] {
    qint64 since_ms = 0;
    qint64 until_ms = 0;
    if (use_window_->isChecked()) {
      since_ms =
          QDateTime(since_->date(), QTime(0, 0)).toMSecsSinceEpoch();
      until_ms = QDateTime(until_->date(), QTime(23, 59, 59))
                     .toMSecsSinceEpoch();
    }
    run_search(account_->text().trimmed(), keyword_->text().trimmed(),
               since_ms, until_ms);
  });

  // 命中表（原型 result-table）：表头五列＋滚动行区＋页脚
  auto* table = new QFrame(this);
  table->setObjectName(QStringLiteral("result_table"));
  auto* tl = new QVBoxLayout(table);
  tl->setContentsMargins(0, 0, 0, 0);
  tl->setSpacing(0);
  auto* header = new QWidget(table);
  auto* hl = new QHBoxLayout(header);
  hl->setContentsMargins(12, 7, 12, 7);
  auto* h_key = new QLabel(QStringLiteral("命中文本"), header);
  auto* h_pair = new QLabel(QStringLiteral("会话 / 对端"), header);
  auto* h_time = new QLabel(QStringLiteral("归档时间"), header);
  auto* h_flag = new QLabel(QStringLiteral("标记"), header);
  auto* h_ops = new QLabel(QStringLiteral("操作"), header);
  h_key->setFixedWidth(320);
  h_pair->setFixedWidth(150);
  h_time->setFixedWidth(110);
  h_flag->setFixedWidth(120);
  hl->addWidget(h_key);
  hl->addWidget(h_pair);
  hl->addWidget(h_time);
  hl->addWidget(h_flag);
  hl->addWidget(h_ops);
  header->setObjectName(QStringLiteral("table_header"));
  tl->addWidget(header);

  auto* scroll = new QScrollArea(table);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  rows_host_ = new QWidget(scroll);
  rows_host_->setObjectName(QStringLiteral("hit_rows"));
  rows_lay_ = new QVBoxLayout(rows_host_);
  rows_lay_->setContentsMargins(0, 0, 0, 0);
  rows_lay_->setSpacing(0);
  rows_lay_->addStretch();
  scroll->setWidget(rows_host_);
  tl->addWidget(scroll, 1);

  auto* foot = new QWidget(table);
  auto* fl = new QHBoxLayout(foot);
  fl->setContentsMargins(12, 7, 12, 7);
  foot_count_ = new QLabel(
      QStringLiteral("共命中 0 条（未检索；连接后按条件检索协作态归档）"),
      foot);
  foot_count_->setObjectName(QStringLiteral("foot_count"));
  fl->addWidget(foot_count_);
  fl->addStretch();
  evidence_ = new QLabel(foot);  // 导出留证行（证据包路径＋条数＋操作人）
  evidence_->setObjectName(QStringLiteral("evidence_line"));
  fl->addWidget(evidence_);
  btn_export_ = new QPushButton(QStringLiteral("导出留证"), foot);
  btn_export_->setObjectName(QStringLiteral("btn_export"));
  btn_export_->setEnabled(false);
  fl->addWidget(btn_export_);
  tl->addWidget(foot);
  root->addWidget(table, 1);
  connect(btn_export_, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出留证"),
        QStringLiteral("memex-audit-export.txt"),
        QStringLiteral("Memex 归档导出 (*.txt)"));
    if (!path.isEmpty()) export_to(path);
  });

  // 查阅日志（原型 audit-log：本页操作自动记录）
  auto* log_title = new QLabel(
      QStringLiteral("查阅日志（本页操作自动记录：检索/上下文/撤回事件/"
                     "导出各落一条，被拒同样留痕）"),
      this);
  log_title->setObjectName(QStringLiteral("log_title"));
  root->addWidget(log_title);
  reads_list_ = new QListWidget(this);
  reads_list_->setAlternatingRowColors(true);
  reads_list_->setMaximumHeight(120);
  root->addWidget(reads_list_);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  root->addWidget(status_);
  set_status(QStringLiteral("未连接——先在上方填写文件面地址与账号连接"));
}

void ArchiveConsole::restyle() {
  const auto& t = ThemeManager::instance().tokens();
  const auto pill = [&](const QString& name, int radius) {
    if (auto* l = findChild<QLabel*>(name)) {
      l->setStyleSheet(QStringLiteral(
                           "font-size:11px; font-weight:600; color:%1;"
                           " border:1px solid %2; border-radius:%3px;"
                           " padding:2px 9px;")
                           .arg(t.text_muted.name(), t.border.name())
                           .arg(radius));
    }
  };
  badge_->setStyleSheet(
      QStringLiteral("font-size:12px; font-weight:600; color:%1;")
          .arg(t.text_muted.name()));
  pill(QStringLiteral("role_pill"), 10);
  pill(QStringLiteral("exact_pill"), 9);
  if (auto* l = findChild<QLabel*>(QStringLiteral("console_title"))) {
    l->setStyleSheet(
        QStringLiteral("font-size:16px; font-weight:800; color:%1;")
            .arg(t.text.name()));
  }
  if (auto* l = findChild<QLabel*>(QStringLiteral("console_sub"))) {
    l->setStyleSheet(QStringLiteral("font-size:11.5px; color:%1;")
                         .arg(t.text_muted.name()));
  }
  if (auto* l = findChild<QLabel*>(QStringLiteral("log_title"))) {
    l->setStyleSheet(
        QStringLiteral("font-size:12.5px; font-weight:700; color:%1;")
            .arg(t.text.name()));
  }
  btn_search_->setStyleSheet(
      QStringLiteral("QPushButton { background:%1; color:%2; border:none;"
                     " border-radius:6px; padding:6px 18px; font-weight:700; }"
                     " QPushButton:disabled { background:%3; color:%2; }")
          .arg(t.brand.name(), t.on_brand.name(), t.disabled_bg.name()));
  btn_export_->setStyleSheet(
      QStringLiteral("QPushButton { color:%1; border:1px solid %1;"
                     " border-radius:6px; padding:5px 14px; }"
                     " QPushButton:disabled { color:%2; border-color:%2; }")
          .arg(t.brand_text.name(), t.disabled_bg.name()));
  if (auto* table_frame =
          findChild<QFrame*>(QStringLiteral("result_table"))) {
    table_frame->setStyleSheet(
        QStringLiteral("QFrame#result_table { background:%1; border:1px"
                       " solid %2; border-radius:10px; }"
                       " QWidget#table_header { background:%3;"
                       " border-bottom:1px solid %2; }"
                       " QWidget#table_header QLabel { font-size:11px;"
                       " font-weight:700; color:%4; border:none; }"
                       " QWidget#foot_count { border:none; }")
            .arg(t.surface_raised.name(), t.border.name(),
                 t.surface_alt.name(), t.text_muted.name()));
  }
  foot_count_->setStyleSheet(
      QStringLiteral("font-size:11px; color:%1; border:none;")
          .arg(t.text_muted.name()));
  evidence_->setStyleSheet(
      QStringLiteral("font-size:11px; color:%1; border:none;")
          .arg(t.text_muted.name()));
  // 命中行富文本高亮色在造行时烙进 HTML——重染时用缓存回包重建行，
  // 高亮随新令牌重算（results_ 空则只清不建，行为不变）
  if (!results_.isEmpty()) populate_results(results_);
}

void ArchiveConsole::connect_to(const QString& host, quint16 files_port,
                                const QString& acc, const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"),
                    QString::number(files_port));
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool ArchiveConsole::is_connected() const { return client_->is_logged_in(); }

bool ArchiveConsole::run_search(const QString& account, const QString& keyword,
                                qint64 since_ms, qint64 until_ms) {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  // 过滤条件写入 UI 位（导出留证按「最近一次检索同口径」重跑；程序化
  // 入口与界面编辑殊途——导出读的是界面过滤位，须同步）
  account_->setText(account);
  keyword_->setText(keyword);
  const bool has_window = since_ms > 0 || until_ms > 0;
  use_window_->setChecked(has_window);
  if (has_window) {
    since_->setDate(QDateTime::fromMSecsSinceEpoch(since_ms).date());
    until_->setDate(QDateTime::fromMSecsSinceEpoch(until_ms).date());
  }
  client_->audit_search(account, keyword, since_ms, until_ms);
  return true;
}

void ArchiveConsole::open_context(const QString& msg_id) {
  if (!is_connected() || msg_id.isEmpty()) return;
  client_->audit_context(msg_id, kSpan);
}

void ArchiveConsole::open_recall_event(const QString& msg_id) {
  if (!is_connected() || msg_id.isEmpty()) return;
  client_->audit_recall_event(msg_id);
}

void ArchiveConsole::export_to(const QString& path) {
  if (!is_connected() || path.isEmpty()) return;
  last_export_path_ = path;
  // 服务端按当前过滤条件重跑（与最近一次检索同口径）＋落 export 留痕
  qint64 since_ms = 0;
  qint64 until_ms = 0;
  if (use_window_->isChecked()) {
    since_ms = QDateTime(since_->date(), QTime(0, 0)).toMSecsSinceEpoch();
    until_ms =
        QDateTime(until_->date(), QTime(23, 59, 59)).toMSecsSinceEpoch();
  }
  client_->audit_export(account_->text().trimmed(), keyword_->text().trimmed(),
                        since_ms, until_ms);
}

void ArchiveConsole::populate_results(const QJsonArray& messages) {
  results_ = messages;
  // 全量重建（Qt 清空重建三招：旧件 delete 前先从布局摘除）
  while (rows_lay_->count() > 1) {  // 尾部 stretch 保留
    QLayoutItem* it = rows_lay_->takeAt(0);
    if (QWidget* w = it->widget()) w->deleteLater();
    delete it;
  }
  const auto& t = ThemeManager::instance().tokens();
  for (const auto& v : messages) {
    const auto m = v.toObject();
    const QString msg_id = m.value(QStringLiteral("msg_id")).toString();
    const bool recalled = m.value(QStringLiteral("recalled")).toBool();
    auto* row = new QFrame(rows_host_);
    row->setObjectName(QStringLiteral("hit_row"));
    row->setStyleSheet(
        QStringLiteral("QFrame#hit_row { background:transparent;"
                       " border:none; border-bottom:1px solid %1; }")
            .arg(t.divider.name()));
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(12, 6, 12, 6);
    auto* key = new QLabel(highlight(m.value(QStringLiteral("text")).toString()),
                           row);
    key->setWordWrap(true);
    key->setTextFormat(Qt::RichText);
    key->setFixedWidth(320);
    auto* pair =
        new QLabel(row_pair(m), row);
    pair->setWordWrap(true);
    pair->setFixedWidth(150);
    const QString when = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(
                                          m.value(QStringLiteral("ts_ms"))
                                              .toDouble()))
                              .toString(QStringLiteral("MM-dd HH:mm:ss"));
    auto* time = new QLabel(when, row);
    time->setFixedWidth(110);
    auto* flag = new QLabel(
        recalled ? QStringLiteral("已撤回 · 原文留存") : QStringLiteral("—"),
        row);
    flag->setObjectName(QStringLiteral("hit_flag"));
    flag->setFixedWidth(120);
    if (recalled) {
      flag->setStyleSheet(
          QStringLiteral("font-size:10.5px; font-weight:600; color:%1;"
                         " background:%2; border-radius:8px; padding:2px 8px;")
              .arg(t.warning.name(), t.warning_wash.name()));
    } else {
      flag->setStyleSheet(
          QStringLiteral("font-size:11px; color:%1;").arg(t.text_muted.name()));
    }
    auto* ops = new QWidget(row);
    auto* ol = new QHBoxLayout(ops);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->setSpacing(8);
    auto* b_ctx = new QPushButton(QStringLiteral("查看上下文"), ops);
    b_ctx->setFlat(true);
    b_ctx->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px; padding:0;")
            .arg(t.brand_text.name()));
    connect(b_ctx, &QPushButton::clicked, this,
            [this, msg_id] { open_context(msg_id); });
    ol->addWidget(b_ctx);
    if (recalled) {
      auto* b_ev = new QPushButton(QStringLiteral("撤回事件"), ops);
      b_ev->setFlat(true);
      b_ev->setStyleSheet(
          QStringLiteral("color:%1; font-size:11px; padding:0;")
              .arg(t.brand_text.name()));
      connect(b_ev, &QPushButton::clicked, this,
              [this, msg_id] { open_recall_event(msg_id); });
      ol->addWidget(b_ev);
    }
    ops->setLayout(ol);
    rl->addWidget(key);
    rl->addWidget(pair);
    rl->addWidget(time);
    rl->addWidget(flag);
    rl->addWidget(ops);
    row->show();
    rows_lay_->insertWidget(rows_lay_->count() - 1, row);
  }
  foot_count_->setText(
      QStringLiteral("共命中 %1 条 · 显示 1–%2（服务端单页上限 200）")
          .arg(messages.size())
          .arg(messages.size()));
  if (messages.isEmpty()) {
    set_status(QStringLiteral("共命中 0 条——换条件或放宽时间窗再检索"));
  }
}

void ArchiveConsole::populate_reads(const QJsonArray& reads) {
  reads_list_->clear();
  for (const auto& v : reads) {
    const auto r = v.toObject();
    const QString when = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(
                                          r.value(QStringLiteral("ts_ms"))
                                              .toDouble()))
                              .toString(QStringLiteral("MM-dd HH:mm:ss"));
    auto* item = new QListWidgetItem(QString(), reads_list_);
    item->setText(QStringLiteral("%1  %2  %3  %4%5")
                      .arg(when,
                           r.value(QStringLiteral("op_account")).toString(),
                           r.value(QStringLiteral("action")).toString(),
                           r.value(QStringLiteral("filters")).toString(),
                           r.value(QStringLiteral("action")).toString() ==
                                   QStringLiteral("audit.message.search")
                               ? QStringLiteral("  命中%1条").arg(
                                     r.value(QStringLiteral("result_count"))
                                         .toInt())
                               : QString()));
  }
}

QString ArchiveConsole::highlight(const QString& text) const {
  const QString escaped = text.toHtmlEscaped();
  // 服务端分词口径：空白＋半角/全角逗号；ASCII 大小写不敏感
  QStringList toks;
  QString cur;
  for (const QChar& c : keyword_->text()) {
    if (c == ' ' || c == '\t' || c == ',' || c == QChar(0xFF0C)) {
      if (!cur.isEmpty()) toks << cur;
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.isEmpty()) toks << cur;
  if (toks.isEmpty()) return escaped;
  // 逐词标亮（大小写不敏感扫描；词表已 HTML 转义口径一致后替换）
  const auto& t = ThemeManager::instance().tokens();
  QString out = escaped;
  for (const QString& raw : toks) {
    const QString tok = raw.toHtmlEscaped();
    if (tok.isEmpty()) continue;
    const QString needle = tok.toLower();
    QString built;
    qsizetype i = 0;
    while (i < out.size()) {
      const qsizetype hit =
          out.mid(i).toLower().indexOf(needle);
      if (hit < 0) {
        built += out.mid(i);
        break;
      }
      built += out.mid(i, hit);
      built += QStringLiteral("<span style=\"color:%1; font-weight:700\">%2</span>")
                   .arg(t.brand_text.name(), out.mid(i + hit, tok.size()));
      i += hit + tok.size();
    }
    out = built;
  }
  return out;
}

QString ArchiveConsole::row_pair(const QJsonObject& m) const {
  const QString from = m.value(QStringLiteral("from")).toString();
  const QString to = m.value(QStringLiteral("to")).toString();
  if (to.startsWith(QStringLiteral("group:"))) {
    return QStringLiteral("%1 → %2（群）").arg(from, to);
  }
  return QStringLiteral("%1 ⇄ %2（单聊）").arg(from, to);
}

void ArchiveConsole::set_status(const QString& text, bool error) {
  if (status_ == nullptr) return;
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  const auto& t = ThemeManager::instance().tokens();
  status_->setStyleSheet(
      error
          ? QStringLiteral("font-size:11.5px; color:%1;").arg(t.danger.name())
          : QStringLiteral("font-size:11.5px; color:%1;")
                .arg(t.text_muted.name()));
}

int ArchiveConsole::result_count() const { return results_.size(); }

QString ArchiveConsole::evidence_text() const {
  return evidence_ == nullptr ? QString() : evidence_->text();
}

}  // namespace memex::client
