// 邮箱·统一收件箱视图（原型 desktop-mailbox.html）——结构与文案照原型：
// console-top（标题「邮箱」＋副题＋tx-tabs 计数＋只看未读＋搜索＋全部标记
// 已读）、五列表（条目/来源/时间/状态/操作）、rt-foot 未读脚注、
// audit-log 边界注、infopanel 条目详情＋已读确认台账＋告警处理留痕。
// 颜色全走主题令牌（ThemeManager），不留字面量；近似项见
// docs/design/prototype-conformance.md desktop-mailbox 行。
#include "mailbox_view.hpp"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <functional>

#include <engine/collab/files_client.hpp>

#include "theme.hpp"

namespace memex::client {
namespace {

// 原型列宽（.result-table 五列）：前四列 flex 比例 2.4/1.2/1/1（见
// populate_rows 与表头处 addWidget stretch），操作列定宽（容最宽行
// 标记已处理＋标记已读＋详情 ≈ 178px）
constexpr int kColOps = 184;

QString kind_label(const QString& kind) {
  if (kind == QStringLiteral("warning")) return QStringLiteral("告警");
  if (kind == QStringLiteral("announce")) return QStringLiteral("公告");
  return QStringLiteral("通知");
}

QString fmt_time(qint64 ms) {
  return QDateTime::fromMSecsSinceEpoch(ms).toString(
      QStringLiteral("MM-dd HH:mm"));
}

}  // namespace

MailboxView::MailboxView(QWidget* parent) : QWidget(parent) {
  build_ui();
  restyle();
  connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
          [this] { restyle(); });
}

void MailboxView::set_client(FilesClient* client) {
  if (client_ == client) return;
  client_ = client;
  if (!client_) return;
  connect(client_, &FilesClient::mailbox_listed, this,
          [this](const QJsonArray& items, const QJsonObject& counts) {
            on_items(items, counts);
          });
  connect(client_, &FilesClient::mailbox_unread_loaded, this,
          [this](const QJsonObject& counts) {
            counts_ = counts;
            populate_tabs();
            emit unread_changed(
                static_cast<int>(counts.value(QStringLiteral("all")).toDouble()));
          });
  connect(client_, &FilesClient::mailbox_read_marked, this,
          [this](qint64, int) { request_list(); });
  connect(client_, &FilesClient::mailbox_handled, this,
          [this](qint64) { request_list(); });
  connect(client_, &FilesClient::mailbox_confirmed, this, [this](qint64) {
    request_list();
    if (selected_id_ > 0) client_->mailbox_confirmations(selected_id_);
  });
  connect(client_, &FilesClient::mailbox_confirmations_listed, this,
          [this](qint64, const QJsonArray& rows) {
            // 台账行就地重建（详情面板内；已确认在前由服务端排序保证）
            if (auto* host = detail_panel_->findChild<QVBoxLayout*>(
                    QStringLiteral("mbx_ledger_lay"))) {
              while (host->count() > 0) {
                QLayoutItem* it = host->takeAt(0);
                if (QWidget* w = it->widget()) w->deleteLater();
                delete it;
              }
              const ThemeTokens& t = ThemeManager::instance().tokens();
              for (const auto& v : rows) {
                const QJsonObject r = v.toObject();
                const QString account =
                    r.value(QStringLiteral("account")).toString();
                const qint64 ms = static_cast<qint64>(
                    r.value(QStringLiteral("confirmedMs")).toDouble());
                auto* row = new QLabel(detail_panel_);
                row->setText(account + QStringLiteral("　") +
                             (ms > 0 ? fmt_time(ms) + QStringLiteral(" 已确认")
                                     : QStringLiteral("未确认")));
                row->setStyleSheet(QStringLiteral(
                                       "font-size:12px; color:%1; padding:1px 0;")
                                       .arg(ms > 0 ? t.text.name()
                                                   : t.warning.name()));
                host->addWidget(row);
              }
              host->activate();
            }
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            if (!op.startsWith(QStringLiteral("mailbox."))) return;
            show_status(QStringLiteral("邮箱操作失败（%1 %2：%3）")
                            .arg(op,
                                 status > 0 ? QString::number(status)
                                            : QStringLiteral("网络"),
                                 error),
                        true);
          });
}

