// 「盯」提醒（LAY-002 拆分）：盯住/解除、超时巡检、列表对话框
//（排序小工具 watched_ordered 同收此处）。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include "message_search_dialog.hpp"
#include <QSystemTrayIcon>
#include <core/local_store.hpp>
namespace memex::client {

// —— 「盯」提醒（todo「盯」单·实现单修订 2026-10-14）——

bool MainWindow::watch_current_row(const QString& key) {
  // 右键菜单与测试同一路径。已读拒绝创建（盯与已读同生命周期）；重复
  // 盯同键＝刷新（UPSERT）而非报错
  if (key.isEmpty()) return false;
  const ChatRow* row = nullptr;
  for (const ChatRow& r : chat_rows_) {
    if (!r.system && r.watch_key == key) {
      row = &r;
      break;
    }
  }
  if (row == nullptr) {
    show_status(QStringLiteral("盯失败：消息不在当前会话"));
    return false;
  }
  if (row->receipt == QStringLiteral("read")) {
    show_status(QStringLiteral("该消息已被读，无需盯"));
    return false;
  }
  LocalStore* store = direct_engine_.store();
  if (store == nullptr) {
    show_status(QStringLiteral("盯失败：本地库未就绪"));
    return false;
  }
  WatchedRow w;
  w.key = key.toStdString();
  w.peer = current_peer_.toStdString();
  w.source = current_kind_.toStdString();
  w.text = row->text.left(24).toStdString(); // 提醒预览（超长截断）
  w.created_ms = QDateTime::currentMSecsSinceEpoch();
  if (!store->add_watch(w)) {
    show_status(QStringLiteral("盯失败：本地库写入失败"));
    return false;
  }
  watched_.insert(key, w);
  rerender_chat(); // 气泡 meta 行长出灰 ◉
  show_status(QStringLiteral(
                  "已盯住本消息：对方已读或未读超 %1 分钟时提醒")
                  .arg(watch_remind_ms() / 60000));
  return true;
}

void MainWindow::unwatch_key(const QString& key) {
  // 手动解除（菜单「取消盯」同路径）；未盯的键静默幂等
  if (watched_.remove(key) == 0) return;
  if (LocalStore* store = direct_engine_.store()) store->remove_watch(key.toStdString());
  rerender_chat(); // 摘 ◉
  show_status(QStringLiteral("已取消盯住"));
}

void MainWindow::watch_read_fired(const QString& key, const QString& reader) {
  // 已读＝静默自动解除（实现单修订 2026-10-14：对方一打开就提醒是「已读
  // 通知」不是「盯」——提醒只走未读超时一路）。生产路径＝message_read／
  // receipts_received 信号驱动；盯住集合无此键＝普通已读，静默穿过
  Q_UNUSED(reader);
  const auto it = watched_.constFind(key);
  if (it == watched_.constEnd()) return;
  const WatchedRow w = it.value();
  watched_.erase(it);
  if (LocalStore* store = direct_engine_.store()) {
    store->remove_watch(w.key);
  }
  ++watch_released_;
  rerender_chat(); // 摘 ◉（无任何用户可见输出）
}

void MainWindow::sweep_watches() {
  LocalStore* store = direct_engine_.store();
  if (store == nullptr) return;
  // 以库为源巡检（内存 watched_ 只是界面侧快速集）：双窗口同库、外部
  // 改写 created_ms（测试回拨）、重启后遗留行都按库面现值裁决
  const QList<WatchedRow> rows = store->watched_all();
  if (rows.isEmpty()) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  QStringList read_fire;      // 离线期间已变已读的（补解除）
  QStringList remind_unread;  // 未读超阈值提醒（只一次；阈值可配，默认 10 分钟）
  QStringList expired;        // 超时静默过期（默认 24h，可配）
  for (const WatchedRow& w : rows) {
    // 已读对账：READ 到达前盯行已存在（离线期/补拉场景）——receipt 落库
    // 而 watch 未解除即在此补触发（与信号路径同一出口）
    std::string receipt;
    if (w.source == "collab") {
      receipt = store->receipt_of_msg_id(w.key.substr(2));
    } else {
      // 直连键 d:<收件人>:<序号>（对端不含冒号，取最后一段）
      const QString qkey = QString::fromStdString(w.key);
      const qsizetype cut = qkey.lastIndexOf(QLatin1Char(':'));
      const QString peer = cut > 1 ? qkey.mid(2, cut - 2) : QString();
      const quint64 seq = cut > 1 ? qkey.mid(cut + 1).toULongLong() : 0;
      receipt = (!peer.isEmpty() && seq != 0)
                    ? store->receipt_of_seq(peer.toStdString(), seq)
                    : std::string();
    }
    const QString key = QString::fromStdString(w.key);
    if (receipt == "read") {
      read_fire.append(key);
    } else if (!w.reminded && now - w.created_ms >= watch_remind_ms()) {
      remind_unread.append(key);
    } else if (now - w.created_ms >= watch_expire_ms()) {
      expired.append(key);
    }
  }
  for (const QString& key : read_fire) {
    watch_read_fired(key); // 离线补解除不标读者名（静默）
  }
  for (const QString& key : remind_unread) {
    WatchedRow w;
    for (const WatchedRow& r : rows) {
      if (r.key == key.toStdString()) {
        w = r;
        break;
      }
    }
    store->mark_watch_reminded(w.key); // 只一次打点（先打点防重入轰炸）
    ++watch_timeout_;
    last_watch_notify_ =
        QStringLiteral("盯住的消息仍未读：%1")
            .arg(QString::fromStdString(w.text));
    tray_notify(QStringLiteral("盯住的消息未读提醒"), last_watch_notify_);
    show_status(last_watch_notify_);
    watch_reminded_peers_.insert(QString::fromStdString(w.peer));
    refresh_devices(); // 列表高亮（◉ 前缀，复用未读角标位；打开该会话即清）
  }
  for (const QString& key : expired) {
    watched_.remove(key);
    store->remove_watch(key.toStdString());
    ++watch_expired_; // 超时解除不通知（防僵尸盯）
  }
  if (!expired.isEmpty()) rerender_chat();
}

void MainWindow::load_watches() {
  watched_.clear();
  if (LocalStore* store = direct_engine_.store()) {
    for (const WatchedRow& w : store->watched_all()) {
      watched_.insert(QString::fromStdString(w.key), w);
    }
  }
}

namespace {

// 盯行按创建时刻排序（快照与列表展示的稳定序）
QList<WatchedRow> watched_ordered(const QHash<QString, WatchedRow>& watched) {
  QList<WatchedRow> rows = watched.values();
  std::sort(rows.begin(), rows.end(),
            [](const WatchedRow& a, const WatchedRow& b) {
              return a.created_ms < b.created_ms;
            });
  return rows;
}

} // namespace

QString MainWindow::watch_row_label(const WatchedRow& w) const {
  // 「我在盯哪些」一行：域 · 对端名（解析不出用短 id）· 预览 · 已盯时长
  const QString key = QString::fromStdString(w.key);
  const QString domain = w.source == "collab" ? QStringLiteral("协作")
                                              : QStringLiteral("直连");
  QString peer_name = QString::fromStdString(w.peer);
  if (w.source != "collab") {
    const Peer p = direct_engine_.peer(w.peer);
    if (!p.name.empty()) peer_name = QString::fromStdString(p.name);
  } else {
    peer_name = key.mid(2); // 协作键 c:<会话 id> 即对端账号
  }
  const qint64 held_min = (QDateTime::currentMSecsSinceEpoch() - w.created_ms +
                           60000 - 1) / 60000;
  return QStringLiteral("%1 · %2 · %3 · 已盯 %4 分钟")
      .arg(domain, peer_name, QString::fromStdString(w.text),
           QString::number(held_min));
}

QStringList MainWindow::watch_list_snapshot() const {
  QStringList out;
  for (const WatchedRow& w : watched_ordered(watched_)) {
    out.append(watch_row_label(w));
  }
  return out;
}

bool MainWindow::cancel_watch_by_index(int index) {
  const QList<WatchedRow> rows = watched_ordered(watched_);
  if (index < 0 || index >= rows.size()) return false;
  unwatch_key(QString::fromStdString(rows[index].key));
  return true;
}

void MainWindow::open_watch_list() {
  // 盯列表（实现单修订 2026-10-14：要有入口看到「我在盯哪些」）：非模态
  // 对话框，逐条可取消；空态给一行说明
  if (watch_list_dialog_ == nullptr) {
    watch_list_dialog_ = new QDialog(this);
    watch_list_dialog_->setAttribute(Qt::WA_DeleteOnClose);
    watch_list_dialog_->setWindowTitle(QStringLiteral("盯列表"));
    auto* lay = new QVBoxLayout(watch_list_dialog_);
    watch_list_widget_ = new QListWidget(watch_list_dialog_);
    watch_list_widget_->setMinimumSize(420, 220);
    lay->addWidget(watch_list_widget_);
    auto* row = new QHBoxLayout();
    auto* cancel_btn = new QPushButton(QStringLiteral("取消盯"), watch_list_dialog_);
    auto* close_btn = new QPushButton(QStringLiteral("关闭"), watch_list_dialog_);
    row->addStretch();
    row->addWidget(cancel_btn);
    row->addWidget(close_btn);
    lay->addLayout(row);
    connect(cancel_btn, &QPushButton::clicked, this, [this] {
      const int cur = watch_list_widget_->currentRow();
      if (cancel_watch_by_index(cur)) refresh_watch_list();
    });
    connect(close_btn, &QPushButton::clicked, watch_list_dialog_, &QDialog::close);
  }
  refresh_watch_list();
  watch_list_dialog_->show();
  watch_list_dialog_->raise();
  watch_list_dialog_->activateWindow();
}

void MainWindow::refresh_watch_list() {
  if (watch_list_widget_ == nullptr) return;
  watch_list_widget_->clear();
  const QStringList entries = watch_list_snapshot();
  if (entries.isEmpty()) {
    watch_list_widget_->addItem(QStringLiteral(
        "暂无盯住的消息——在气泡上右键「盯住本消息」可跟进一条"));
    return;
  }
  watch_list_widget_->addItems(entries);
}

} // namespace memex::client
