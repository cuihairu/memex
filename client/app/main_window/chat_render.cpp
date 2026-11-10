// 聊天渲染（LAY-002 拆分）：气泡注入、会话历史渲染与日期筛选、
// append_* 行族、空态引导、回执态落行与状态断言面。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QDateEdit>
#include <QDateTimeEdit>
#include <QTextEdit>
#include <QScrollBar>
#include <QStatusBar>
#include <core/local_store.hpp>

#include "theme.hpp"
namespace memex::client {

// —— 需求批① 文字颜色 ——
void MainWindow::inject_message(const QString& from_id, const QString& text,
                                bool outgoing, const QString& msg_id,
                                const QString& receipt, qint64 ts_ms,
                                quint64 seq) {
  append_message(from_id, text,
                ts_ms > 0 ? ts_ms : QDateTime::currentMSecsSinceEpoch(),
                outgoing, QStringLiteral("direct"), msg_id, receipt, seq);
}

void MainWindow::inject_image(const QString& from_id, const QString& image_path,
                              bool outgoing) {
  append_image_message(from_id, image_path, QDateTime::currentMSecsSinceEpoch(),
                       outgoing, QStringLiteral("direct"));
}

QString MainWindow::banner_text() const { return banner_->text(); }

QString MainWindow::status_text() const { return statusBar()->currentMessage(); }

QString MainWindow::chat_html() const { return chat_view_->toHtml(); }

QString MainWindow::input_hint() const { return input_box_->placeholderText(); }

QString MainWindow::chat_meta() const { return chat_meta_->text(); }

// —— T4.3 消息状态与多端（验收面）——
QString MainWindow::delivery_text() const { return delivery_text_; }
QString MainWindow::kick_text() const { return kick_text_; }
QStringList MainWindow::online_accounts() const {
  QStringList out = online_accounts_.values(); // values() 直取，避免区间构造歧义
  out.sort();
  return out;
}
void MainWindow::set_delivery_state(const QString& text) {
  delivery_text_ = text;
  show_status(text); // 状态栏同步（最近一条发出消息的状态常驻可查）
}

// —— 需求批⑦ 消息回执：开关与消息旁状态 ——

bool MainWindow::read_receipts_enabled(const QString& peer) const {
  // 全局默认开；会话级显式设置（true/false）覆盖全局（QSettings 持久化）
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  if (!peer.isEmpty() &&
      settings.contains(QStringLiteral("receipts/peer/%1").arg(peer))) {
    return settings.value(QStringLiteral("receipts/peer/%1").arg(peer))
        .toBool();
  }
  return settings
      .value(QStringLiteral("receipts/send_read"), true)
      .toBool();
}

void MainWindow::set_read_receipts_enabled(const QString& peer, bool enabled) {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  if (peer.isEmpty()) {
    settings.setValue(QStringLiteral("receipts/send_read"), enabled);
    show_status(enabled ? QStringLiteral("已读回执：全局开启（新收消息将上报已读）")
                        : QStringLiteral("已读回执：全局关闭（不再上报已读）"));
    return;
  }
  settings.setValue(QStringLiteral("receipts/peer/%1").arg(peer), enabled);
  show_status(enabled ? QStringLiteral("已读回执（本会话 %1）：开启").arg(peer)
                      : QStringLiteral("已读回执（本会话 %1）：关闭").arg(peer));
}

void MainWindow::apply_message_receipt(const QString& msg_id,
                                       const QString& state) {
  if (msg_id.isEmpty() || state.isEmpty()) return;
  bool changed = false;
  for (ChatRow& row : chat_rows_) {
    if (!row.system && !row.msg_id.isEmpty() && row.msg_id == msg_id &&
        row.receipt != state) {
      // 只升不降（与本地库同口径）：已读不被乱序送达通知倒退
      if (row.receipt == QStringLiteral("read") &&
          state == QStringLiteral("delivered"))
        continue;
      row.receipt = state;
      changed = true;
    }
  }
  if (changed) rerender_chat();
}

void MainWindow::apply_direct_receipt(quint64 seq, const QString& state) {
  // 直连域回执落行（需求批⑦·已读灰勾单）：直连行无 msg_id，按我发出行
  // 的发送序号命中；只升不降与 apply_message_receipt 同口径
  if (seq == 0 || state.isEmpty()) return;
  bool changed = false;
  for (ChatRow& row : chat_rows_) {
    if (!row.system && row.outgoing && row.seq == seq &&
        row.receipt != state) {
      if (row.receipt == QStringLiteral("read") &&
          state == QStringLiteral("delivered"))
        continue;
      row.receipt = state;
      changed = true;
    }
  }
  if (changed) rerender_chat();
}

void MainWindow::open_chat(const QString& kind, const QString& id) {
  // 传输面/组织架构面下打开会话（通知点入/传输卡「所在会话」/档案「发消息」）：
  // 先回会话视图
  if (sidebar_mode_ == QStringLiteral("transfers")) leave_transfers_view();
  if (sidebar_mode_ == QStringLiteral("org")) leave_org_view();
  // 布局令：对话面板只在打开会话后展开（列表态点好友／菜单／通知点入）；
  // 从列表态展开时若窗口还停在窄长方形，恢复会话态几何（Telegram 按态归档
  // 优先，无存档＝默认宽 1280×当前高）
  setMaximumWidth(QWIDGETSIZE_MAX); // 释放列表态的右缘上限（见 close_chat）
  if (!chat_open_ && !isMaximized() && width() < 520) {
    if (!restore_chat_geometry()) resize(1280, height());
  }
  // 会话态：列表列收回固定宽，余量交给 chat 面板（stretch 100）
  side_->setFixedWidth(260);
  chat_panel_->show();
  chat_open_ = true; // 布局态切到会话态（存档键/最小尺寸判据）
  apply_state_minimum();
  current_kind_ = kind;
  current_peer_ = id;
  // 打开即读：清该会话未读角标并重渲行（原型行 n-count 只在列表态显）
  if (unread_counts_.remove(id) > 0) refresh_devices();
  // 「盯」已读提醒高亮同口径：打开该会话即清（已到场，不再占角标位）
  if (watch_reminded_peers_.remove(id) > 0) refresh_devices();
  chat_showing_guidance_ = false;
  chat_view_->clear();
  chat_rows_.clear(); // 换会话即清空重渲记录（否则旧行会在切换主题时冒出来）

  const bool collab = kind == QStringLiteral("collab");
  const bool group = kind == QStringLiteral("group");
  // 群专用头部钮（原型 .ch-actions）与非群会话一律隐藏
  if (head_announce_btn_) head_announce_btn_->setVisible(group);
  if (head_gfiles_btn_) head_gfiles_btn_->setVisible(group);
  if (head_members_btn_) head_members_btn_->setVisible(group);
  if (at_menu_) at_menu_->hide(); // 换会话即收 @ 成员菜单
  chat_title_->setToolTip(QString()); // 群号悬浮提示（群分支再挂，其余清空）
  QString avatar_src; // 头像取字源（原型 .chat-head .avatar.sm 首字）
  if (collab) {
    chat_title_->setText(id);
    chat_meta_->setText(QStringLiteral("%1 · 协作态 · 消息进入服务端归档")
                            .arg(id));
    avatar_src = id;
  } else if (group) {
    // 服务端群会话（T4.1）：标题群名；meta 行走原型口径（群成员 · 群主 ·
    // 全量归档，见 group_meta_text），群号挪到标题悬浮提示
    const quint64 gid = id.mid(QStringLiteral("group:").size()).toULongLong();
    const GroupEntry g = groups_.value(gid);
    const QString gname = g.name.isEmpty()
                              ? QStringLiteral("群 %1").arg(gid)
                              : g.name;
    chat_title_->setText(gname);
    chat_title_->setToolTip(QStringLiteral("群 %1").arg(gid));
    avatar_src = gname;
    chat_meta_->setText(group_meta_text(g));
  } else if (kind == QStringLiteral("dgroup")) {
    chat_title_->setText(dgroup_title(id));
    chat_meta_->setText(
        QStringLiteral("%1 台设备 · 点对点扇出 · 消息不进归档；对端回复落在"
                       "与其的单聊会话")
            .arg(dgroup_members_.value(id).size()));
    avatar_src = dgroup_title(id);
  } else {
    const Peer p = direct_engine_.peer(id.toStdString());
    const QString name = p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    // 原型行名「人名 · 设备名」：对端登录账号即人名位（未登录＝纯设备名）
    const QString account = QString::fromStdString(p.account);
    chat_title_->setText(account.isEmpty()
                             ? name
                             : QStringLiteral("%1 · %2").arg(account, name));
    avatar_src = account.isEmpty() ? name : account;
    // 平台-7：降级态 meta 固定「已降级 · 未归档」前缀（服务端不可达对本
    // 会话恒真，压过跨态——更具体的口径）；T4.2 跨态次之（A7）
    if (collab_degraded_) {
      chat_meta_->setText(
          QStringLiteral("已降级 · 未归档 · %1 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    } else if (is_cross_state(id)) {
      chat_meta_->setText(
          QStringLiteral("跨态 · 未归档 · %1 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    } else {
      chat_meta_->setText(
          QStringLiteral("%1 · 点对点直连 · TCP %2")
              .arg(p.address.toString(), QString::number(p.tcp_port)));
    }
  }
  chat_avatar_src_ = avatar_src;
  update_chat_avatar();
  update_banner();
  update_group_panel(); // 群右栏随会话换内容/显隐（原型 .infopanel）

  // 需求批⑧：筛选态与挂起跳转只属于当前会话——换会话即复位
  filter_from_ms_ = 0;
  filter_until_ms_ = 0;
  pending_jump_ = QDate();
  render_open_history();
}

// 会话历史渲染（open_chat 尾段抽取，按日期跳转/清筛选复用）：跨态标记→
// 空历史引导→双态合并行→已读上报（⑦开关裁决）→回执态补查（⑦）。
void MainWindow::render_open_history() {
  const QString kind = current_kind_;
  const QString id = current_peer_;
  const bool collab = kind == QStringLiteral("collab");
  const bool group = kind == QStringLiteral("group");
  const QString my_id = (collab || group)
                            ? collab_engine_.account()
                            : QString::fromStdString(direct_engine_.device_id());
  chat_view_->clear();
  chat_rows_.clear();

  // T4.2 跨态会话固定标识（A7）＋归档起点提示（A8 客户端面）：
  // 标识常驻不可关闭，无论历史空否都打。
  if (kind == QStringLiteral("direct") && is_cross_state(id)) {
    if (collab_engine_.is_logged_in()) {
      append_chip_line(QStringLiteral(
          "跨态会话 · 未归档：与未登录终端点对点传输（标记常驻不可关闭）；"
          "归档自 %1（本次协作态登录时刻，此前跨态直连消息不进归档）")
                           .arg(QDateTime::fromMSecsSinceEpoch(collab_login_ms_)
                                    .toString(QStringLiteral(
                                        "yyyy-MM-dd HH:mm:ss"))));
    } else {
      append_chip_line(QStringLiteral(
          "跨态会话 · 未归档：对端已登录协作态，本机未登录——本机消息不进归档"
          "（标记常驻不可关闭）"));
    }
  }

  const auto hist = direct_engine_.history(id);
  if (hist.isEmpty()) {
    if (collab) {
      append_system_line(
          QStringLiteral("已与 %1 建立协作会话 · 消息经服务端转发并全量归档")
              .arg(esc(id)));
    } else if (group) {
      append_system_line(
          QStringLiteral("已打开群「%1」· 消息经服务端按成员扇出并全量归档")
              .arg(esc(chat_title_->text())));
    } else if (kind == QStringLiteral("dgroup")) {
      append_system_line(
          QStringLiteral("已建立临时群「%1」（%2 台设备）· 点对点扇出，"
                         "本会话不进归档")
              .arg(esc(chat_title_->text()))
              .arg(dgroup_members_.value(id).size()));
    } else {
      // 原型 .sys-hint 胶囊口径：发现方式＋不归档（2026-10-30 对齐文案）
      append_system_line(
          QStringLiteral("已通过 UDP 广播发现对方 · 本对话不归档"));
    }
  }
  for (const StoredMessage& m : hist) {
    append_message(QString::fromStdString(m.from),
                   QString::fromStdString(m.text), m.ts_ms,
                   m.from == my_id.toStdString(),
                   QString::fromStdString(m.source),
                   QString::fromStdString(m.msg_id),
                   QString::fromStdString(m.receipt), m.seq);
  }
  // T4.3：打开协作单聊=已读历史——对最近一条收到的协作消息上报已读
  //（打开前到达的消息此前未上报；逐条全报是噪音，只报最新一条）。
  // 需求批⑦：是否上报受会话/全局已读回执开关裁决。
  if (collab && read_receipts_enabled(id)) {
    for (auto it = hist.crbegin(); it != hist.crend(); ++it) {
      if (it->from == id.toStdString() && !it->msg_id.empty()) {
        collab_engine_.mark_read(QString::fromStdString(it->msg_id));
        break;
      }
    }
  }
  // 直连已读上报（需求批⑦·已读灰勾单 2026-10-14）：打开直连会话＝本轮
  // 消息已览，对来自对端的消息逐条上报已读（引擎按序号去重——重开聊天
  // 不重发，重启后重报一次幂等）。与协作态「只报最新一条」不同：直连无
  // 服务端台账，漏报无补拉机会，逐条才闭环；开关裁决同款。
  if (kind == QStringLiteral("direct") && read_receipts_enabled(id)) {
    for (const StoredMessage& m : hist) {
      if (m.from == id.toStdString() && m.seq != 0)
        direct_engine_.mark_read(id.toStdString(), m.seq);
    }
  }
  // 需求批⑦ 归档扩：打开会话即补查自己消息的回执态——离线期错过的
  // DELIVER_NOTICE/READ_NOTICE 经服务端台账对齐（最近 50 条封顶防滥用）。
  if ((collab || group) && collab_engine_.is_logged_in()) {
    QStringList mine;
    for (auto it = hist.crbegin(); it != hist.crend() && mine.size() < 50;
         ++it) {
      if (it->from == my_id.toStdString() && !it->msg_id.empty())
        mine.push_back(QString::fromStdString(it->msg_id));
    }
    collab_engine_.query_receipts(mine);
  }
}

// —— 需求批⑧ 聊天记录按日期 ——
// 筛选态渲染：本地索引（双态合并，idx_messages_peer_ts）按时间窗重建当前
// 会话行；头部系统行注明范围与条数；自己发出行的回执态照常补查（⑦）。
void MainWindow::render_filtered_history() {
  const bool collab = current_kind_ == QStringLiteral("collab");
  const bool group = current_kind_ == QStringLiteral("group");
  const QString my_id = (collab || group)
                            ? collab_engine_.account()
                            : QString::fromStdString(direct_engine_.device_id());
  chat_view_->clear();
  chat_rows_.clear();
  const auto hist = direct_engine_.history_between(
      current_peer_, filter_from_ms_, filter_until_ms_, 2000);
  append_system_line(QStringLiteral("已筛选：%1 ~ %2 · 共 %3 条 · "
                                   "「按日期」对话框可清除筛选")
                         .arg(day_label(filter_from_ms_),
                              day_label(filter_until_ms_))
                         .arg(hist.size()));
  for (const StoredMessage& m : hist) {
    append_message(QString::fromStdString(m.from),
                   QString::fromStdString(m.text), m.ts_ms,
                   m.from == my_id.toStdString(),
                   QString::fromStdString(m.source),
                   QString::fromStdString(m.msg_id),
                   QString::fromStdString(m.receipt), m.seq);
  }
  if ((collab || group) && collab_engine_.is_logged_in()) {
    QStringList mine;
    for (auto it = hist.crbegin(); it != hist.crend() && mine.size() < 50;
         ++it) {
      if (it->from == my_id.toStdString() && !it->msg_id.empty())
        mine.push_back(QString::fromStdString(it->msg_id));
    }
    collab_engine_.query_receipts(mine);
  }
  // 直连已读上报（需求批⑦·已读灰勾单）：筛选窗同开窗口径——窗内对端
  // 消息视为已览，逐条上报（引擎按序号去重）
  if (!collab && !group && read_receipts_enabled(current_peer_)) {
    for (const StoredMessage& m : hist) {
      if (m.from == current_peer_.toStdString() && m.seq != 0)
        direct_engine_.mark_read(current_peer_.toStdString(), m.seq);
    }
  }
}

// 范围筛选（毫秒含端点）：本地即时重建；协作/群登录态同时向服务端补拉
// 该窗（服务端归档比本地多的行——离线期/换机漏收——回包落本地索引后
// history_received 触发再次渲染）。
void MainWindow::apply_date_filter(qint64 from_ms, qint64 until_ms) {
  if (current_peer_.isEmpty() || chat_panel_->isHidden()) {
    show_status(QStringLiteral("先选择会话再按日期查看"));
    return;
  }
  // 两侧全开的窗＝未筛选（与清除同义，避免「筛选态字段全 0」歧义）
  if (from_ms <= 0 && until_ms <= 0) {
    clear_date_filter();
    return;
  }
  filter_from_ms_ = from_ms;
  filter_until_ms_ = until_ms;
  render_filtered_history();
  if ((current_kind_ == QStringLiteral("collab") ||
       current_kind_ == QStringLiteral("group")) &&
      collab_engine_.is_logged_in()) {
    collab_engine_.query_history(current_peer_, from_ms, until_ms);
  }
}

// 清除筛选恢复全量（幂等：无筛选即无动作；重渲含跨态标记与引导，与
// open_chat 同构）。
void MainWindow::clear_date_filter() {
  if (filter_from_ms_ == 0 && filter_until_ms_ == 0) return;
  filter_from_ms_ = 0;
  filter_until_ms_ = 0;
  render_open_history();
  show_status(QStringLiteral("已清除日期筛选"));
}

// 跳转到目标日首条：先按当日窗向服务端补拉（协作/群登录态；本地缺的行
// 到达后重渲再定位），本地全量重渲（保持上下文完整）后锚点滚动。
void MainWindow::jump_to_date(const QDate& date) {
  if (current_peer_.isEmpty() || chat_panel_->isHidden()) {
    show_status(QStringLiteral("先选择会话再按日期查看"));
    return;
  }
  if (!date.isValid()) return;
  pending_jump_ = date;
  if ((current_kind_ == QStringLiteral("collab") ||
       current_kind_ == QStringLiteral("group")) &&
      collab_engine_.is_logged_in()) {
    collab_engine_.query_history(current_peer_, day_start_ms(date),
                                 day_end_ms(date));
  }
  render_open_history();
  scroll_to_day(date);
  show_status(QStringLiteral("已跳转到 %1")
                  .arg(date.toString(QStringLiteral("yyyy-MM-dd"))));
}

void MainWindow::scroll_to_day(const QDate& date) {
  if (!date.isValid()) return;
  chat_view_->scrollToAnchor(QStringLiteral("d") +
                              date.toString(QStringLiteral("yyyyMMdd")));
}

// 「按日期」对话框：日历选择起始/结束日；跳转（滚到起始日首条）、
// 范围筛选（只看窗内记录）、清除筛选三动作。
void MainWindow::show_date_dialog() {
  if (current_peer_.isEmpty() || chat_panel_->isHidden()) {
    show_status(QStringLiteral("先选择会话再按日期查看"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("按日期查看聊天记录"));
  auto* layout = new QVBoxLayout(&dlg);
  auto* form = new QFormLayout;
  auto* from_edit = new QDateEdit(QDate::currentDate(), &dlg);
  from_edit->setCalendarPopup(true);
  from_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  auto* until_edit = new QDateEdit(QDate::currentDate(), &dlg);
  until_edit->setCalendarPopup(true);
  until_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
  form->addRow(QStringLiteral("起始"), from_edit);
  form->addRow(QStringLiteral("结束"), until_edit);
  layout->addLayout(form);
  layout->addWidget(new QLabel(
      QStringLiteral("跳转＝滚动到起始日第一条；筛选＝只看日期范围内的记录"
                     "（协作/群会话自动向服务端补拉）"),
      &dlg));
  auto* actions = new QHBoxLayout;
  auto* jump_btn = new QPushButton(QStringLiteral("跳转到起始日"), &dlg);
  auto* filter_btn = new QPushButton(QStringLiteral("按范围筛选"), &dlg);
  auto* clear_btn = new QPushButton(QStringLiteral("清除筛选"), &dlg);
  auto* close_btn = new QPushButton(QStringLiteral("关闭"), &dlg);
  // 清除只在筛选态可用（无筛选可清）
  clear_btn->setEnabled(filter_from_ms_ > 0 || filter_until_ms_ > 0);
  connect(jump_btn, &QPushButton::clicked, this, [&] {
    dlg.accept();
    jump_to_date(from_edit->date());
  });
  connect(filter_btn, &QPushButton::clicked, this, [&] {
    dlg.accept();
    apply_date_filter(day_start_ms(from_edit->date()),
                      day_end_ms(until_edit->date()));
  });
  connect(clear_btn, &QPushButton::clicked, this, [&] {
    dlg.accept();
    clear_date_filter();
  });
  connect(close_btn, &QPushButton::clicked, &dlg, &QDialog::reject);
  actions->addWidget(jump_btn);
  actions->addWidget(filter_btn);
  actions->addWidget(clear_btn);
  actions->addStretch();
  actions->addWidget(close_btn);
  layout->addLayout(actions);
  dlg.exec();
}

void MainWindow::append_message(const QString& from_id, const QString& text,
                                qint64 ts_ms, bool outgoing,
                                const QString& source, const QString& msg_id,
                                const QString& receipt, quint64 seq) {
  QString name = from_id;
  if (!outgoing) {
    if (current_kind_ == QStringLiteral("collab") ||
        current_kind_ == QStringLiteral("group")) {
      name = from_id; // 协作单聊／群聊对端即账号（群内显账号便于 @）
    } else {
      const Peer p = direct_engine_.peer(from_id.toStdString());
      if (!p.name.empty()) name = QString::fromStdString(p.name);
    }
  } else {
    name = QStringLiteral("我");
  }
  // 振屏标记（需求批⑥）：不进气泡，渲染居中系统行（实时与历史重载同构）。
  // 受控标记口径（同文字颜色〔#RRGGBB〕）：普通文本恰为 "[振屏]" 亦按
  // 振屏渲染——两侧落库一致，副作用可接受。
  if (text == QStringLiteral("[振屏]")) {
    const QString line =
        outgoing ? QStringLiteral("你发送了窗口抖动")
                 : QStringLiteral("%1 给你发来窗口抖动").arg(esc(name));
    append_system_line(line);
    return;
  }
  // 来源字段（合并展示）：直连＝仅本机；协作＝服务端归档
  const QString tag =
      source == QStringLiteral("collab")
          ? QStringLiteral(" · 协作·已归档")
          : QStringLiteral(" · 直连·仅本机");
  // 群聊启用 @成员 高亮（T4.1）
  const bool at_mode = current_kind_ == QStringLiteral("group");
  ChatRow row;
  row.system = false;
  row.name = name + tag;
  row.text = text;
  row.ts_ms = ts_ms;
  row.outgoing = outgoing;
  row.at_mode = at_mode;
  // 群主标签（原型 .msg-meta .role）：群内收向行且发送者＝群主
  row.owner_tag =
      at_mode && !outgoing &&
      from_id == groups_.value(current_group_id()).owner;
  row.msg_id = msg_id;
  row.seq = seq;
  row.receipt = receipt;
  // 「盯」行键（「盯」单·实现单修订）：仅我发出的 1:1 文本行可盯——
  // 直连按 (对端, 序号)、协作按 msg_id（回执同口径键）；群/临时群/图片/
  // 收向行恒空＝右键不弹盯菜单
  if (outgoing) {
    if (current_kind_ == QStringLiteral("collab") && !msg_id.isEmpty()) {
      row.watch_key = collab_watch_key(msg_id);
    } else if (current_kind_ == QStringLiteral("direct") && seq != 0) {
      row.watch_key = direct_watch_key(current_peer_, seq);
    }
  }
  if (needs_day_divider(chat_rows_, row.ts_ms))
    append_system_line(day_divider_html(
        QDateTime::fromMSecsSinceEpoch(row.ts_ms).date()));
  chat_rows_.append(row);
  chat_view_->append(day_anchor(row.ts_ms) +
                     bubble_html(row.name, row.text, row.ts_ms, row.outgoing,
                                 row.at_mode, QString(), row.receipt,
                                 row.watch_key,
                                 watched_.contains(row.watch_key),
                                 row.owner_tag));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::append_image_message(const QString& from_id,
                                      const QString& image_path, qint64 ts_ms,
                                      bool outgoing, const QString& source) {
  QString name = from_id;
  if (outgoing) {
    name = QStringLiteral("我");
  } else if (current_kind_ != QStringLiteral("collab") &&
             current_kind_ != QStringLiteral("group")) {
    const Peer p = direct_engine_.peer(from_id.toStdString());
    if (!p.name.empty()) name = QString::fromStdString(p.name);
  }
  const QString tag =
      source == QStringLiteral("collab")
          ? QStringLiteral(" · 协作·已归档")
          : QStringLiteral(" · 直连·仅本机");
  ChatRow row;
  row.system = false;
  row.name = name + tag;
  row.text = image_path; // 文本位存路径（图片渲染在 image 分支）
  row.image = image_path;
  row.ts_ms = ts_ms;
  row.outgoing = outgoing;
  row.at_mode = false;
  if (needs_day_divider(chat_rows_, row.ts_ms))
    append_system_line(day_divider_html(
        QDateTime::fromMSecsSinceEpoch(row.ts_ms).date()));
  chat_rows_.append(row);
  chat_view_->append(day_anchor(row.ts_ms) +
                     bubble_html(row.name, row.text, row.ts_ms, row.outgoing,
                                 row.at_mode, row.image));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::append_file_card(const QString& from_id,
                                  const QString& file_name,
                                  const QString& file_meta, qint64 ts_ms,
                                  bool outgoing) {
  QString name = from_id;
  if (outgoing) {
    name = QStringLiteral("我");
  } else if (current_kind_ != QStringLiteral("collab") &&
             current_kind_ != QStringLiteral("group")) {
    const Peer p = direct_engine_.peer(from_id.toStdString());
    if (!p.name.empty()) name = QString::fromStdString(p.name);
  }
  ChatRow row;
  row.system = false;
  row.name = name;
  row.text = file_name;
  row.file_card = true;
  row.file_meta = file_meta;
  row.ts_ms = ts_ms;
  row.outgoing = outgoing;
  if (needs_day_divider(chat_rows_, row.ts_ms))
    append_system_line(day_divider_html(
        QDateTime::fromMSecsSinceEpoch(row.ts_ms).date()));
  chat_rows_.append(row);
  chat_view_->append(day_anchor(row.ts_ms) +
                     file_bubble_html(row.name, file_name, file_meta, ts_ms,
                                      outgoing));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
  update_group_panel(); // 群右栏「群文件」随会话内文件卡增长
}

void MainWindow::append_system_line(const QString& text) {
  ChatRow row;
  row.system = true;
  row.text = text;
  chat_rows_.append(row);
  chat_view_->append(system_line_html(text));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

// 居中胶囊系统行（原型 .sys-hint .pill）：跨态/归档提示用——语义仍是系统行
//（不参与日序判定），但重渲按 chip 样式走
void MainWindow::append_chip_line(const QString& text) {
  ChatRow row;
  row.system = true;
  row.chip = true;
  row.text = text;
  chat_rows_.append(row);
  chat_view_->append(chip_line_html(text));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

bool MainWindow::needs_day_divider(const QVector<ChatRow>& rows,
                                   qint64 ts_ms) {
  if (ts_ms <= 0) return false;
  const QDate d = QDateTime::fromMSecsSinceEpoch(ts_ms).date();
  for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
    if (it->system || it->ts_ms <= 0) continue;
    return QDateTime::fromMSecsSinceEpoch(it->ts_ms).date() != d;
  }
  return true;
}

void MainWindow::show_guidance() {
  chat_showing_guidance_ = true;
  chat_rows_.clear();
  chat_view_->clear();
  render_guidance();
}

void MainWindow::render_guidance() {
  const ThemeTokens& t = tk();
  chat_view_->append(QStringLiteral(
      "<div align=\"center\" style=\"margin-top:48px;\">"
      "<span style=\"color:%1;\">尚未发现同网段设备。</span><br><br>"
      "<span style=\"color:%2; font-size:small;\">"
      "请确认对方已安装 Memex 且与本机同一局域网；<br>"
      "直连发现使用 UDP 2425、点对点传输使用 TCP 2426–2437，"
      "请检查终端防火墙放行。<br><br>"
      "如需组织架构、云端历史与归档检索，请经左上角「协作」菜单登录协作态。"
      "</span></div>")
                         .arg(t.text.name(), t.text_muted.name()));
}

// 登录后零会话欢迎页（原型 main-empty.html .first-guide）。与发现引导
// 分开：已登录用户看到「请登录协作态」是误导文案，替换为原型欢迎口径。
// 布局令（2026-10-08）：列表态窗口坍缩 320px、主区隐藏——本页呈潜伏态，
// 文案先行就位（台账近似项见 prototype-conformance.md main-empty 行）。
void MainWindow::show_welcome_guide() {
  chat_showing_guidance_ = true;
  chat_rows_.clear();
  chat_view_->clear();
  render_welcome_guide();
}

void MainWindow::render_welcome_guide() {
  const ThemeTokens& t = tk();
  // 原型 .guide-mark ✦ 字形未在抓图环境验证，以 ● 代（豆腐块纪律）；
  // 动作钮「打开通讯录/查找局域网设备」与四张链接卡为原型交互件，QTextEdit
  // 富文本不可点——不造假控件，链接卡以说明文案呈现（左侧 rail 均有真钮）
  chat_view_->append(QStringLiteral(
      "<div align=\"center\" style=\"margin-top:56px;\">"
      "<table align=\"center\" cellspacing=\"0\" cellpadding=\"6\">"
      "<tr><td width=\"40\" align=\"center\" style=\"background:%1;"
      " color:%2; font-size:20px;\"><b>●</b></td></tr></table>"
      "<span style=\"font-size:20px; font-weight:600; color:%3;\">"
      "欢迎使用 Memex</span><br><br>"
      "<span style=\"color:%4; font-size:13px;\">"
      "你的会话会显示在这里。先从通讯录找到同事，"
      "或打开文件传输，开始使用 Memex。</span><br><br>"
      "<span style=\"color:%5; font-size:12px; line-height:1.8;\">"
      "通讯录 · 查找组织成员与联系人<br>"
      "收藏 · 保存常用消息与内容<br>"
      "文件 · 查看文件收发与传输记录<br>"
      "设置 · 调整主题、通知与快捷键</span></div>")
                         .arg(t.brand_wash.name(), t.brand_text.name(),
                              t.text.name(), t.text_muted.name(),
                              t.text_muted.name()));
}

// 聊天区重渲：主题切换后富文本里的内联色不会自动跟随，故按记录重放。
// 只重渲当前会话已有的行（记录在 chat_rows_），不重新查库。
void MainWindow::rerender_chat() {
  if (!chat_view_) return;
  if (chat_showing_guidance_) {
    chat_view_->clear();
    // 引导页随登录态重推导（与 refresh_devices 空态分支同口径）
    if (collab_engine_.is_logged_in()) {
      render_welcome_guide();
    } else {
      render_guidance();
    }
    return;
  }
  const bool at_bottom =
      chat_view_->verticalScrollBar()->value() >=
      chat_view_->verticalScrollBar()->maximum() - 4;
  chat_view_->clear();
  for (const ChatRow& row : chat_rows_) {
    if (row.system) {
      chat_view_->append(row.chip ? chip_line_html(row.text)
                                  : system_line_html(row.text));
    } else if (row.file_card) {
      chat_view_->append(day_anchor(row.ts_ms) +
                         file_bubble_html(row.name, row.text, row.file_meta,
                                          row.ts_ms, row.outgoing));
    } else {
      chat_view_->append(day_anchor(row.ts_ms) +
                         bubble_html(row.name, row.text, row.ts_ms,
                                     row.outgoing, row.at_mode, row.image,
                                     row.receipt, row.watch_key,
                                     watched_.contains(row.watch_key),
                                     row.owner_tag));
    }
  }
  if (at_bottom) {
    auto* bar = chat_view_->verticalScrollBar();
    bar->setValue(bar->maximum());
  }
}

} // namespace memex::client
