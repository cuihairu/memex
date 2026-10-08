// 二期·远程协助窗口（实现）。红线条条在服务端：consent（批/拒/撤只属
// 受控方）、audit（迁移自动留痕）、部门放行开关默认禁、媒体权限位门。
// 客户端只做门面＋媒体搬运：受控方 500ms 抓屏推帧（JPEG），发起方拉帧
// 渲染＋点画面发鼠标事件；受控方拉输入事件（鼠标移动应用 QCursor 这
// 一可移植原语，深度注入属平台层留后续）。
#include "assist_dialog.hpp"
#include <app/net_guard.hpp>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include <QGuiApplication>

#include "engine/collab/files_client.hpp"

namespace memex::client {
namespace {

QString status_cn(const QString& s) {
  if (s == QStringLiteral("requested")) return QStringLiteral("待批");
  if (s == QStringLiteral("approved")) return QStringLiteral("已批待启动");
  if (s == QStringLiteral("active")) return QStringLiteral("进行中");
  if (s == QStringLiteral("closed")) return QStringLiteral("已结束");
  if (s == QStringLiteral("denied")) return QStringLiteral("已拒绝");
  return s;
}

QString mask_names(int mask) {
  QStringList names;
  if (mask & 1) names << QStringLiteral("查看");
  if (mask & 4) names << QStringLiteral("鼠标");
  if (mask & 2) names << QStringLiteral("键盘");
  if (mask & 8) names << QStringLiteral("剪贴板");
  if (mask & 16) names << QStringLiteral("文件");
  return names.isEmpty() ? QStringLiteral("—") : names.join(QStringLiteral("/"));
}

QString short_id(const QString& id) { return id.right(6); }

} // namespace

AssistDialog::AssistDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("远程协助"));
  resize(760, 560);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    account_ = client_->account();
    set_status(QStringLiteral("已连接（") + account_ + QStringLiteral("）"));
    timer_->start(500); // 轮询台账＋媒体搬运=过程持续可见
    client_->fetch_assist_sessions();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::assist_requested, this,
          [this](const QString& id) {
            set_status(QStringLiteral("已发起（") + short_id(id) +
                       QStringLiteral("），等受控方批准"));
            client_->fetch_assist_sessions();
          });
  connect(client_, &FilesClient::assist_responded, this,
          [this](const QString&) {
            set_status(QStringLiteral("已答复协助请求"));
            client_->fetch_assist_sessions();
          });
  connect(client_, &FilesClient::assist_started, this,
          [this](const QString&) {
            set_status(QStringLiteral("会话已启动（进行中）"));
            client_->fetch_assist_sessions();
          });
  connect(client_, &FilesClient::assist_ended, this,
          [this](const QString&) {
            set_status(QStringLiteral("会话已结束（终态留痕）"));
            sharing_ = false;
            btn_share_->setText(QStringLiteral("开始共享屏幕"));
            client_->fetch_assist_sessions();
          });
  connect(client_, &FilesClient::assist_sessions_listed, this,
          [this](const QJsonArray& rows) {
            // 会话台账（全部相关）＋待批行＋共享/查看态推导
            sessions_->blockSignals(true);
            sessions_->clear();
            pending_->clear();
            for (const auto& v : rows) {
              const auto s = v.toObject();
              const QString id = s.value(QStringLiteral("id")).toString();
              const QString requester =
                  s.value(QStringLiteral("requester")).toString();
              const QString target =
                  s.value(QStringLiteral("target")).toString();
              const QString st =
                  s.value(QStringLiteral("status")).toString();
              const bool mine = requester == account_;
              // 角色与状态挂数据角色（不靠行文本反推）
              auto* it = new QListWidgetItem(
                  QStringLiteral("%1 %2→%3 [%4] 权 %5")
                      .arg(short_id(id),
                           mine ? QStringLiteral("我") : requester,
                           mine ? target : QStringLiteral("我"),
                           status_cn(st),
                           mask_names(static_cast<int>(
                               s.value(QStringLiteral("granted_mask"))
                                   .toDouble()))),
                  sessions_);
              it->setData(Qt::UserRole, id);
              it->setData(Qt::UserRole + 1, mine ? QStringLiteral("req")
                                                 : QStringLiteral("tgt"));
              it->setData(Qt::UserRole + 2, st);
              if (st == QStringLiteral("active") && mine) {
                it->setBackground(QColor(0xE8, 0xF4, 0xE8)); // 进行中淡绿
              }
              if (st == QStringLiteral("requested") &&
                  target == account_) {
                auto* pit = new QListWidgetItem(
                    QStringLiteral("%1 请求协助（申请：%2）")
                        .arg(requester,
                             mask_names(static_cast<int>(
                                 s.value(QStringLiteral("requested_mask"))
                                     .toDouble()))),
                    pending_);
                pit->setData(Qt::UserRole, id);
              }
            }
            sessions_->blockSignals(false);
            // 共享中指示（受控侧持续可见）＋共享按钮面
            const QString share_id = active_id_as_target();
            if (!share_id.isEmpty() && sharing_) {
              share_label_->setText(
                  QStringLiteral("● 屏幕共享中——对方正在查看你的屏幕"
                                 "（随时结束=撤权）"));
            } else if (!share_id.isEmpty()) {
              share_label_->setText(
                  QStringLiteral("有进行中的协助会话（未在共享）"));
            } else {
              share_label_->setText(QString());
            }
          });
  connect(client_, &FilesClient::assist_audit_listed, this,
          [this](const QJsonArray& rows) {
            audit_->clear();
            for (const auto& v : rows) {
              const auto a = v.toObject();
              audit_->addItem(
                  QStringLiteral("%1  %2  %3  %4")
                      .arg(QDateTime::fromMSecsSinceEpoch(
                               static_cast<qint64>(
                                   a.value(QStringLiteral("ts_ms"))
                                       .toDouble()))
                               .toString(QStringLiteral("MM-dd hh:mm:ss")),
                           a.value(QStringLiteral("actor")).toString(),
                           a.value(QStringLiteral("action")).toString(),
                           a.value(QStringLiteral("detail")).toString()));
            }
            set_status(QStringLiteral("审计链 %1 笔").arg(rows.size()));
          });
  connect(client_, &FilesClient::assist_frame_pushed, this,
          [this](qint64 seq) {
            set_status(QStringLiteral("帧已推（seq=%1）").arg(seq));
          });
  connect(client_, &FilesClient::assist_frame_pulled, this,
          [this](qint64 seq, const QString& jpeg_b64) {
            if (seq <= 0 || jpeg_b64.isEmpty()) return;
            QPixmap pm;
            pm.loadFromData(QByteArray::fromBase64(jpeg_b64.toUtf8()),
                            "JPEG");
            if (!pm.isNull()) {
              frame_->setPixmap(pm.scaled(
                  frame_->size(), Qt::KeepAspectRatio,
                  Qt::SmoothTransformation));
              frame_seq_ = seq;
              set_status(QStringLiteral("远端画面 seq=%1").arg(seq));
            }
          });
  connect(client_, &FilesClient::assist_inputs_listed, this,
          [this](const QJsonArray& events) {
            for (const auto& v : events) {
              const auto e = v.toObject();
              const QString kind =
                  e.value(QStringLiteral("kind")).toString();
              // 可移植原语只应用鼠标移动（归一化坐标→本机屏幕）；
              // 点击/键盘如实记录（深度注入属平台层留后续）
              if (kind == QStringLiteral("mouse_move")) {
                const auto* scr = QGuiApplication::primaryScreen();
                if (scr != nullptr) {
                  QCursor::setPos(
                      static_cast<int>(e.value(QStringLiteral("x"))
                                           .toDouble() *
                                       scr->geometry().width()),
                      static_cast<int>(e.value(QStringLiteral("y"))
                                           .toDouble() *
                                       scr->geometry().height()));
                }
              }
              inputs_->addItem(QStringLiteral("%1 (%2,%3) %4")
                                   .arg(kind,
                                        QString::number(
                                            e.value(QStringLiteral("x"))
                                                .toDouble(),
                                            'f', 2),
                                        QString::number(
                                            e.value(QStringLiteral("y"))
                                                .toDouble(),
                                            'f', 2),
                                        e.value(QStringLiteral("key"))
                                            .toString()));
            }
          });
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            set_status(QStringLiteral("操作失败（%1：%2 %3）")
                           .arg(op, QString::number(status), error),
                       true);
          });
}

void AssistDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // 连接区（与文件助手同构）
  auto* conn = new QHBoxLayout;
  auto* host = new QLineEdit(this);
  host->setPlaceholderText(QStringLiteral("服务器地址"));
  auto* port = new QLineEdit(this);
  port->setPlaceholderText(QStringLiteral("文件端口"));
  port->setMaximumWidth(90);
  auto* account_box = new QLineEdit(this);
  account_box->setPlaceholderText(QStringLiteral("账号"));
  account_box->setMaximumWidth(110);
  auto* password = new QLineEdit(this);
  password->setPlaceholderText(QStringLiteral("口令"));
  password->setEchoMode(QLineEdit::Password);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn->addWidget(host);
  conn->addWidget(port);
  conn->addWidget(account_box);
  conn->addWidget(password);
  conn->addWidget(btn_connect_);
  layout->addLayout(conn);
  connect(btn_connect_, &QPushButton::clicked, this,
          [this, host, port, account_box, password] {
            connect_to(host->text(), port->text().toUShort(),
                       account_box->text(), password->text());
          });

  auto* cols = new QHBoxLayout;

  // —— 左列：我发起协助 ——
  auto* left = new QVBoxLayout;
  auto* rrow = new QHBoxLayout;
  target_edit_ = new QLineEdit(this);
  target_edit_->setPlaceholderText(QStringLiteral("受控方账号"));
  chk_view_ = new QCheckBox(QStringLiteral("查看"), this);
  chk_view_->setChecked(true);
  chk_mouse_ = new QCheckBox(QStringLiteral("鼠标"), this);
  chk_kb_ = new QCheckBox(QStringLiteral("键盘"), this);
  btn_request_ = new QPushButton(QStringLiteral("发起协助"), this);
  rrow->addWidget(target_edit_, 1);
  rrow->addWidget(chk_view_);
  rrow->addWidget(chk_mouse_);
  rrow->addWidget(chk_kb_);
  rrow->addWidget(btn_request_);
  left->addLayout(rrow);
  left->addWidget(new QLabel(QStringLiteral("会话台账（过程持续可见）"), this));
  sessions_ = new QListWidget(this);
  left->addWidget(sessions_, 2);
  auto* orow = new QHBoxLayout;
  btn_start_ = new QPushButton(QStringLiteral("启动选中"), this);
  btn_end_ = new QPushButton(QStringLiteral("结束选中"), this);
  btn_audit_ = new QPushButton(QStringLiteral("看审计链"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  orow->addWidget(btn_start_);
  orow->addWidget(btn_end_);
  orow->addWidget(btn_audit_);
  orow->addWidget(btn_refresh_);
  left->addLayout(orow);
  frame_ = new QLabel(QStringLiteral("（远端画面——选中进行中的会话自动显示）"),
                      this);
  frame_->setMinimumSize(320, 200);
  frame_->setAlignment(Qt::AlignCenter);
  frame_->setStyleSheet(
      QStringLiteral("background:#222;color:#bbb;font-size:12px;"));
  frame_->installEventFilter(this); // 点画面→发鼠标事件
  left->addWidget(frame_, 1);
  left->addWidget(new QLabel(QStringLiteral("审计链（状态迁移留痕）"), this));
  audit_ = new QListWidget(this);
  audit_->setMaximumHeight(110);
  left->addWidget(audit_);
  cols->addLayout(left, 1);

  // —— 右列：别人协助我（受控方）——
  auto* right = new QVBoxLayout;
  right->addWidget(new QLabel(QStringLiteral("待我批（consent 只属受控方）"),
                              this));
  pending_ = new QListWidget(this);
  right->addWidget(pending_, 1);
  auto* prow = new QHBoxLayout;
  btn_approve_ = new QPushButton(QStringLiteral("批准勾选项"), this);
  btn_deny_ = new QPushButton(QStringLiteral("拒绝"), this);
  prow->addWidget(btn_approve_);
  prow->addWidget(btn_deny_);
  right->addLayout(prow);
  auto* grows = new QHBoxLayout;
  grows->addWidget(new QLabel(QStringLiteral("实批："), this));
  gview_ = new QCheckBox(QStringLiteral("查看"), this);
  gview_->setChecked(true);
  gmouse_ = new QCheckBox(QStringLiteral("鼠标"), this);
  gkb_ = new QCheckBox(QStringLiteral("键盘"), this);
  grows->addWidget(gview_);
  grows->addWidget(gmouse_);
  grows->addWidget(gkb_);
  right->addLayout(grows);
  share_label_ = new QLabel(this);
  share_label_->setStyleSheet(
      QStringLiteral("color:#c0392b;font-weight:bold;"));
  right->addWidget(share_label_);
  btn_share_ = new QPushButton(QStringLiteral("开始共享屏幕"), this);
  right->addWidget(btn_share_);
  right->addWidget(new QLabel(QStringLiteral("对方操作（取走即清）"), this));
  inputs_ = new QListWidget(this);
  right->addWidget(inputs_, 1);
  cols->addLayout(right, 1);
  layout->addLayout(cols);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_request_, &QPushButton::clicked, this,
          [this] { request_assist(target_edit_->text().trimmed()); });
  connect(btn_start_, &QPushButton::clicked, this,
          [this] { start_selected(); });
  connect(btn_end_, &QPushButton::clicked, this, [this] {
    end_selected();
  });
  connect(btn_audit_, &QPushButton::clicked, this,
          [this] { show_audit(); });
  connect(btn_refresh_, &QPushButton::clicked, this,
          [this] { client_->fetch_assist_sessions(); });
  connect(btn_approve_, &QPushButton::clicked, this, [this] {
    // 远程控制配对密码门（用户令 2026-10-08 ⑤）：设置里开了远程＝批准
    // 前须输对配对密码；拒绝与取消不走此门（密码关着时直批同旧路径）。
    if (remote_control::enabled()) {
      bool ok = false;
      const QString pwd = QInputDialog::getText(
          this, QStringLiteral("远程控制配对密码"),
          QStringLiteral("批准前请输入配对密码"), QLineEdit::Password,
          QString(), &ok);
      if (!ok) return; // 取消＝不动（请求保持待批）
      const QString err = try_approve(pwd, grant_checks());
      if (!err.isEmpty()) set_status(err, true);
      return;
    }
    approve_pending(true, grant_checks());
  });
  connect(btn_deny_, &QPushButton::clicked, this,
          [this] { approve_pending(false, {}); });
  connect(btn_share_, &QPushButton::clicked, this, [this] {
    if (sharing_) {
      stop_sharing();
    } else {
      start_sharing();
    }
  });
  connect(sessions_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) {
    set_status(QStringLiteral("选中 %1").arg(short_id(selected_session())));
  });

  // 轮询：台账保鲜＋受控侧推帧＋发起侧拉帧＋受控侧拉输入
  timer_ = new QTimer(this);
  connect(timer_, &QTimer::timeout, this, [this] {
    if (!is_connected()) return;
    client_->fetch_assist_sessions();
    if (sharing_) {
      const QString share_id = active_id_as_target();
      if (!share_id.isEmpty()) {
        auto* scr = QGuiApplication::primaryScreen();
        if (scr != nullptr) {
          const QPixmap pm = scr->grabWindow(0);
          if (!pm.isNull()) {
            QByteArray bytes;
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            pm.toImage()
                .scaled(1280, 720, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation)
                .save(&buf, "JPEG", 50);
            client_->push_assist_frame(share_id, frame_seq_ + 1,
                                       QString::fromUtf8(bytes.toBase64()));
          }
        }
      }
    }
    const QString view_id = active_id_as_requester();
    if (!view_id.isEmpty()) client_->pull_assist_frame(view_id);
    const QString in_id = active_id_as_target();
    if (!in_id.isEmpty()) client_->fetch_assist_input(in_id);
  });
}

