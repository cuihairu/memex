#include "main_window.hpp"

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QStatusBar>
#include <QVBoxLayout>

#include <core/local_store.hpp>

namespace memex::client {

namespace {

QString esc(const QString& s) {
  QString out = s;
  out.replace(QChar('&'), QStringLiteral("&amp;"));
  out.replace(QChar('<'), QStringLiteral("&lt;"));
  out.replace(QChar('>'), QStringLiteral("&gt;"));
  out.replace(QChar('"'), QStringLiteral("&quot;"));
  return out;
}

QString hhmm(qint64 ts_ms) {
  return QDateTime::fromMSecsSinceEpoch(ts_ms).toString(QStringLiteral("HH:mm"));
}

// 气泡行：外出＝品牌橙靠右，来访＝浅灰靠左（QTextBrowser 富文本子集，不用圆角）
QString bubble_html(const QString& name, const QString& text, qint64 ts_ms,
                    bool outgoing) {
  const QString meta =
      QStringLiteral("<span style=\"color:#9b8f86; font-size:small;\">%1 %2</span>")
          .arg(esc(name), hhmm(ts_ms));
  const QString body = QStringLiteral(
                           "<table cellspacing=\"0\" cellpadding=\"6\"><tr><td "
                           "bgcolor=\"%1\"><span style=\"color:%2;\">%3</span></td>"
                           "</tr></table>")
                           .arg(outgoing ? QStringLiteral("#e16531")
                                         : QStringLiteral("#f0ebe5"),
                                outgoing ? QStringLiteral("#ffffff")
                                         : QStringLiteral("#332b24"),
                                esc(text));
  if (outgoing) {
    return QStringLiteral(
               "<div>%1</div><table width=\"100%\" cellspacing=\"0\"><tr>"
               "<td width=\"28%\"></td><td align=\"right\">%2</td></tr></table>")
        .arg(meta, body);
  }
  return QStringLiteral(
             "<div>%1</div><table width=\"100%\" cellspacing=\"0\"><tr>"
             "<td align=\"left\">%2</td><td width=\"28%\"></td></tr></table>")
      .arg(meta, body);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Memex"));
  resize(960, 640);

  build_ui();

  // 两套引擎同起：协作引擎未登录只待机，直连引擎默认可用
  wire_engines();
  direct_engine_.start();
  collab_engine_.start();

  refresh_devices();
  show_status(QStringLiteral("就绪"));
}

void MainWindow::build_ui() {
  // —— 左侧：局域网设备列表（直连态以设备替代联系人）——
  auto* side = new QWidget(this);
  auto* side_layout = new QVBoxLayout(side);
  side_layout->setContentsMargins(10, 10, 6, 10);
  side_layout->setSpacing(6);

  auto* side_head = new QHBoxLayout();
  auto* side_title = new QLabel(QStringLiteral("局域网设备"), side);
  side_title->setStyleSheet(
      QStringLiteral("font-weight:600; font-size:14px; color:#332b24;"));
  device_count_ = new QLabel(side);
  device_count_->setStyleSheet(
      QStringLiteral("color:#6f8f6a; font-size:12px;"));
  side_head->addWidget(side_title);
  side_head->addStretch();
  side_head->addWidget(device_count_);

  search_box_ = new QLineEdit(side);
  search_box_->setPlaceholderText(QStringLiteral("搜索设备 / IP"));
  search_box_->setClearButtonEnabled(true);

  device_list_ = new QListWidget(side);
  device_list_->setFrameShape(QFrame::NoFrame);
  device_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  device_list_->setStyleSheet(QStringLiteral(
      "QListWidget { background:#faf7f3; border:1px solid #e8e0d6; "
      "border-radius:8px; }"
      "QListWidget::item { padding:8px; border-bottom:1px solid #efe8de; }"
      "QListWidget::item:selected { background:#f6e3d7; color:#332b24; }"));

  side_layout->addLayout(side_head);
  side_layout->addWidget(search_box_);
  side_layout->addWidget(device_list_, 1);

  // —— 右侧：聊天窗 ——
  auto* chat = new QWidget(this);
  auto* chat_layout = new QVBoxLayout(chat);
  chat_layout->setContentsMargins(0, 0, 0, 0);
  chat_layout->setSpacing(0);

  auto* head = new QWidget(chat);
  head->setStyleSheet(QStringLiteral("background:#ffffff;"));
  auto* head_layout = new QHBoxLayout(head);
  head_layout->setContentsMargins(14, 10, 14, 10);
  chat_title_ = new QLabel(QStringLiteral("直连态"), head);
  chat_title_->setStyleSheet(
      QStringLiteral("font-weight:600; font-size:15px; color:#332b24;"));
  chat_meta_ = new QLabel(QStringLiteral("未选择设备"), head);
  chat_meta_->setStyleSheet(QStringLiteral("color:#9b8f86; font-size:12px;"));
  auto* local_badge = new QLabel(QStringLiteral("本机保存"), head);
  local_badge->setStyleSheet(QStringLiteral(
      "background:#f6e3d7; color:#a05a26; border-radius:8px; padding:2px 8px; "
      "font-size:12px;"));
  head_layout->addWidget(chat_title_);
  head_layout->addWidget(chat_meta_);
  head_layout->addStretch();
  head_layout->addWidget(local_badge);

  // 「未归档」语义标记：常驻不可关（留痕铁律的界面表达）
  auto* banner = new QLabel(
      QStringLiteral("　⚠ 直连态会话：消息点对点传输，不经过服务器，服务端无任何"
                     "记录（仅保存在双方本机）"),
      chat);
  banner->setWordWrap(true);
  banner->setStyleSheet(QStringLiteral(
      "background:#fdeee2; color:#8a4a1f; font-size:12px; padding:6px 10px;"));

  chat_view_ = new QTextBrowser(chat);
  chat_view_->setFrameShape(QFrame::NoFrame);
  chat_view_->setStyleSheet(QStringLiteral(
      "QTextBrowser { background:#ffffff; border:none; }"));

  auto* input_row = new QWidget(chat);
  input_row->setStyleSheet(QStringLiteral("background:#ffffff;"));
  auto* input_layout = new QHBoxLayout(input_row);
  input_layout->setContentsMargins(10, 8, 10, 8);
  auto* file_btn = new QPushButton(QStringLiteral("发文件"), input_row);
  file_btn->setStyleSheet(QStringLiteral(
      "QPushButton { background:#ffffff; color:#a05a26; border:1px solid "
      "#e16531; border-radius:8px; padding:6px 12px; }"
      "QPushButton:hover { background:#fdeee2; }"));
  input_box_ = new QLineEdit(input_row);
  input_box_->setPlaceholderText(QStringLiteral("输入消息，回车发送"));
  send_btn_ = new QPushButton(QStringLiteral("发送"), input_row);
  send_btn_->setStyleSheet(QStringLiteral(
      "QPushButton { background:#e16531; color:#ffffff; border:none; "
      "border-radius:8px; padding:6px 18px; font-weight:600; }"
      "QPushButton:hover { background:#c95524; }"
      "QPushButton:disabled { background:#d9cfc4; }"));
  input_layout->addWidget(file_btn);
  input_layout->addWidget(input_box_, 1);
  input_layout->addWidget(send_btn_);

  chat_layout->addWidget(head);
  chat_layout->addWidget(banner);
  chat_layout->addWidget(chat_view_, 1);
  chat_layout->addWidget(input_row);

  auto* central = new QWidget(this);
  auto* layout = new QHBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  side->setFixedWidth(260);
  side->setStyleSheet(
      QStringLiteral("background:#f5f0e8; border-right:1px solid #e8e0d6;"));
  layout->addWidget(side);
  layout->addWidget(chat, 1);
  setCentralWidget(central);

  connect(search_box_, &QLineEdit::textChanged, this, [this](const QString& t) {
    const QString needle = t.trimmed();
    for (int i = 0; i < device_list_->count(); ++i) {
      auto* item = device_list_->item(i);
      item->setHidden(!needle.isEmpty() &&
                      !item->text().contains(needle, Qt::CaseInsensitive));
    }
  });
  connect(device_list_, &QListWidget::itemSelectionChanged, this, [this] {
    auto* item = device_list_->currentItem();
    if (!item) return;
    open_peer(item->data(Qt::UserRole).toString());
  });
  connect(send_btn_, &QPushButton::clicked, this, [this] {
    const QString text = input_box_->text().trimmed();
    if (text.isEmpty() || current_peer_.isEmpty()) return;
    const std::string peer = current_peer_.toStdString();
    if (direct_engine_.send_text(peer, text.toStdString()) == 0) {
      show_status(QStringLiteral("发送失败：对端不可达"));
      return;
    }
    append_message(QString::fromStdString(direct_engine_.device_id()), text,
                   QDateTime::currentMSecsSinceEpoch(), true);
    input_box_->clear();
  });
  connect(input_box_, &QLineEdit::returnPressed, this,
          [this] { send_btn_->click(); });
  connect(file_btn, &QPushButton::clicked, this, [this] {
    if (current_peer_.isEmpty()) {
      show_status(QStringLiteral("先选择设备再发送文件"));
      return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty()) return;
    const std::string tid =
        direct_engine_.send_file(current_peer_.toStdString(), path);
    if (tid.empty()) {
      show_status(QStringLiteral("文件发送失败"));
      return;
    }
    const QString name = QFileInfo(path).fileName();
    file_sent_.insert(name, 0);
    append_system_line(QStringLiteral("[文件] %1 开始发送").arg(esc(name)));
    show_status(QStringLiteral("文件发送中：%1").arg(name));
  });
}

void MainWindow::wire_engines() {
  connect(&direct_engine_, &DirectEngine::peers_changed, this, [this] {
    refresh_devices();
    show_status(status_hint_); // 在线数变化，刷新引擎态前缀
  });
  connect(&direct_engine_, &DirectEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms) {
            if (from == current_peer_) {
              append_message(from, text, ts_ms, false);
            } else {
              show_status(QStringLiteral("来自 %1 的新消息").arg(from));
            }
          });
  connect(&direct_engine_, &DirectEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            show_status(ok ? QStringLiteral("消息已送达（seq %1）").arg(seq)
                           : QStringLiteral("消息送达失败（seq %1）").arg(seq));
          });
  connect(&direct_engine_, &DirectEngine::file_progress, this,
          [this](const QString& /*id*/, quint64 done, quint64 total) {
            if (total == 0) return;
            // 状态栏节流：按 5% 步进刷新
            const int pct = static_cast<int>(done * 100 / total);
            static int last_pct = -1;
            if (pct / 5 != last_pct / 5 || pct == 100) {
              last_pct = pct;
              show_status(QStringLiteral("文件传输进度：%1%").arg(pct));
            }
          });
  connect(&direct_engine_, &DirectEngine::file_finished, this,
          [this](const QString& /*id*/, bool ok, const QString& error) {
            show_status(ok ? QStringLiteral("文件已送达")
                           : QStringLiteral("文件传输中断：%1").arg(error));
          });
  connect(&direct_engine_, &DirectEngine::file_received, this,
          [this](const QString& /*id*/, const QString& path) {
            append_system_line(
                QStringLiteral("[文件] 已接收：%1").arg(esc(path)));
            show_status(QStringLiteral("文件已接收：%1").arg(path));
          });
}

