// 文件传输面（LAY-002 拆分）：传输台账入账、进度差分、传输卡列表
// 重建与发送缝。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QDesktopServices>
#include <QIcon>
#include <QJsonObject>
#include <QProgressBar>
#include <QUrl>
#include <functional>
#include "theme.hpp"
namespace memex::client {

// 台账入账：同 id 覆盖（进度终态同键回流）；传输面可见即刷新。
void MainWindow::track_transfer(const TransferEntry& e) {
  if (e.id.isEmpty()) return;
  bool replaced = false;
  for (TransferEntry& old : transfers_) {
    if (old.id == e.id) {
      old = e;
      replaced = true;
      break;
    }
  }
  if (!replaced) transfers_.append(e);
  if (sidebar_mode_ == QStringLiteral("transfers")) refresh_transfers_view();
}

// 进度差分：字节累计＋瞬时速度（两次进度事件的字节差/时差）。
void MainWindow::update_tx_progress(const QString& id, quint64 done,
                                    quint64 total) {
  if (id.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (TransferEntry& e : transfers_) {
    if (e.id != id) continue;
    if (e.last_prog_ms > 0 && done > e.bytes_done) {
      const double dt = double(now - e.last_prog_ms);
      if (dt > 0) e.speed_bps = (done - e.bytes_done) * 1000.0 / dt;
    }
    e.bytes_done = done;
    e.bytes_total = total;
    e.last_prog_ms = now;
    break;
  }
  if (sidebar_mode_ == QStringLiteral("transfers")) refresh_transfers_view();
}

// 终态：ok＝已完成（字节收满＋完成时刻）；失败且非取消＝已中断（原因入
// 副注）；已取消的终态回执不改判（用户主动中止，取消即终态语义）。
void MainWindow::finish_tx_entry(const QString& id, bool ok,
                                 const QString& note) {
  if (id.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (TransferEntry& e : transfers_) {
    if (e.id != id) continue;
    if (ok) {
      e.state = 1;
      e.done_ms = now;
      e.speed_bps = 0;
      if (e.bytes_total > 0) e.bytes_done = e.bytes_total;
      if (e.is_folder && e.files_total > 0) e.files_done = e.files_total;
    } else if (e.state != 3) {
      e.state = 2;
      e.note = note;
    }
    break;
  }
  if (sidebar_mode_ == QStringLiteral("transfers")) refresh_transfers_view();
}

// 传输卡对端显示名：协作＝通讯录显示名（空回落账号）；直连＝设备号
//（直连态无显示名映射，同会话列表行口径）。
QString MainWindow::tx_peer_display(const QString& kind,
                                    const QString& peer) const {
  if (kind == QStringLiteral("collab")) {
    const QString d = roster_display(peer);
    return d.isEmpty() ? peer : d;
  }
  return peer;
}

// 侧栏「传输通道」全量重建（行/卡样式内嵌令牌色：主题切换与进入视图都
// 走这里）。通道行＝点对点直传（当前唯一可用通道，active 态）＋共享空间
//（全工程无功能体，行落位但置灰——同 rail 共享空间钮口径，不冒充可用）。
void MainWindow::rebuild_transfer_side() {
  if (!transfer_side_) return;
  auto* lay = qobject_cast<QVBoxLayout*>(transfer_side_->layout());
  if (!lay) return;
  const ThemeTokens& t = tk();
  while (lay->count() > 0) {
    QLayoutItem* it = lay->takeAt(0);
    if (it->widget()) {
      // 先摘画面再延后销毁：DeferredDelete 在嵌套泵里可能迟迟不消化，
      // 旧行残留绘制会盖住重建内容（首代行浅色残影实录）
      it->widget()->hide();
      it->widget()->deleteLater();
    }
    delete it;
  }
  auto* ts_title = new QLabel(QStringLiteral("传输通道"), transfer_side_);
  ts_title->setStyleSheet(QStringLiteral("font-weight:600; font-size:14px; "
                                         "color:%1;")
                              .arg(t.text.name()));
  // 已可见父下新建的子件不随 addWidget 自动显示——逐件显式 show
  ts_title->show();
  lay->addWidget(ts_title);
  // 通道行（原型 .side-list .row：40px 头像 radius 12＋名/副行）
  const auto chan_row = [&t](QWidget* parent, const QString& initial,
                             const QString& name, const QString& sub,
                             bool active) {
    auto* row = new QWidget(parent);
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(8, 6, 8, 6);
    rl->setSpacing(10);
    auto* av = new QLabel(initial, row);
    av->setFixedSize(40, 40);
    av->setAlignment(Qt::AlignCenter);
    // 通道行头像用令牌色（avatar_hue 散色进样式表会撞「样式全令牌」门禁）
    av->setStyleSheet(QStringLiteral("background:%1; color:%2; font-weight:700;"
                                     " font-size:15px; border-radius:12px;")
                          .arg(active ? t.brand_tint.name()
                                      : t.surface_alt.name(),
                               active ? t.brand_text.name()
                                      : t.text_muted.name()));
    rl->addWidget(av, 0, Qt::AlignTop);
    auto* main = new QWidget(row);
    auto* ml = new QVBoxLayout(main);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(2);
    auto* name_l = new QLabel(name, main);
    name_l->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:13px; color:%1;")
            .arg(active ? t.text.name() : t.text_muted.name()));
    ml->addWidget(name_l);
    auto* sub_l = new QLabel(sub, main);
    sub_l->setStyleSheet(QStringLiteral("font-size:11px; color:%1;")
                             .arg(t.text_muted.name()));
    ml->addWidget(sub_l);
    rl->addWidget(main, 1);
    if (active) {
      row->setStyleSheet(QStringLiteral("background:%1; border-radius:10px;")
                             .arg(t.brand_tint.name()));
    }
    return row;
  };
  auto* peer_row = chan_row(transfer_side_, QStringLiteral("⇅"),
                            QStringLiteral("点对点直传"),
                            QStringLiteral("默认 · 不经服务端"), true);
  peer_row->show();
  lay->addWidget(peer_row);
  auto* share_row =
      chan_row(transfer_side_, QStringLiteral("档"), QStringLiteral("共享空间"),
               QStringLiteral("Samba · 未接入"), false);
  share_row->setToolTip(QStringLiteral("共享空间无功能体（本版仅点对点直传）"));
  share_row->show();
  lay->addWidget(share_row);
  // 旁路设计虚线卡（原型同位卡：口径如实——字节流点对点，协作态授权
  // 留痕在服务端，直连态完全离线）
  auto* bypass = new QLabel(
      QStringLiteral("<span style=\"font-weight:700; color:%1;\">旁路设计"
                     "</span>：文件字节流始终在局域网内点对点直传，不经服务"
                     "端；协作态发送前经服务端授权留痕（文件名、大小、哈希、"
                     "收发方）。")
          .arg(t.text.name()),
      transfer_side_);
  bypass->setWordWrap(true);
  bypass->setStyleSheet(
      QStringLiteral("border:1px dashed %1; border-radius:10px; "
                     "padding:11px 12px; font-size:11px; color:%2;")
          .arg(t.surface_alt.name(), t.text_muted.name()));
  bypass->show();
  lay->addWidget(bypass);
  // 伙伴发现品牌卡：N 取发现表实时数（每次进视图/发现表变化刷新）
  tx_peers_hint_ = new QLabel(
      QStringLiteral("本机已发现同网段直传伙伴 %1 台。")
          .arg(static_cast<int>(direct_engine_.peers().size())),
      transfer_side_);
  tx_peers_hint_->setWordWrap(true);
  tx_peers_hint_->setStyleSheet(
      QStringLiteral("background:%1; color:%2; border-radius:10px; "
                     "padding:11px 12px; font-size:11px;")
          .arg(t.brand_wash.name(), t.brand_wash_text.name()));
  tx_peers_hint_->show();
  lay->addWidget(tx_peers_hint_);
  lay->addStretch();
  // 重建后同步结算布局（同 refresh_transfers_view：不等事件泵消化
  // LayoutRequest，新行滞留默认几何实录）
  lay->activate();
}

// 传输列表全量重建：头部 meta（正在传输/今日完成/累计，全部取自真实
// 台账）＋标签（计数＋active 态）＋传输卡（图标/名/副行/状态胶囊/进度条/
// 脚行/操作）。空态与「已暂停」标签如实说明（台账会话内存级；引擎无
// 暂停 API）。
void MainWindow::refresh_transfers_view() {
  if (!tx_list_lay_ || !tx_meta_) return;
  const ThemeTokens& t = tk();
  int doing = 0;
  int done_today = 0;
  quint64 total_bytes = 0;
  const qint64 day_start_ms =
      QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch();
  for (const TransferEntry& e : transfers_) {
    if (e.state == 0) ++doing;
    if (e.state == 1) {
      total_bytes += e.bytes_total;
      if (e.done_ms >= day_start_ms) ++done_today;
    }
  }
  tx_meta_->setText(QStringLiteral("正在传输 %1 · 今日完成 %2 · 累计 %3")
                        .arg(QString::number(doing),
                             QString::number(done_today),
                             human_size(static_cast<qint64>(total_bytes))));
  tx_meta_->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                              .arg(t.text_muted.name()));
  tx_tabs_[1]->setText(QStringLiteral("传输中 %1").arg(doing));
  for (int i = 0; i < 4; ++i) {
    if (!tx_tabs_[i]) continue;
    if (i == transfer_filter_) {
      tx_tabs_[i]->setStyleSheet(
          QStringLiteral("background:%1; color:%2; border:none; "
                         "border-radius:8px; padding:6px 14px; "
                         "font-size:12.5px; font-weight:700;")
              .arg(t.brand_wash.name(), t.brand_text.name()));
    } else {
      tx_tabs_[i]->setStyleSheet(
          QStringLiteral("background:transparent; color:%1; border:none; "
                         "border-radius:8px; padding:6px 14px; "
                         "font-size:12.5px;")
              .arg(t.text_muted.name()));
    }
  }
  // 清空重建（tx_empty_ 常驻复用，卡片销毁重建）。先 hide 再延后销毁：
  // DeferredDelete 在嵌套事件泵里可能迟迟不消化，旧卡残留绘制会盖住重建
  // 内容（首代浅色卡在深色抓图里残影实录）
  while (tx_list_lay_->count() > 0) {
    QLayoutItem* it = tx_list_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      if (w != tx_empty_) {
        w->hide();
        w->deleteLater();
      }
    }
    delete it;
  }
  QList<const TransferEntry*> shown;
  for (const TransferEntry& e : transfers_) {
    const bool pass = transfer_filter_ == 0
                          ? true
                          : transfer_filter_ == 1
                                ? e.state == 0
                                : transfer_filter_ == 2 ? e.state == 1
                                                        : false;
    if (pass) shown.append(&e);
  }
  std::sort(shown.begin(), shown.end(),
            [](const TransferEntry* a, const TransferEntry* b) {
              const bool ad = a->state == 0, bd = b->state == 0;
              if (ad != bd) return ad; // 传输中置顶（原型首卡在传）
              const qint64 ta = a->state == 1 ? a->done_ms : a->ts_ms;
              const qint64 tb = b->state == 1 ? b->done_ms : b->ts_ms;
              return ta > tb; // 新事件在上
            });
  if (shown.isEmpty()) {
    tx_empty_->setText(
        transfer_filter_ == 3
            ? QStringLiteral("本版传输不支持暂停——取消后重发自动断点续传"
                             "（对端保留已传分块与哈希校验点）")
            : QStringLiteral("暂无传输记录（会话内发送/接收的文件在此列出；"
                             "台账为本机本次运行内，重启从零累计）"));
    tx_empty_->setStyleSheet(QStringLiteral("color:%1; font-size:11.5px;")
                                 .arg(t.text_muted.name()));
    tx_empty_->show();
    tx_list_lay_->addWidget(tx_empty_);
    tx_list_lay_->addStretch();
    return;
  }
  const QString op_plain = QStringLiteral(
      "QPushButton { background:transparent; color:%1; border:1px solid %2; "
      "border-radius:6px; padding:2px 9px; font-size:11.5px; }"
      "QPushButton:hover { background:%3; }");
  const QString plain_a =
      op_plain.arg(t.text_muted.name(), t.border.name(), t.surface_alt.name());
  const QString brand_a =
      op_plain.arg(t.brand_text.name(), t.brand_tint.name(),
                   t.brand_wash.name());
  const QString muted_l =
      QStringLiteral("font-size:11.5px; color:%1;").arg(t.text_muted.name());
  for (const TransferEntry* pe : shown) {
    const TransferEntry& e = *pe;
    auto* card = new QFrame(tx_empty_->parentWidget());
    // 纵向封顶＝按内容高：滚动区可视高大于内容总高时，多余空间全让
    // 给尾部 stretch，卡片不被拉高（首版实况被拉成半空高卡实录）
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    card->setStyleSheet(
        QStringLiteral("QFrame { background:%1; border:1px solid %2; "
                       "border-radius:10px; }")
            .arg(t.surface_raised.name(), t.border.name()));
    auto* cl = new QVBoxLayout(card);
    cl->setContentsMargins(15, 13, 15, 13);
    cl->setSpacing(8);
    // 首行：图标＋名/副行＋状态胶囊（原型 .tx-top）
    auto* top = new QHBoxLayout();
    top->setSpacing(10);
    const bool ok_look = e.state == 1;
    auto* icon = new QLabel(
        e.is_folder ? QStringLiteral("▸") : avatar_initial(e.name), card);
    icon->setFixedSize(36, 36);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(
        QStringLiteral("background:%1; color:%2; border-radius:10px; "
                       "font-size:15px; font-weight:800;")
            .arg((e.is_folder || ok_look) ? t.success_wash.name()
                                          : t.brand_tint.name(),
                 (e.is_folder || ok_look) ? t.success.name()
                                          : t.brand_text.name()));
    top->addWidget(icon, 0, Qt::AlignTop);
    auto* tit = new QVBoxLayout();
    tit->setSpacing(1);
    QString name_text = e.name;
    if (e.is_folder) { // 原型 .f-name「目录名/（文件夹 · N 个文件）」
      name_text += QStringLiteral("/（文件夹");
      if (e.files_total > 0)
        name_text += QStringLiteral(" · %1 个文件").arg(e.files_total);
      name_text += QStringLiteral("）");
    }
    auto* name_l = new QLabel(name_text, card);
    name_l->setStyleSheet(
        QStringLiteral("font-weight:700; font-size:13.5px; color:%1;")
            .arg(t.text.name()));
    tit->addWidget(name_l);
    const QString dir_word =
        e.outgoing ? QStringLiteral("发给") : QStringLiteral("来自");
    QString sub;
    if (e.state == 1) {
      sub = QStringLiteral("%1 %2 · %3 · 完成 %4")
                .arg(dir_word, e.peer_display,
                     human_size(static_cast<qint64>(e.bytes_total)),
                     hhmm(e.done_ms));
    } else if (e.state == 2) {
      sub = QStringLiteral("%1 %2 · %3")
                .arg(dir_word, e.peer_display,
                     e.note.isEmpty() ? QStringLiteral("传输中断") : e.note);
    } else if (e.state == 3) {
      sub = QStringLiteral("%1 %2 · 已取消").arg(dir_word, e.peer_display);
    } else {
      sub = QStringLiteral("%1 %2 · 直传中").arg(dir_word, e.peer_display);
    }
    auto* sub_l = new QLabel(sub, card);
    sub_l->setStyleSheet(muted_l);
    tit->addWidget(sub_l);
    top->addLayout(tit, 1);
    QString pill_text;
    QColor pill_fg, pill_bg;
    switch (e.state) {
      case 1:
        pill_text = QStringLiteral("已完成");
        pill_fg = t.success;
        pill_bg = t.success_wash;
        break;
      case 2:
        pill_text = QStringLiteral("已中断");
        pill_fg = t.warning;
        pill_bg = t.warning_wash;
        break;
      case 3:
        pill_text = QStringLiteral("已取消");
        pill_fg = t.text_muted;
        pill_bg = t.surface_alt;
        break;
      default:
        pill_text = QStringLiteral("传输中");
        pill_fg = t.brand_text;
        pill_bg = t.brand_tint;
    }
    auto* pill = new QLabel(pill_text, card);
    pill->setStyleSheet(
        QStringLiteral("color:%1; background:%2; border-radius:999px; "
                       "padding:1px 8px; font-size:10.5px; font-weight:700;")
            .arg(pill_fg.name(), pill_bg.name()));
    top->addWidget(pill, 0, Qt::AlignTop);
    cl->addLayout(top);
    // 进度条（原型 .tx-bar：6px；完成＝ok 色）
    int pct = 0;
    if (e.bytes_total > 0)
      pct = static_cast<int>(e.bytes_done * 100 / e.bytes_total);
    if (e.state == 1) pct = 100;
    auto* bar = new QProgressBar(card);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    bar->setRange(0, 100);
    bar->setValue(pct);
    bar->setStyleSheet(
        QStringLiteral("QProgressBar { background:%1; border:1px solid %2; "
                       "border-radius:3px; }"
                       "QProgressBar::chunk { background:%3; "
                       "border-radius:3px; }")
            .arg(t.surface_alt.name(), t.border.name(),
                 e.state == 1 ? t.success.name() : t.brand.name()));
    cl->addWidget(bar);
    // 脚行：字节/速度/文件粒度＋说明＋操作（原型 .tx-foot/.ops）
    auto* foot = new QHBoxLayout();
    foot->setSpacing(10);
    QString left;
    if (e.is_folder) {
      left = QStringLiteral("文件夹 · 已传 %1/%2 个文件")
                 .arg(QString::number(e.files_done),
                      QString::number(e.files_total));
    } else if (e.bytes_total > 0) {
      left = QStringLiteral("%1 / %2 · %3%")
                 .arg(human_size(static_cast<qint64>(e.bytes_done)),
                      human_size(static_cast<qint64>(e.bytes_total)),
                      QString::number(pct));
    } else {
      left = QStringLiteral("待对端上报大小");
    }
    auto* left_l = new QLabel(left, card);
    left_l->setStyleSheet(muted_l);
    foot->addWidget(left_l);
    if (e.state == 0 && e.speed_bps > 0 && e.bytes_total > e.bytes_done) {
      const double remain_s =
          double(e.bytes_total - e.bytes_done) / e.speed_bps;
      const QString remain =
          remain_s >= 60
              ? QStringLiteral("%1 分 %2 秒")
                    .arg(int(remain_s / 60))
                    .arg(int(remain_s) % 60)
              : QStringLiteral("%1 秒").arg(int(remain_s) + 1);
      auto* speed_l =
          new QLabel(QStringLiteral("%1/s · 剩余 %2")
                         .arg(human_size(static_cast<qint64>(e.speed_bps)),
                              remain),
                     card);
      speed_l->setStyleSheet(muted_l);
      foot->addWidget(speed_l);
    }
    if (e.state == 1) {
      // 收方整文件 SHA-256 校验通过才落盘/回执——双向都可如实标校验
      auto* ok_l = new QLabel(
          e.outgoing ? QStringLiteral("✓ 接收方已确认 · SHA-256 已校验")
                     : QStringLiteral("✓ 已保存至本机 · SHA-256 已校验"),
          card);
      ok_l->setStyleSheet(
          QStringLiteral("font-size:11.5px; color:%1;").arg(t.success.name()));
      foot->addWidget(ok_l);
    }
    if (e.state == 3) {
      auto* c_l =
          new QLabel(QStringLiteral("对端保留已传分块，重发自动续传"), card);
      c_l->setStyleSheet(muted_l);
      foot->addWidget(c_l);
    }
    foot->addStretch();
    // 操作：描边小钮（品牌色＝打开/打开目录）；取消走引擎中止＋台账改判
    const auto op_btn = [&](const QString& text, bool brand,
                            const std::function<void()>& on_click) {
      auto* b = new QPushButton(text, card);
      b->setCursor(Qt::PointingHandCursor);
      b->setStyleSheet(brand ? brand_a : plain_a);
      connect(b, &QPushButton::clicked, this, on_click);
      foot->addWidget(b);
    };
    if (e.state == 0) {
      op_btn(QStringLiteral("取消"), false, [this, id = e.id] {
        for (TransferEntry& en : transfers_) {
          if (en.id == id && en.state == 0) {
            en.state = 3;
            en.note = QStringLiteral("已取消");
          }
        }
        direct_engine_.cancel_transfer(id.toStdString());
        refresh_transfers_view();
      });
      if (e.outgoing && !e.path.isEmpty()) {
        op_btn(QStringLiteral("打开目录"), true, [path = e.path] {
          QDesktopServices::openUrl(QUrl::fromLocalFile(
              QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
        });
      }
    } else if (e.state == 1) {
      if (!e.path.isEmpty()) {
        const QString label = (e.is_folder || !e.outgoing)
                                  ? QStringLiteral("打开目录")
                                  : QStringLiteral("打开");
        op_btn(label, true, [is_folder = e.is_folder, path = e.path] {
          QDesktopServices::openUrl(QUrl::fromLocalFile(
              is_folder ? path : QFileInfo(path).absolutePath()));
        });
      }
      op_btn(QStringLiteral("所在会话"), false,
             [this, kind = e.kind, peer = e.peer] {
               leave_transfers_view();
               open_chat(kind, peer);
             });
    }
    cl->addLayout(foot);
    // 已可见父下新建的子件不随 addWidget 自动显示——显式 show
    card->show();
    tx_list_lay_->addWidget(card);
  }
  tx_list_lay_->addStretch();
  // 重建后同步结算布局：新增卡在脏布局上等 LayoutRequest 事件，嵌套事件
  // 泵里可能迟消化——曾实录第二张卡滞留默认几何 (0,0 100x30) 不入画
  tx_list_lay_->activate();
}

// 传输台账验收面：逐条 JSON（id/名/形态/对端/状态/方向/文件夹/字节）。
QString MainWindow::transfers_json() const {
  QJsonArray arr;
  for (const TransferEntry& e : transfers_) {
    QJsonObject o;
    o.insert(QStringLiteral("id"), e.id);
    o.insert(QStringLiteral("name"), e.name);
    o.insert(QStringLiteral("kind"), e.kind);
    o.insert(QStringLiteral("peer"), e.peer);
    o.insert(QStringLiteral("state"), e.state);
    o.insert(QStringLiteral("outgoing"), e.outgoing);
    o.insert(QStringLiteral("folder"), e.is_folder);
    o.insert(QStringLiteral("bytes"), QString::number(e.bytes_total));
    arr.append(o);
  }
  QJsonObject root;
  root.insert(QStringLiteral("transfers"), arr);
  return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// 发文件发送缝（传输面「发送文件」钮与聊天发文件钮同一路径）：守卫→
// 直连通道发送→台账跟踪＋会话内文件卡。
QString MainWindow::transfer_send_file(const QString& path) {
  if (current_peer_.isEmpty()) {
    show_status(QStringLiteral("先选择设备再发送文件"));
    return {};
  }
  if (current_kind_ == QStringLiteral("group") ||
      current_kind_ == QStringLiteral("dgroup")) {
    // 群文件传输不在本期范围：文件仍走点对点直连（单聊）
    show_status(QStringLiteral("群会话暂不支持文件发送（文件走单聊点对点）"));
    return {};
  }
  if (current_kind_ != QStringLiteral("collab") && !direct_send_allowed()) {
    return {}; // T3.4 策略闸门（文案策略面已给）
  }
  const std::string tid =
      direct_engine_.send_file(current_peer_.toStdString(), path);
  if (tid.empty()) {
    show_status(QStringLiteral("文件发送失败"));
    return {};
  }
  const QString id = QString::fromStdString(tid);
  const QString name = QFileInfo(path).fileName();
  file_sent_.insert(name, 0);
  TransferEntry e;
  e.id = id;
  e.name = name;
  e.kind = current_kind_;
  e.peer = current_peer_;
  e.peer_display = tx_peer_display(e.kind, e.peer);
  e.path = path;
  e.bytes_total = static_cast<quint64>(QFileInfo(path).size());
  e.ts_ms = QDateTime::currentMSecsSinceEpoch();
  track_transfer(e);
  // 文件卡气泡（原型 .bubble.file-card）：协作单聊＝传输元数据入归档，
  // 直连/跨态＝仅双方本机保存（文案如实随形态，不虚构归档）
  append_file_card(
      QString(), name,
      QStringLiteral("%1 · 直传 · %2")
          .arg(human_size(QFileInfo(path).size()),
               (current_kind_ == QStringLiteral("collab") &&
                collab_engine_.is_logged_in())
                   ? QStringLiteral("传输元数据已入归档")
                   : QStringLiteral("仅双方本机保存")),
      QDateTime::currentMSecsSinceEpoch(), /*outgoing=*/true);
  show_status(QStringLiteral("文件发送中：%1").arg(name));
  return id;
}

} // namespace memex::client