void MailboxView::build_ui() {
  auto* body = new QHBoxLayout(this);
  body->setContentsMargins(0, 0, 0, 0);
  body->setSpacing(0);

  // —— console（原型 .console：top＋表＋脚注＋边界注）——
  auto* console = new QWidget(this);
  console->setObjectName(QStringLiteral("mbx_console"));
  {
    auto* cl = new QVBoxLayout(console);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    // console-top：标题＋副题（原型一行 baseline 对齐）
    auto* top = new QWidget(console);
    auto* tl = new QHBoxLayout(top);
    tl->setContentsMargins(18, 14, 18, 6);
    tl->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("邮箱"), top);
    title->setObjectName(QStringLiteral("mbx_title"));
    tl->addWidget(title);
    sub_title_ = new QLabel(
        QStringLiteral("告警 · 通知 · 公告 三类条目进同一个收件箱；条目正文留在"
                       "各自来源，邮箱只管入箱 / 未读 / 已读 / 已处理"),
        top);
    sub_title_->setObjectName(QStringLiteral("mbx_sub"));
    sub_title_->setWordWrap(true);
    tl->addWidget(sub_title_, 1);
    cl->addWidget(top);
    // filters：tx-tabs 计数＋只看未读＋搜索＋全部标记已读（原型一行）
    auto* filters = new QWidget(console);
    auto* fl = new QHBoxLayout(filters);
    fl->setContentsMargins(18, 4, 18, 10);
    fl->setSpacing(10);
    const char* tab_names[4] = {"全部", "告警", "通知", "公告"};
    for (int i = 0; i < 4; ++i) {
      tab_btns_[i] = new QPushButton(QString::fromUtf8(tab_names[i]), filters);
      tab_btns_[i]->setObjectName(QStringLiteral("mbx_tab"));
      tab_btns_[i]->setCursor(Qt::PointingHandCursor);
      tab_btns_[i]->setFlat(true);
      tab_btns_[i]->setCheckable(true);
      connect(tab_btns_[i], &QPushButton::clicked, this, [this, i] {
        // 页签互斥（原型 tx-tabs 单选态）：点谁谁亮，其余复位
        for (int k = 0; k < 4; ++k) tab_btns_[k]->setChecked(k == i);
        type_filter_ = i == 0    ? QString()
                       : i == 1  ? QStringLiteral("warning")
                       : i == 2  ? QStringLiteral("notice")
                                 : QStringLiteral("announce");
        request_list();
      });
      fl->addWidget(tab_btns_[i]);
    }
    tab_btns_[0]->setChecked(true);
    unread_toggle_ = new QPushButton(QStringLiteral("只看未读"), filters);
    unread_toggle_->setObjectName(QStringLiteral("mbx_toggle"));
    unread_toggle_->setCursor(Qt::PointingHandCursor);
    unread_toggle_->setCheckable(true);
    connect(unread_toggle_, &QPushButton::toggled, this, [this](bool on) {
      unread_only_ = on;
      request_list();
    });
    fl->addWidget(unread_toggle_);
    search_ = new QLineEdit(filters);
    search_->setPlaceholderText(QStringLiteral("搜索标题 / 来源"));
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
      query_ = t.trimmed();
      request_list();
    });
    fl->addWidget(search_, 1);
    mark_all_btn_ = new QPushButton(QStringLiteral("全部标记已读"), filters);
    mark_all_btn_->setObjectName(QStringLiteral("mbx_mark_all"));
    mark_all_btn_->setCursor(Qt::PointingHandCursor);
    connect(mark_all_btn_, &QPushButton::clicked, this, [this] {
      if (client_ && client_->is_logged_in()) client_->mailbox_mark_read(0, true);
    });
    fl->addWidget(mark_all_btn_);
    cl->addWidget(filters);
    // 五列表（滚动区：表头随内容滚动——原型表头在表卡内）
    auto* scroll = new QScrollArea(console);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    table_host_ = new QWidget(scroll);
    table_host_->setObjectName(QStringLiteral("mbx_table"));
    auto* thl = new QVBoxLayout(table_host_);
    thl->setContentsMargins(18, 6, 18, 12);
    thl->setSpacing(0);
    // 表头行（.rt-head 五列）
    {
      auto* head = new QWidget(table_host_);
      head->setObjectName(QStringLiteral("mbx_thead"));
      auto* hl = new QHBoxLayout(head);
      hl->setContentsMargins(10, 6, 10, 6);
      hl->setSpacing(8);
      const char* cols[5] = {"条目", "来源", "时间", "状态", "操作"};
      // 原型五列是 flex 比例（c-key 2.4 / c-people 1.2 / c-time 1 /
      // c-flag 1，c-ops 定宽）：定宽窄列会把条目列挤成余数（980px 窗约
      // 110px，标题折五六行），按比例分宽条目列恒占大头，压缩行为与原型同
      auto* c0 = new QLabel(QString::fromUtf8(cols[0]), head);
      hl->addWidget(c0, 24);
      const int col_stretch[4] = {12, 10, 10, 0};
      for (int i = 1; i < 5; ++i) {
        auto* c = new QLabel(QString::fromUtf8(cols[i]), head);
        if (i == 4) c->setFixedWidth(kColOps);
        hl->addWidget(c, col_stretch[i - 1]);
      }
      thl->addWidget(head);
    }
    rows_lay_ = new QVBoxLayout();
    rows_lay_->setSpacing(0);
    thl->addLayout(rows_lay_);
    empty_hint_ = new QLabel(table_host_);
    empty_hint_->setWordWrap(true);
    thl->addWidget(empty_hint_);
    scroll->setWidget(table_host_);
    cl->addWidget(scroll, 1);
    // rt-foot：未读计数＋三面同源口径＋查阅留痕
    foot_ = new QLabel(console);
    foot_->setObjectName(QStringLiteral("mbx_foot"));
    foot_->setWordWrap(true);
    auto* fw = new QWidget(console);
    auto* fwl = new QHBoxLayout(fw);
    fwl->setContentsMargins(18, 0, 18, 8);
    fwl->addWidget(foot_);
    cl->addWidget(fw);
    // audit-log：两条硬边界（照原型原文）
    auto* audit = new QLabel(
        QStringLiteral("边界①邮箱=视图+已读状态，不是新通道（复用通知中心 / "
                       "webhook / admin 事件源 / R24 公告，禁止第三条推送链）"
                       "\n边界②清空 / 标记已读只动邮箱状态与留痕，消息归档行 / "
                       "公告行 / 告警台账行一概不动"),
        console);
    audit->setObjectName(QStringLiteral("mbx_audit"));
    audit->setWordWrap(true);
    auto* aw = new QWidget(console);
    auto* awl = new QHBoxLayout(aw);
    awl->setContentsMargins(18, 0, 18, 14);
    awl->addWidget(audit);
    cl->addWidget(aw);
    // 状态行（操作回执如实呈现；刷新成功不刷存在感）
    status_ = new QLabel(console);
    status_->setObjectName(QStringLiteral("mbx_status"));
    status_->setWordWrap(true);
    auto* sw = new QWidget(console);
    auto* swl = new QHBoxLayout(sw);
    swl->setContentsMargins(18, 0, 18, 10);
    swl->addWidget(status_);
    cl->addWidget(sw);
  }
  body->addWidget(console, 1);

  // —— infopanel（原型 .infopanel：条目详情＋已读确认台账＋告警处理留痕）——
  detail_panel_ = new QWidget(this);
  detail_panel_->setObjectName(QStringLiteral("mbx_detail"));
  detail_panel_->setFixedWidth(268);
  {
    detail_lay_ = new QVBoxLayout(detail_panel_);
    detail_lay_->setContentsMargins(16, 16, 16, 14);
    detail_lay_->setSpacing(8);
    auto* placeholder = new QLabel(
        QStringLiteral("选择左侧条目查看详情\n（公告带已读确认台账；告警带"
                       "处理留痕）"),
        detail_panel_);
    placeholder->setObjectName(QStringLiteral("mbx_detail_empty"));
    placeholder->setWordWrap(true);
    detail_lay_->addWidget(placeholder);
    detail_lay_->addStretch();
  }
  body->addWidget(detail_panel_);
}

