#include "main_window.hpp"

#include <QAction>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

#include <nlohmann/json.hpp>

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

  wire_engines();
  direct_engine_.start();
  // 双态共用同一份本地库：协作消息与直连消息同库，界面按 source 合并展示
  collab_engine_.attach_store(direct_engine_.store());
  wire_collab();

  refresh_devices();
  show_status(QStringLiteral("就绪"));
  update_banner();
}

void MainWindow::build_ui() {
  // —— 菜单：协作态登录／登出（不重启切换形态，T2.4）——
  auto* collab_menu = menuBar()->addMenu(QStringLiteral("协作"));
  auto* act_login = collab_menu->addAction(QStringLiteral("登录协作态…"));
  auto* act_org = collab_menu->addAction(QStringLiteral("组织架构…"));
  act_collab_logout_ = collab_menu->addAction(QStringLiteral("登出（回到直连态）"));
  act_collab_logout_->setEnabled(false);
  connect(act_login, &QAction::triggered, this,
          &MainWindow::show_collab_login_dialog);
  connect(act_org, &QAction::triggered, this, &MainWindow::show_org_dialog);
  connect(act_collab_logout_, &QAction::triggered, this,
          &MainWindow::logout_collab);

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

  // 「未归档」语义标记：常驻不可关（留痕铁律的界面表达）；
  // 文案随形态切换（T2.4）：直连／降级态必须出现「消息不进归档」。
  banner_ = new QLabel(chat);
  banner_->setObjectName(QStringLiteral("mode_banner"));
  banner_->setWordWrap(true);
  banner_->setStyleSheet(QStringLiteral(
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
  chat_layout->addWidget(banner_);
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
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return; // 分组标题行不可选
    open_chat(item->data(Qt::UserRole + 1).toString(), id);
  });
  connect(send_btn_, &QPushButton::clicked, this, [this] {
    const QString text = input_box_->text().trimmed();
    if (text.isEmpty() || current_peer_.isEmpty()) return;
    if (send_in_current_chat(text)) input_box_->clear();
  });
  connect(input_box_, &QLineEdit::returnPressed, this,
          [this] { send_btn_->click(); });
  connect(file_btn, &QPushButton::clicked, this, [this] {
    if (current_peer_.isEmpty()) {
      show_status(QStringLiteral("先选择设备再发送文件"));
      return;
    }
    if (current_kind_ != QStringLiteral("collab") &&
        !direct_send_allowed()) { // T3.4 策略闸门（文件与文字同口径）
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
            if (from == current_peer_ &&
                current_kind_ == QStringLiteral("direct")) {
              append_message(from, text, ts_ms, false,
                             QStringLiteral("direct"));
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

// 协作信号接入界面（T2.4）：形态切换提示、降级提示、协作消息渲染。
// 降级口径：服务端不可达＝回落直连态，必须明示「消息不进归档」（A11）。
void MainWindow::wire_collab() {
  connect(&collab_engine_, &CollabEngine::logged_in, this,
          [this](const QString& account, const QString& display) {
            (void)account;
            act_collab_logout_->setEnabled(true);
            collab_was_logged_in_ = true;
            seed_collab_peers();
            update_banner();
            refresh_devices();
            append_system_line(
                QStringLiteral("已切换协作态（%1）：此后消息经服务端转发并"
                               "全量归档")
                    .arg(esc(display)));
            show_status(QStringLiteral("协作态已登录：%1").arg(display));
          });
  connect(&collab_engine_, &CollabEngine::login_failed, this,
          [this](const QString& reason) {
            act_collab_logout_->setEnabled(false);
            update_banner();
            if (reason == QStringLiteral("无法连接服务器")) {
              // 降级：服务端不可达 → 停留直连态，明示归档缺口
              append_system_line(QStringLiteral(
                  "服务端不可达：已回落直连态，消息不进归档"
                  "（点对点传输，仍可正常收发）"));
              show_status(
                  QStringLiteral("服务端不可达，已回落直连态：消息不进归档"));
            } else {
              show_status(QStringLiteral("协作登录失败：%1").arg(reason));
            }
          });
  connect(&collab_engine_, &CollabEngine::connection_lost, this, [this] {
    collab_was_logged_in_ = false;
    update_banner();
    show_status(QStringLiteral(
        "服务端连接断开，直连态仍可用：消息不进归档（自动重连中）"));
  });
  connect(&collab_engine_, &CollabEngine::reconnected, this, [this] {
    collab_was_logged_in_ = true;
    update_banner();
    refresh_devices();
    show_status(QStringLiteral("协作态已恢复：消息重新进入归档"));
  });
  connect(&collab_engine_, &CollabEngine::kicked, this,
          [this](const QString& reason, const QString& replaced_by) {
            collab_was_logged_in_ = false;
            act_collab_logout_->setEnabled(false);
            update_banner();
            refresh_devices();
            show_status(QStringLiteral("已被顶替下线（%1）：%2")
                            .arg(replaced_by, reason));
          });
  connect(&collab_engine_, &CollabEngine::message_received, this,
          [this](const QString& from, const QString& text, qint64 ts_ms,
                 const QString& /*msg_id*/) {
            if (current_kind_ == QStringLiteral("collab") &&
                from == current_peer_) {
              // 本地库已由引擎落库（同库），这里只做界面渲染
              append_message(from, text, ts_ms, false,
                             QStringLiteral("collab"));
            } else {
              show_status(QStringLiteral("来自 %1 的协作消息").arg(from));
            }
            collab_peers_.insert(from);
            refresh_devices();
          });
  connect(&collab_engine_, &CollabEngine::text_delivered, this,
          [this](quint64 seq, bool ok) {
            show_status(ok ? QStringLiteral("协作消息已送达（seq %1）").arg(seq)
                           : QStringLiteral("协作消息送达超时（seq %1）").arg(seq));
          });
  connect(&collab_engine_, &CollabEngine::org_received, this,
          [this](const QString& org_json) {
            last_org_json_ = org_json;
            apply_policy(org_json);
            if (org_dialog_pending_) {
              org_dialog_pending_ = false;
              build_org_tree(org_json);
            } else {
              show_status(QStringLiteral("组织架构已更新"));
            }
          });
}

// —— T2.4 模式切换公共入口 ——

void MainWindow::login_collab(const QString& host, quint16 port,
                              const QString& account,
                              const QString& password) {
  collab_engine_.login(host, port, account, password);
}

void MainWindow::logout_collab() {
  collab_engine_.logout();
  act_collab_logout_->setEnabled(false);
  collab_was_logged_in_ = false;
  update_banner();
  append_system_line(QStringLiteral(
      "已登出协作态，回到直连态：消息不进归档（本地历史保留，合并展示）"));
  show_status(QStringLiteral("协作态已登出：当前直连态，消息不进归档"));
  refresh_devices();
}

void MainWindow::open_collab_peer(const QString& account) {
  if (account.isEmpty()) return;
  collab_peers_.insert(account);
  refresh_devices();
  open_chat(QStringLiteral("collab"), account);
}

bool MainWindow::send_in_current_chat(const QString& text) {
  if (text.isEmpty() || current_peer_.isEmpty()) return false;
  if (current_kind_ == QStringLiteral("collab")) {
    const quint64 seq = collab_engine_.send_text(current_peer_, text);
    if (seq == 0) {
      show_status(QStringLiteral(
          "发送失败：协作态未登录（可先登录协作态；当前消息不进归档）"));
      return false;
    }
    append_message(collab_engine_.account(), text,
                   QDateTime::currentMSecsSinceEpoch(), true,
                   QStringLiteral("collab"));
    return true;
  }
  if (!direct_send_allowed()) return false; // T3.4 策略闸门
  const std::string peer = current_peer_.toStdString();
  if (direct_engine_.send_text(peer, text.toStdString()) == 0) {
    show_status(QStringLiteral("发送失败：对端不可达"));
    return false;
  }
  append_message(QString::fromStdString(direct_engine_.device_id()), text,
                 QDateTime::currentMSecsSinceEpoch(), true,
                 QStringLiteral("direct"));
  return true;
}

// —— T3.4 策略开关（下发自服务端，按本人部门解析）——
// 直连发送闸门：未登录发直连＝免登录使用；已登录发直连＝与未登录设备通信。
bool MainWindow::direct_send_allowed() {
  if (!collab_engine_.is_logged_in() && !anonymous_allowed_) {
    show_status(QStringLiteral(
        "当前策略：需登录协作态后使用（免登录使用已被管理员禁止）"));
    return false;
  }
  if (collab_engine_.is_logged_in() && !cross_state_allowed_) {
    show_status(QStringLiteral(
        "当前策略：禁止与未登录设备通信（该会话未进归档）"));
    return false;
  }
  return true;
}

// 解析本人生效策略：本人部门 → 逐级上级部门 → 全局行 → 默认宽松
void MainWindow::apply_policy(const QString& org_json) {
  nlohmann::json j =
      nlohmann::json::parse(org_json.toStdString(), nullptr, false);
  if (j.is_discarded()) return;
  // 本人部门路径
  QString dept;
  const QString me = collab_engine_.account();
  if (j.contains("members")) {
    for (const auto& m : j["members"]) {
      if (QString::fromStdString(m.value("account", std::string{})) == me) {
        dept = QString::fromStdString(m.value("department_path", std::string{}));
        break;
      }
    }
  }
  const auto match = [&](const QString& path) -> const nlohmann::json* {
    if (!j.contains("policies")) return nullptr;
    for (const auto& p : j["policies"]) {
      if (QString::fromStdString(p.value("department_path", std::string{})) ==
          path) {
        return &p;
      }
    }
    return nullptr;
  };
  const nlohmann::json* row = nullptr;
  QString path = dept;
  while (!path.isEmpty()) {
    if (const nlohmann::json* hit = match(path)) {
      row = hit;
      break;
    }
    const int pos = path.lastIndexOf(QChar('/'));
    if (pos <= 0) break;
    path.truncate(pos);
  }
  if (!row) row = match(QString()); // 全局行
  if (row) {
    anonymous_allowed_ = row->value("allow_anonymous", true);
    cross_state_allowed_ = row->value("allow_cross_state", true);
  } else {
    anonymous_allowed_ = true; // 未配置＝默认宽松
    cross_state_allowed_ = true;
  }
}

bool MainWindow::collab_logged_in() const {
  return collab_engine_.is_logged_in();
}

// —— T3.1 组织架构：登录后向服务端查询，管理端维护即生效到客户端 ——

void MainWindow::request_org() {
  if (collab_engine_.is_logged_in()) collab_engine_.query_org();
}

QString MainWindow::org_json() const { return last_org_json_; }

void MainWindow::show_org_dialog() {
  if (!collab_engine_.is_logged_in()) {
    show_status(QStringLiteral("组织架构需登录协作态后查看"));
    return;
  }
  org_dialog_pending_ = true;
  collab_engine_.query_org();
}

// 组织架构弹窗：部门树（全路径逐级成树）＋成员挂部门（含直属上级、职务、角色）
void MainWindow::build_org_tree(const QString& org_json) {
  nlohmann::json j = nlohmann::json::parse(org_json.toStdString(),
                                           nullptr, false);
  if (j.is_discarded()) {
    show_status(QStringLiteral("组织架构数据解析失败"));
    return;
  }
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("组织架构（服务端下发）"));
  dlg.resize(560, 620);
  auto* layout = new QVBoxLayout(&dlg);
  auto* tree = new QTreeWidget(&dlg);
  tree->setHeaderLabels({QStringLiteral("部门 / 成员"), QStringLiteral("职务"),
                         QStringLiteral("直属上级")});
  tree->setColumnWidth(0, 280);
  layout->addWidget(tree);

  // 部门：全路径逐级挂树（"公司/研发部/客户端组"）
  std::map<QString, QTreeWidgetItem*> nodes;
  auto* unassigned = new QTreeWidgetItem(tree);
  unassigned->setText(0, QStringLiteral("（未分配部门）"));
  if (j.contains("departments")) {
    for (const auto& d : j["departments"]) {
      const QString path =
          QString::fromStdString(d.value("path", std::string{}));
      if (path.isEmpty() || nodes.count(path)) continue;
      const QStringList segs = path.split(QChar('/'));
      QTreeWidgetItem* parent = nullptr;
      QString prefix;
      for (const QString& seg : segs) {
        prefix = prefix.isEmpty() ? seg : prefix + QChar('/') + seg;
        auto it = nodes.find(prefix);
        if (it == nodes.end()) {
          auto* item =
              new QTreeWidgetItem(parent ? parent : tree->invisibleRootItem());
          item->setText(0, seg);
          it = nodes.emplace(prefix, item).first;
        }
        parent = it->second;
      }
    }
  }
  // 成员挂部门节点；显示名（账号）＋管理员标记
  if (j.contains("members")) {
    for (const auto& m : j["members"]) {
      const QString dept =
          QString::fromStdString(m.value("department_path", std::string{}));
      const QString account =
          QString::fromStdString(m.value("account", std::string{}));
      const QString name =
          QString::fromStdString(m.value("display_name", std::string{}));
      const QString title =
          QString::fromStdString(m.value("title", std::string{}));
      const QString manager =
          QString::fromStdString(m.value("manager", std::string{}));
      const QString role =
          QString::fromStdString(m.value("role", std::string{}));
      auto it = nodes.find(dept);
      QTreeWidgetItem* parent =
          it != nodes.end() ? it->second : unassigned;
      auto* item = new QTreeWidgetItem(parent);
      item->setText(0, QStringLiteral("%1（%2）%3")
                            .arg(name, account,
                                 role == QStringLiteral("admin")
                                     ? QStringLiteral("·管理员")
                                     : QString{}));
      item->setText(1, title);
      item->setText(2, manager);
    }
  }
  tree->expandAll();
  auto* close = new QPushButton(QStringLiteral("关闭"), &dlg);
  connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
  layout->addWidget(close);
  dlg.exec();
}

QString MainWindow::banner_text() const { return banner_->text(); }

QString MainWindow::status_text() const { return statusBar()->currentMessage(); }

QString MainWindow::chat_html() const { return chat_view_->toHtml(); }

// 归档提示条（常驻不可关）：协作态显示归档口径；直连／降级态必须明示
// 「消息不进归档」——切换与降级共用这一条提示（A7、A11 界面口径）。
void MainWindow::update_banner() {
  const bool collab_session =
      current_kind_ == QStringLiteral("collab") &&
      collab_engine_.is_logged_in();
  if (collab_session) {
    banner_->setStyleSheet(QStringLiteral(
        "background:#eaf3e7; color:#3f6b3a; font-size:12px; padding:6px 10px;"));
    banner_->setText(QStringLiteral(
        "　✔ 协作态会话：消息经服务端转发并全量归档；撤回仅改显示，"
        "服务端保留原文与撤回记录"));
  } else {
    banner_->setStyleSheet(QStringLiteral(
        "background:#fdeee2; color:#8a4a1f; font-size:12px; padding:6px 10px;"));
    if (collab_was_logged_in_ && !collab_engine_.is_logged_in()) {
      banner_->setText(QStringLiteral(
          "　⚠ 已降级直连态：服务端不可达，消息不进归档"
          "（点对点传输，仅保存在双方本机）"));
    } else {
      banner_->setText(QStringLiteral(
          "　⚠ 直连态会话：消息点对点传输，不经过服务器——消息不进归档"
          "（服务端无任何记录，仅保存在双方本机）"));
    }
  }
}

// 登录后把本地库里的历史协作会话补进列表（换机／重启后会话入口不丢）
void MainWindow::seed_collab_peers() {
  if (LocalStore* store = direct_engine_.store()) {
    const QStringList hist = store->peers(QStringLiteral("collab"));
    for (const QString& p : hist) collab_peers_.insert(p);
  }
}

void MainWindow::show_collab_login_dialog() {
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("登录协作态"));
  auto* form = new QFormLayout(&dlg);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  auto* host = new QLineEdit(
      settings.value(QStringLiteral("host"), QStringLiteral("127.0.0.1"))
          .toString(),
      &dlg);
  auto* port = new QLineEdit(
      settings.value(QStringLiteral("port"), QStringLiteral("24360")).toString(),
      &dlg);
  auto* account = new QLineEdit(
      settings.value(QStringLiteral("account")).toString(), &dlg);
  auto* password = new QLineEdit(&dlg);
  password->setEchoMode(QLineEdit::Password);
  form->addRow(QStringLiteral("服务器地址"), host);
  form->addRow(QStringLiteral("端口"), port);
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
  settings.setValue(QStringLiteral("account"), account->text().trimmed());
  login_collab(host->text().trimmed(),
               static_cast<quint16>(port->text().toUInt()),
               account->text().trimmed(), password->text());
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
    item->setData(Qt::UserRole + 1, QStringLiteral("direct"));
  }

  // 协作会话分组（登录后出现；本地历史 + 本会话窗口期的对端）
  if (collab_engine_.is_logged_in() && !collab_peers_.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("协作会话 · 服务端归档"));
    header->setFlags(Qt::NoItemFlags); // 分组标题：不可选（空 id 不进会话）
    device_list_->addItem(header);
    QStringList sorted(collab_peers_.begin(), collab_peers_.end());
    sorted.sort();
    for (const QString& account : sorted) {
      auto* item = new QListWidgetItem(device_list_);
      item->setText(QStringLiteral("%1\n协作态 · 已归档").arg(account));
      item->setData(Qt::UserRole, account);
      item->setData(Qt::UserRole + 1, QStringLiteral("collab"));
    }
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
  open_chat(QStringLiteral("direct"), device_id);
}

// 打开会话（kind=direct 设备 / collab 账号）：历史取自共享本地库——
// 直连与协作消息同库合并，按 source 字段标注来源（T2.4 合并展示）。
void MainWindow::open_chat(const QString& kind, const QString& id) {
  current_kind_ = kind;
  current_peer_ = id;
  chat_showing_guidance_ = false;
  chat_view_->clear();

  const bool collab = kind == QStringLiteral("collab");
  const QString my_id = collab ? collab_engine_.account()
                               : QString::fromStdString(direct_engine_.device_id());
  if (collab) {
    chat_title_->setText(id);
    chat_meta_->setText(QStringLiteral("%1 · 协作态 · 消息进入服务端归档")
                            .arg(id));
  } else {
    const Peer p = direct_engine_.peer(id.toStdString());
    const QString name = p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    chat_title_->setText(name);
    chat_meta_->setText(
        QStringLiteral("%1 · 点对点直连 · TCP %2")
            .arg(p.address.toString(), QString::number(p.tcp_port)));
  }
  update_banner();

  const auto hist = direct_engine_.history(id);
  if (hist.isEmpty()) {
    if (collab) {
      append_system_line(
          QStringLiteral("已与 %1 建立协作会话 · 消息经服务端转发并全量归档")
              .arg(esc(id)));
    } else {
      append_system_line(
          QStringLiteral("已与 %1 建立点对点会话 · 本对话不归档")
              .arg(esc(chat_title_->text())));
    }
  }
  for (const StoredMessage& m : hist) {
    append_message(QString::fromStdString(m.from),
                   QString::fromStdString(m.text), m.ts_ms,
                   m.from == my_id.toStdString(),
                   QString::fromStdString(m.source));
  }
}

void MainWindow::append_message(const QString& from_id, const QString& text,
                                qint64 ts_ms, bool outgoing,
                                const QString& source) {
  QString name = from_id;
  if (!outgoing) {
    if (current_kind_ == QStringLiteral("collab")) {
      name = from_id; // 协作会话对端即账号
    } else {
      const Peer p = direct_engine_.peer(from_id.toStdString());
      if (!p.name.empty()) name = QString::fromStdString(p.name);
    }
  } else {
    name = QStringLiteral("我");
  }
  // 来源字段（合并展示）：直连＝仅本机；协作＝服务端归档
  const QString tag =
      source == QStringLiteral("collab")
          ? QStringLiteral(" · 协作·已归档")
          : QStringLiteral(" · 直连·仅本机");
  chat_view_->append(bubble_html(name + tag, text, ts_ms, outgoing));
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
      "如需组织架构、云端历史与归档检索，请经左上角「协作」菜单登录协作态。"
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