bool AssistDialog::eventFilter(QObject* watched, QEvent* ev) {
  if (watched == frame_ && ev->type() == QEvent::MouseButtonPress) {
    const auto* me = static_cast<QMouseEvent*>(ev);
    const QString view_id = active_id_as_requester();
    if (view_id.isEmpty()) {
      set_status(QStringLiteral("没有进行中的协助会话"), true);
      return true;
    }
    const QPointF rel(me->position().x() / frame_->width(),
                      me->position().y() / frame_->height());
    client_->send_assist_input(view_id, QStringLiteral("mouse_click"),
                               rel.x(), rel.y(), QString());
    set_status(QStringLiteral("已发点击（%1,%2）")
                   .arg(QString::number(rel.x(), 'f', 2),
                        QString::number(rel.y(), 'f', 2)));
    return true;
  }
  return QDialog::eventFilter(watched, ev);
}

void AssistDialog::connect_to(const QString& host, quint16 files_port,
                              const QString& acc, const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool AssistDialog::is_connected() const { return client_->is_logged_in(); }

void AssistDialog::request_assist(const QString& target,
                                  const QStringList& perms) {
  if (target.isEmpty() || !is_connected()) {
    set_status(QStringLiteral("受控方账号不能为空（或未连接）"), true);
    return;
  }
  QStringList use = perms;
  if (use.isEmpty()) {
    if (chk_view_->isChecked()) use << QStringLiteral("view");
    if (chk_mouse_->isChecked()) use << QStringLiteral("mouse");
    if (chk_kb_->isChecked()) use << QStringLiteral("keyboard");
  }
  if (use.isEmpty()) {
    set_status(QStringLiteral("至少勾选一项权限"), true);
    return;
  }
  client_->assist_request(target, use);
}

QString AssistDialog::try_approve(const QString& pwd,
                                 const QStringList& perms) {
  if (remote_control::enabled() && !remote_control::verify_password(pwd))
    return QStringLiteral("配对密码不对");
  approve_pending(true, perms);
  return QString();
}

void AssistDialog::approve_pending(bool allow, const QStringList& perms) {
  if (pending_->count() == 0) {
    set_status(QStringLiteral("没有待批请求"), true);
    return;
  }
  client_->assist_respond(first_pending_id(), allow, perms);
}

void AssistDialog::start_selected() {
  const QString id = selected_session();
  if (id.isEmpty()) {
    set_status(QStringLiteral("先选中一个会话"), true);
    return;
  }
  client_->assist_start(id);
}

void AssistDialog::end_selected(bool with_reason) {
  const QString id = selected_session();
  if (id.isEmpty()) {
    set_status(QStringLiteral("先选中一个会话"), true);
    return;
  }
  client_->assist_end(id, with_reason ? QStringLiteral("当事方结束") : QString());
}

void AssistDialog::start_sharing() {
  if (active_id_as_target().isEmpty()) {
    set_status(QStringLiteral("没有进行中的协助会话可共享"), true);
    return;
  }
  sharing_ = true;
  btn_share_->setText(QStringLiteral("结束共享（撤权）"));
  set_status(QStringLiteral("开始共享屏幕"));
}

void AssistDialog::stop_sharing() {
  const QString share_id = active_id_as_target();
  sharing_ = false;
  btn_share_->setText(QStringLiteral("开始共享屏幕"));
  if (!share_id.isEmpty()) {
    // 受控方结束=撤权即时生效（服务端终态即擦媒体槽）
    client_->assist_end(share_id, QStringLiteral("受控方结束共享"));
  }
}

void AssistDialog::push_frame_once(const QString& jpeg_b64) {
  const QString share_id = active_id_as_target();
  if (share_id.isEmpty()) {
    set_status(QStringLiteral("没有进行中的协助会话可推帧"), true);
    return;
  }
  client_->push_assist_frame(share_id, frame_seq_ + 1, jpeg_b64);
}

void AssistDialog::refresh_sessions() { client_->fetch_assist_sessions(); }

void AssistDialog::select_session(int row) {
  sessions_->setCurrentRow(row);
}

void AssistDialog::click_frame(double nx, double ny) {
  // 构造与真实按压同款鼠标事件发往画面（eventFilter 路径全真）
  QMouseEvent me(QEvent::MouseButtonPress,
                 QPointF(frame_->width() * nx, frame_->height() * ny),
                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(frame_, &me);
}

void AssistDialog::show_audit() {
  const QString id = selected_session();
  if (id.isEmpty()) {
    set_status(QStringLiteral("先选中一个会话"), true);
    return;
  }
  client_->fetch_assist_audit(id);
}

QString AssistDialog::first_pending_id() const {
  return pending_->count() > 0
             ? pending_->item(0)->data(Qt::UserRole).toString()
             : QString();
}

QString AssistDialog::active_id_as_target() const {
  for (int i = 0; i < sessions_->count(); ++i) {
    const auto* it = sessions_->item(i);
    if (it->data(Qt::UserRole + 1).toString() == QStringLiteral("tgt") &&
        it->data(Qt::UserRole + 2).toString() == QStringLiteral("active")) {
      return it->data(Qt::UserRole).toString();
    }
  }
  return QString();
}

QString AssistDialog::active_id_as_requester() const {
  for (int i = 0; i < sessions_->count(); ++i) {
    const auto* it = sessions_->item(i);
    if (it->data(Qt::UserRole + 1).toString() == QStringLiteral("req") &&
        it->data(Qt::UserRole + 2).toString() == QStringLiteral("active")) {
      return it->data(Qt::UserRole).toString();
    }
  }
  return QString();
}

QString AssistDialog::selected_session() const {
  return sessions_->currentItem() != nullptr
             ? sessions_->currentItem()->data(Qt::UserRole).toString()
             : QString();
}

void AssistDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString AssistDialog::status_text() const { return status_->text(); }

int AssistDialog::session_count() const { return sessions_->count(); }

int AssistDialog::pending_count() const { return pending_->count(); }

int AssistDialog::audit_count() const { return audit_->count(); }

QString AssistDialog::session_text(int row) const {
  return row >= 0 && row < sessions_->count()
             ? sessions_->item(row)->text()
             : QString();
}

qint64 AssistDialog::frame_seq_seen() const { return frame_seq_; }

int AssistDialog::input_log_count() const { return inputs_->count(); }

QString AssistDialog::share_text() const { return share_label_->text(); }

QStringList AssistDialog::grant_checks() const {
  QStringList perms;
  if (gview_->isChecked()) perms << QStringLiteral("view");
  if (gmouse_->isChecked()) perms << QStringLiteral("mouse");
  if (gkb_->isChecked()) perms << QStringLiteral("keyboard");
  return perms;
}

} // namespace memex::client