void MailboxView::restyle() {
  const ThemeTokens& t = ThemeManager::instance().tokens();
  setStyleSheet(QStringLiteral(
                    "background:%1;"
                    "QLabel#mbx_title { font-weight:700; font-size:17px; "
                    "color:%2; }"
                    "QLabel#mbx_sub { color:%3; font-size:11.5px; }"
                    "QPushButton#mbx_tab { color:%3; font-size:12px; "
                    "padding:4px 10px; border:none; border-radius:8px; }"
                    "QPushButton#mbx_tab:hover { background:%4; }"
                    "QPushButton#mbx_tab:checked { background:%5; color:%6; "
                    "font-weight:700; }"
                    "QPushButton#mbx_toggle { color:%3; font-size:12px; "
                    "padding:4px 10px; border:1px solid %7; "
                    "border-radius:8px; background:transparent; }"
                    "QPushButton#mbx_toggle:checked { background:%5; "
                    "color:%6; border-color:transparent; }"
                    "QLineEdit { background:%8; border:1px solid %7; "
                    "border-radius:8px; padding:5px 10px; font-size:12px; "
                    "color:%2; }"
                    "QPushButton#mbx_mark_all { background:transparent; "
                    "color:%6; border:1px solid %9; border-radius:8px; "
                    "padding:5px 12px; font-size:12px; }"
                    "QPushButton#mbx_mark_all:hover { background:%4; }"
                    "QWidget#mbx_thead { border-bottom:1px solid %10; }"
                    "QWidget#mbx_thead QLabel { color:%3; font-size:11.5px; "
                    "font-weight:700; }"
                    "QLabel#mbx_foot { color:%3; font-size:11px; }"
                    "QLabel#mbx_audit { background:%11; border:1px dashed %7; "
                    "border-radius:8px; padding:8px 12px; font-size:10.5px; "
                    "color:%3; }"
                    "QLabel#mbx_status { color:%3; font-size:11px; }"
                    "QWidget#mbx_detail { background:%11; "
                    "border-left:1px solid %7; }"
                    "QLabel#mbx_detail_empty { color:%3; font-size:11.5px; }")
                    .arg(t.surface.name(), t.text.name(), t.text_muted.name(),
                         t.brand_wash.name(), t.brand_tint.name(),
                         t.brand_text.name(), t.border.name(),
                         t.surface_raised.name(), t.brand.name(),
                         t.divider.name(), t.surface_alt.name()));
  populate_rows();  // 行内样式随令牌重建
  refresh_detail();
}