void MainWindow::refresh_devices() {
  const QString selected = current_peer_;
  device_list_->blockSignals(true);
  device_list_->clear();
  const auto peers = direct_engine_.peers();
  for (const Peer& p : peers) {
    auto* item = new QListWidgetItem(device_list_);
    const QString id = QString::fromStdString(p.device_id);
    const QString name =
        p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    item->setText(QStringLiteral("%1\n%2 · TCP %3")
                      .arg(name, p.address.toString(),
                           QString::number(p.tcp_port)));
    item->setData(Qt::UserRole, id);
  }
  device_list_->blockSignals(false);
  device_count_->setText(peers.isEmpty()
                             ? QStringLiteral("未发现设备")
                             : QStringLiteral("%1 台在线").arg(peers.size()));

  // 当前会话设备离线 → 保留视图但提示；列表选中态恢复
  if (!selected.isEmpty()) {
    for (int i = 0; i < device_list_->count(); ++i) {
      if (device_list_->item(i)->data(Qt::UserRole).toString() == selected) {
        device_list_->setCurrentRow(i, QItemSelectionModel::NoUpdate);
        break;
      }
    }
  }

  // 未选中会话时的空态：无设备显示发现引导；有设备提示先选择
  if (selected.isEmpty()) {
    if (peers.isEmpty()) {
      show_guidance();
    } else if (chat_showing_guidance_) {
      chat_showing_guidance_ = false;
      chat_view_->clear();
      append_system_line(QStringLiteral("从左侧选择设备，开始点对点会话"));
    }
  }
}