void MailboxView::refresh() { request_list(); }

void MailboxView::refresh_unread() {
  if (client_ && client_->is_logged_in()) client_->mailbox_unread();
}

void MailboxView::request_list() {
  if (!client_ || !client_->is_logged_in()) return;
  client_->mailbox_list(type_filter_, unread_only_, query_);
}

void MailboxView::on_items(const QJsonArray& items, const QJsonObject& counts) {
  items_ = items;
  counts_ = counts;
  populate_tabs();
  populate_rows();
  refresh_detail();
  emit unread_changed(
      static_cast<int>(counts.value(QStringLiteral("all")).toDouble()));
}

void MailboxView::populate_tabs() {
  const QString keys[4] = {QStringLiteral("all"), QStringLiteral("warning"),
                           QStringLiteral("notice"),
                           QStringLiteral("announce")};
  const char* names[4] = {"全部 ", "告警 ", "通知 ", "公告 "};
  for (int i = 0; i < 4; ++i) {
    if (tab_btns_[i]) {
      tab_btns_[i]->setText(
          QString::fromUtf8(names[i]) +
          QString::number(static_cast<int>(
              counts_.value(keys[i]).toDouble())));
    }
  }
  if (foot_) {
    foot_->setText(QStringLiteral("未读 %1（告警 %2 · 通知 %3 · 公告 %4）· "
                                  "服务端为唯一未读来源，桌面 / 手机 / 管理台"
                                  "三面同源同数　查阅留痕：audit.message.search")
                       .arg(QString::number(static_cast<int>(
                                counts_.value(keys[0]).toDouble())),
                            QString::number(static_cast<int>(
                                counts_.value(keys[1]).toDouble())),
                            QString::number(static_cast<int>(
                                counts_.value(keys[2]).toDouble())),
                            QString::number(static_cast<int>(
                                counts_.value(keys[3]).toDouble()))));
  }
}

int MailboxView::row_count() const {
  int n = 0;
  if (table_host_) n = table_host_->findChildren<QFrame*>(
                          QStringLiteral("mbx_row")).size();
  return n;
}

QString MailboxView::foot_text() const {
  return foot_ ? foot_->text() : QString();
}

void MailboxView::populate_rows() {
  if (!rows_lay_) return;
  const ThemeTokens& t = ThemeManager::instance().tokens();
  // 清空重建（三招模板：摘除→deleteLater；新行显式 add）
  while (rows_lay_->count() > 0) {
    QLayoutItem* it = rows_lay_->takeAt(0);
    if (QWidget* w = it->widget()) w->deleteLater();
    delete it;
  }
  if (empty_hint_) {
    empty_hint_->setText(
        items_.isEmpty()
            ? QStringLiteral("暂无条目——告警 / 通知 / 公告三类事件源入箱后"
                             "在此显示")
            : QString());
    empty_hint_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px; padding:10px;")
            .arg(t.text_muted.name()));
  }
  for (const auto& v : items_) {
    const QJsonObject it = v.toObject();
    const qint64 id =
        static_cast<qint64>(it.value(QStringLiteral("id")).toDouble());
    const QString kind = it.value(QStringLiteral("kind")).toString();
    const QString title = it.value(QStringLiteral("title")).toString();
    const QString sub = it.value(QStringLiteral("sub")).toString();
    const QString source = it.value(QStringLiteral("source")).toString();
    const qint64 ms = static_cast<qint64>(it.value(QStringLiteral("time")).toDouble());
    const int state = static_cast<int>(
        it.value(QStringLiteral("state")).toDouble());
    const QString handled_by =
        it.value(QStringLiteral("handledBy")).toString();
    const qint64 confirmed_ms = static_cast<qint64>(
        it.value(QStringLiteral("confirmedMs")).toDouble());
    const bool unread = state == 0;
    const bool handled = kind == QStringLiteral("warning") && !handled_by.isEmpty();
    const bool confirmed =
        kind == QStringLiteral("announce") && confirmed_ms > 0;

    auto* row = new QFrame(table_host_);
    row->setObjectName(QStringLiteral("mbx_row"));
    row->setProperty("mbx_id", static_cast<double>(id));
    // 未读行浅品牌底（原型 .rt-row style=brand-softer）
    row->setStyleSheet(QStringLiteral("QFrame#mbx_row { background:%1; "
                                      "border-radius:8px; }")
                           .arg(unread ? t.brand_tint.name()
                                       : t.surface.name()));
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(10, 8, 10, 8);
    rl->setSpacing(8);
    // —— 条目列：类目胶囊＋未读点＋标题（12.5px 粗）＋副题（11px）——
    auto* key = new QWidget(row);
    auto* kl = new QHBoxLayout(key);
    kl->setContentsMargins(0, 0, 0, 0);
    kl->setSpacing(8);
    auto* tag = new QLabel(kind_label(kind), key);
    tag->setStyleSheet(
        QStringLiteral("color:%1; border:1px solid %2; border-radius:8px; "
                       "padding:1px 8px; font-size:10.5px;")
            .arg(kind == QStringLiteral("warning") ? t.warning.name()
                                                   : t.text_muted.name(),
                 t.border.name()));
    kl->addWidget(tag);
    auto* txt = new QWidget(key);
    auto* txl = new QVBoxLayout(txt);
    txl->setContentsMargins(0, 0, 0, 0);
    txl->setSpacing(1);
    auto* tline = new QLabel(
        (unread ? QStringLiteral("● ") : QString()) + title, txt);
    tline->setStyleSheet(
        QStringLiteral("font-size:12.5px; font-weight:700; color:%1;")
            .arg(unread ? t.brand_text.name() : t.text.name()));
    tline->setWordWrap(true);
    txl->addWidget(tline);
    auto* sline = new QLabel(sub, txt);
    sline->setStyleSheet(
        QStringLiteral("font-size:11px; color:%1;").arg(t.text_muted.name()));
    sline->setWordWrap(true);
    txl->addWidget(sline);
    kl->addWidget(txt, 1);
    rl->addWidget(key, 24);
    // —— 来源 / 时间列（比例分宽，同表头 12/10）——
    auto* src = new QLabel(source, row);
    src->setWordWrap(true);
    src->setStyleSheet(
        QStringLiteral("font-size:11.5px; color:%1;").arg(t.text_muted.name()));
    rl->addWidget(src, 12);
    auto* tm = new QLabel(fmt_time(ms), row);
    tm->setStyleSheet(
        QStringLiteral("font-size:11.5px; color:%1;").arg(t.text_muted.name()));
    rl->addWidget(tm, 10);
    // —— 状态列（.tag 样式：未读 paused / 已读 done / 告警已处理 doing /
    //     公告待确认 paused、已确认 done）——
    QString state_text;
    QColor state_col = t.text_muted;
    if (kind == QStringLiteral("warning")) {
      state_text = handled ? QStringLiteral("已处理") : (unread ? QStringLiteral("未读") : QStringLiteral("已读"));
      state_col = handled ? t.success : (unread ? t.warning : t.text_muted);
    } else if (kind == QStringLiteral("announce")) {
      state_text = confirmed ? QStringLiteral("已确认") : QStringLiteral("待确认");
      state_col = confirmed ? t.success : t.warning;
    } else {
      state_text = unread ? QStringLiteral("未读") : QStringLiteral("已读");
      state_col = unread ? t.warning : t.text_muted;
    }
    auto* flag = new QLabel(state_text, row);
    flag->setStyleSheet(
        QStringLiteral("color:%1; border:1px solid %1; border-radius:8px; "
                       "padding:1px 8px; font-size:10.5px;")
            .arg(state_col.name()));
    rl->addWidget(flag, 10);
    // —— 操作列（按类目/状态给真钮：告警＝标记已处理/标记已读/详情；
    //     通知＝去会话/去任务/去审批/去群＋标记已读；公告＝确认已读/详情）——
    auto* ops = new QWidget(row);
    auto* ol = new QHBoxLayout(ops);
    ol->setContentsMargins(0, 0, 0, 0);
    ol->setSpacing(8);
    const QString op_style =
        QStringLiteral("QPushButton { background:transparent; color:%1; "
                       "border:none; padding:2px 4px; font-size:11.5px; }"
                       "QPushButton:hover { color:%2; }")
            .arg(t.brand_text.name(), t.brand_hover.name());
    auto add_op = [&](const QString& text, bool primary,
                      const std::function<void()>& fn) {
      auto* b = new QPushButton(text, ops);
      b->setCursor(Qt::PointingHandCursor);
      b->setStyleSheet(primary
                           ? QStringLiteral("QPushButton { background:%1; "
                                            "color:%2; border:none; "
                                            "border-radius:6px; padding:3px "
                                            "10px; font-size:11.5px; "
                                            "font-weight:700; }")
                                 .arg(t.brand.name(), t.on_brand.name())
                           : op_style);
      connect(b, &QPushButton::clicked, this, fn);
      ol->addWidget(b);
    };
    if (kind == QStringLiteral("warning")) {
      if (!handled) {
        add_op(QStringLiteral("标记已处理"), true, [this, id] {
          if (client_ && client_->is_logged_in())
            client_->mailbox_mark_handled(id);
        });
        if (unread)
          add_op(QStringLiteral("标记已读"), false, [this, id] {
            if (client_ && client_->is_logged_in()) client_->mailbox_mark_read(id);
          });
      }
      add_op(QStringLiteral("详情"), false,
             [this, id] { select_item(id); });
    } else if (kind == QStringLiteral("announce")) {
      if (!confirmed)
        add_op(QStringLiteral("确认已读"), true, [this, id] {
          if (client_ && client_->is_logged_in()) client_->mailbox_confirm(id);
        });
      add_op(QStringLiteral("详情"), false, [this, id] { select_item(id); });
    } else {
      // 通知：正文在各自来源——跳转钮（ref 路由交 MainWindow）＋未读时
      // 标记已读
      const QString ref = it.value(QStringLiteral("ref")).toString();
      QString jump_label;
      if (ref == QLatin1String("tasks")) jump_label = QStringLiteral("去任务");
      else if (ref == QLatin1String("approvals"))
        jump_label = QStringLiteral("去审批");
      else if (ref.startsWith(QLatin1String("group:")))
        // 注意：@我 是中文，比较必须走 QStringLiteral——QLatin1String 只
        // 表达 Latin-1，中文按字节截断比较恒不中（去会话退化成去群）
        jump_label = source == QStringLiteral("@我")
                         ? QStringLiteral("去会话")
                         : QStringLiteral("去群");
      if (!jump_label.isEmpty())
        add_op(jump_label, false, [this, ref] { emit jump_requested(ref); });
      if (unread)
        add_op(QStringLiteral("标记已读"), false, [this, id] {
          if (client_ && client_->is_logged_in()) client_->mailbox_mark_read(id);
        });
      add_op(QStringLiteral("详情"), false, [this, id] { select_item(id); });
    }
    ol->addStretch();
    ops->setFixedWidth(kColOps);  // 与表头操作列同宽，列缘对齐
    rl->addWidget(ops);
    rows_lay_->addWidget(row);
  }
  rows_lay_->activate();
}