void MainWindow::open_peer(const QString& device_id) {
  current_peer_ = device_id;
  chat_showing_guidance_ = false;
  chat_view_->clear();
  const Peer p = direct_engine_.peer(device_id.toStdString());
  const QString name =
      p.name.empty() ? device_id.left(8) : QString::fromStdString(p.name);
  chat_title_->setText(name);
  chat_meta_->setText(
      QStringLiteral("%1 · 点对点直连 · TCP %2")
          .arg(p.address.toString(), QString::number(p.tcp_port)));

  const auto hist = direct_engine_.history(device_id);
  if (hist.isEmpty()) {
    append_system_line(
        QStringLiteral("已与 %1 建立点对点会话 · 本对话不归档").arg(esc(name)));
  }
  for (const StoredMessage& m : hist) {
    append_message(QString::fromStdString(m.from),
                   QString::fromStdString(m.text), m.ts_ms,
                   m.from == direct_engine_.device_id());
  }
}

void MainWindow::append_message(const QString& from_id, const QString& text,
                                qint64 ts_ms, bool outgoing) {
  QString name = from_id;
  if (!outgoing) {
    const Peer p = direct_engine_.peer(from_id.toStdString());
    if (!p.name.empty()) name = QString::fromStdString(p.name);
  } else {
    name = QStringLiteral("我");
  }
  chat_view_->append(bubble_html(name, text, ts_ms, outgoing));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::append_system_line(const QString& text) {
  chat_view_->append(QStringLiteral(
      "<div align=\"center\"><span style=\"color:#9b8f86; "
      "font-size:small;\">%1</span></div>")
                         .arg(text));
  auto* bar = chat_view_->verticalScrollBar();
  bar->setValue(bar->maximum());
}

void MainWindow::show_guidance() {
  chat_showing_guidance_ = true;
  chat_view_->clear();
  chat_view_->append(QStringLiteral(
      "<div align=\"center\" style=\"margin-top:48px;\">"
      "<span style=\"color:#6b6157;\">尚未发现同网段设备。</span><br><br>"
      "<span style=\"color:#9b8f86; font-size:small;\">"
      "请确认对方已安装 Memex 且与本机同一局域网；<br>"
      "直连发现使用 UDP 2425、点对点传输使用 TCP 2426–2437，"
      "请检查终端防火墙放行。<br><br>"
      "如需组织架构、云端历史与归档检索，请登录协作态（左上角菜单，阶段 2 接入）。"
      "</span></div>"));
}

void MainWindow::show_status(const QString& text) {
  status_hint_ = text;
  statusBar()->showMessage(
      QString::fromStdString(direct_engine_.status_text()) +
          QStringLiteral("　|　") +
          QString::fromStdString(collab_engine_.status_text()) +
          QStringLiteral("　|　") + status_hint_,
      0);
}

} // namespace memex::client