void MailboxView::select_item(qint64 id) {
  selected_id_ = id;
  refresh_detail();
}

void MailboxView::refresh_detail() {
  if (!detail_lay_) return;
  const ThemeTokens& t = ThemeManager::instance().tokens();
  // 清空重建
  while (detail_lay_->count() > 0) {
    QLayoutItem* it = detail_lay_->takeAt(0);
    if (QWidget* w = it->widget()) w->deleteLater();
    delete it;
  }
  const QJsonObject* sel = nullptr;
  QJsonObject found;
  for (const auto& v : items_) {
    QJsonObject it = v.toObject();
    if (static_cast<qint64>(it.value(QStringLiteral("id")).toDouble()) ==
        selected_id_) {
      found = it;
      sel = &found;
      break;
    }
  }
  if (!sel) {
    auto* placeholder = new QLabel(
        QStringLiteral("选择左侧条目查看详情\n（公告带已读确认台账；告警带"
                       "处理留痕）"),
        detail_panel_);
    placeholder->setObjectName(QStringLiteral("mbx_detail_empty"));
    placeholder->setStyleSheet(
        QStringLiteral("color:%1; font-size:11.5px;").arg(t.text_muted.name()));
    detail_lay_->addWidget(placeholder);
    detail_lay_->addStretch();
    detail_lay_->activate();
    return;
  }
  const QString kind = sel->value(QStringLiteral("kind")).toString();
  const QString title = sel->value(QStringLiteral("title")).toString();
  const QString sub = sel->value(QStringLiteral("sub")).toString();
  const QString source = sel->value(QStringLiteral("source")).toString();
  const qint64 ms =
      static_cast<qint64>(sel->value(QStringLiteral("time")).toDouble());
  const int state =
      static_cast<int>(sel->value(QStringLiteral("state")).toDouble());
  const QString handled_by =
      sel->value(QStringLiteral("handledBy")).toString();
  const qint64 handled_ms = static_cast<qint64>(
      sel->value(QStringLiteral("handledMs")).toDouble());
  const qint64 confirmed_ms = static_cast<qint64>(
      sel->value(QStringLiteral("confirmedMs")).toDouble());
  const int confirm_total = static_cast<int>(
      sel->value(QStringLiteral("confirmTotal")).toDouble());
  const int confirm_count = static_cast<int>(
      sel->value(QStringLiteral("confirmCount")).toDouble());

  auto head = [&](const QString& text) {
    auto* l = new QLabel(text, detail_panel_);
    l->setStyleSheet(QStringLiteral("font-weight:700; font-size:13px; "
                                    "color:%1; margin-top:4px;")
                         .arg(t.text.name()));
    detail_lay_->addWidget(l);
  };
  auto kv = [&](const QString& k, const QString& v, bool brand = false) {
    auto* row = new QWidget(detail_panel_);
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(8);
    auto* kl = new QLabel(k, row);
    kl->setFixedWidth(64);
    kl->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                          .arg(t.text_muted.name()));
    auto* vl = new QLabel(v, row);
    vl->setStyleSheet(
        QStringLiteral("color:%1; font-size:11.5px; %2")
            .arg(brand ? t.brand_text.name() : t.text.name(),
                 brand ? QStringLiteral("font-weight:600;") : QString()));
    vl->setWordWrap(true);
    rl->addWidget(kl);
    rl->addWidget(vl, 1);
    detail_lay_->addWidget(row);
  };

  head(QStringLiteral("条目详情"));
  {
    auto* pills = new QWidget(detail_panel_);
    auto* pl = new QHBoxLayout(pills);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(8);
    auto* k = new QLabel(kind_label(kind), pills);
    k->setStyleSheet(
        QStringLiteral("color:%1; border:1px solid %2; border-radius:8px; "
                       "padding:1px 8px; font-size:10.5px;")
            .arg(kind == QStringLiteral("warning") ? t.warning.name()
                                                   : t.text_muted.name(),
                 t.border.name()));
    pl->addWidget(k);
    QString state_text;
    if (kind == QStringLiteral("warning"))
      state_text = handled_by.isEmpty()
                       ? (state == 0 ? QStringLiteral("未读") : QStringLiteral("已读"))
                       : QStringLiteral("已处理");
    else if (kind == QStringLiteral("announce"))
      state_text = confirmed_ms > 0 ? QStringLiteral("已确认")
                                   : QStringLiteral("待确认");
    else state_text = state == 0 ? QStringLiteral("未读") : QStringLiteral("已读");
    auto* s = new QLabel(state_text, pills);
    s->setStyleSheet(
        QStringLiteral("color:%1; border:1px solid %1; border-radius:8px; "
                       "padding:1px 8px; font-size:10.5px;")
            .arg(state == 0 && handled_by.isEmpty() && confirmed_ms <= 0
                     ? t.warning.name()
                     : t.success.name()));
    pl->addWidget(s);
    pl->addStretch();
    detail_lay_->addWidget(pills);
  }
  {
    auto* tl = new QLabel(title, detail_panel_);
    tl->setStyleSheet(QStringLiteral("font-weight:700; font-size:13.5px; "
                                     "line-height:1.5; color:%1;")
                          .arg(t.text.name()));
    tl->setWordWrap(true);
    detail_lay_->addWidget(tl);
  }
  kv(QStringLiteral("来源"), source);
  kv(QStringLiteral("发布时间"), fmt_time(ms));
  kv(QStringLiteral("正文位置"),
     kind == QStringLiteral("announce")
         ? QStringLiteral("R24 公告（群置顶）")
         : (kind == QStringLiteral("warning")
                ? QStringLiteral("服务端事件源（告警台账留痕）")
                : QStringLiteral("正文留在各自来源（会话 / 任务 / 审批 / 群知识共享）")),
     false);
  kv(QStringLiteral("提醒级别"),
     kind == QStringLiteral("warning")
         ? QStringLiteral("强提醒 · 红点持续到已处理")
         : (kind == QStringLiteral("announce")
                ? QStringLiteral("弱提醒 · 已读确认台账")
                : QStringLiteral("按来源分级（见副题）")));
  // 动作钮（公告未确认＝确认已读；告警未处理＝标记已处理）
  if (kind == QStringLiteral("announce") && confirmed_ms <= 0) {
    auto* b = new QPushButton(QStringLiteral("确认已读"), detail_panel_);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                       "border-radius:8px; padding:7px 0; font-weight:700; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name()));
    connect(b, &QPushButton::clicked, this, [this, id = selected_id_] {
      if (client_ && client_->is_logged_in()) client_->mailbox_confirm(id);
    });
    detail_lay_->addWidget(b);
  }
  if (kind == QStringLiteral("warning") && handled_by.isEmpty()) {
    auto* b = new QPushButton(QStringLiteral("标记已处理"), detail_panel_);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                       "border-radius:8px; padding:7px 0; font-weight:700; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand.name(), t.on_brand.name(), t.brand_hover.name()));
    connect(b, &QPushButton::clicked, this, [this, id = selected_id_] {
      if (client_ && client_->is_logged_in()) client_->mailbox_mark_handled(id);
    });
    detail_lay_->addWidget(b);
  }

  if (kind == QStringLiteral("announce")) {
    head(QStringLiteral("已读确认台账"));
    kv(QStringLiteral("已确认"),
       QStringLiteral("%1 / %2").arg(confirm_count).arg(confirm_total));
    {
      // 台账行容器（confirmations 回包在此重建）——台账随每次详情重建重拉
      //（谁/何时持久可查）：确认/列表回包先后不定，若只在 select_item 拉
      // 一次，列表回包重建面板会把台账行冲掉（空台账滞留）
      auto* ledger = new QWidget(detail_panel_);
      auto* ll = new QVBoxLayout(ledger);
      ll->setObjectName(QStringLiteral("mbx_ledger_lay"));
      ll->setContentsMargins(0, 0, 0, 0);
      ll->setSpacing(2);
      detail_lay_->addWidget(ledger);
      if (client_ && client_->is_logged_in() && selected_id_ > 0)
        client_->mailbox_confirmations(selected_id_);
    }
    auto* note = new QLabel(
        QStringLiteral("谁 / 何时确认持久化可查（按人 / 按条目）；是否强制"
                       "全员确认＝待拍板项（mailbox.md §Ⅵ-1）。"),
        detail_panel_);
    note->setWordWrap(true);
    note->setStyleSheet(
        QStringLiteral("font-size:10.5px; color:%1; line-height:1.7;")
            .arg(t.text_muted.name()));
    detail_lay_->addWidget(note);
  }

  if (kind == QStringLiteral("warning")) {
    head(QStringLiteral("告警处理留痕"));
    kv(QStringLiteral("条目"), title);
    kv(QStringLiteral("处理人"), handled_by.isEmpty()
                                      ? QStringLiteral("—")
                                      : handled_by);
    kv(QStringLiteral("处理时间"),
       handled_ms > 0 ? fmt_time(handled_ms) : QStringLiteral("—"));
    kv(QStringLiteral("动作"),
       handled_by.isEmpty() ? QStringLiteral("未处理（红点持续）")
                            : QStringLiteral("标记已处理"),
       !handled_by.isEmpty());
    auto* note = new QLabel(
        QStringLiteral("处理动作与处理人留痕；红点持续到已处理为止。非管理员"
                       "默认看不到系统详情（磁盘路径 / 日志摘录脱敏，待拍板项"
                       " §Ⅵ-2）。"),
        detail_panel_);
    note->setWordWrap(true);
    note->setStyleSheet(
        QStringLiteral("font-size:10.5px; color:%1; line-height:1.7;")
            .arg(t.text_muted.name()));
    detail_lay_->addWidget(note);
  }
  detail_lay_->addStretch();
  detail_lay_->activate();
}

void MailboxView::show_status(const QString& text, bool error) {
  if (!status_) return;
  const ThemeTokens& t = ThemeManager::instance().tokens();
  status_->setText(text);
  status_->setStyleSheet(
      QStringLiteral("color:%1; font-size:11px;")
          .arg(error ? t.warning.name() : t.text_muted.name()));
}

}  // namespace memex::client
