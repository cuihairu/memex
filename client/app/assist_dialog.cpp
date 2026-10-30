// 二期·远程协助窗口（实现）。红线条条在服务端：consent（批/拒/撤只属
// 受控方）、audit（迁移自动留痕）、部门放行开关默认禁、媒体权限位门。
// 客户端只做门面＋媒体搬运：受控方 500ms 抓屏推帧（JPEG），发起方拉帧
// 渲染＋点画面发鼠标事件；受控方拉输入事件（鼠标移动应用 QCursor 这
// 一可移植原语，深度注入属平台层留后续）。
#include "assist_dialog.hpp"
#include "collab_defaults.hpp"
#include <app/net_guard.hpp>

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
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
  resize(1080, 700);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    account_ = client_->account();
    set_status(QStringLiteral("已连接（") + account_ + QStringLiteral("）"));
    conn_badge_->setText(
        QStringLiteral("已连接（500ms 轮询 · 屏幕永不落盘）"));
    conn_badge_->setStyleSheet(
        QStringLiteral("color:%1;font-weight:600;")
            .arg(QColor(Qt::green).name()));
    timer_->start(500); // 轮询台账＋媒体搬运=过程持续可见
    client_->fetch_assist_sessions();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            conn_badge_->setText(QStringLiteral("未连接"));
            conn_badge_->setStyleSheet(QString());
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
            // 台账 500ms 重建清选中——选中提示同步清（真实选中态为准）
            sel_lbl_->setText(selected_session().isEmpty()
                                  ? QString()
                                  : QStringLiteral("选中 %1")
                                        .arg(short_id(selected_session())));
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
            audit_sub_->setText(QStringLiteral("审计链 %1 笔").arg(rows.size()));
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
              frame_src_ = pm;
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
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);

  // 连接区（原型 conn-row：字段行＋右端已连接徽章）
  auto* conn = new QHBoxLayout;
  conn->setSpacing(6);
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
  conn->addStretch(1);
  conn_badge_ = new QLabel(QStringLiteral("未连接"), this);
  conn_badge_->setObjectName(QStringLiteral("assist_conn_badge"));
  conn->addWidget(conn_badge_);
  layout->addLayout(conn);
  connect(btn_connect_, &QPushButton::clicked, this,
          [this, host, port, account_box, password] {
            connect_to(resolve_conn_host(host->text()), port->text().toUShort(),
                       account_box->text(), password->text());
          });

  // sec-h 小工具（原型 .sec-h：粗题＋灰副题——副题走默认调色板随主题换）
  auto bold_lbl = [this](const QString& text) {
    auto* l = new QLabel(text, this);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
  };

  auto* cols = new QHBoxLayout;
  cols->setSpacing(8);

  // —— 左列：我发起协助（发起方）——
  auto* left = new QVBoxLayout;
  left->setSpacing(8);

  // 卡：我发起协助（原型 .card）
  {
    auto* card = new QGroupBox(QStringLiteral("我发起协助"), this);
    auto* body = new QVBoxLayout(card);
    body->setContentsMargins(10, 6, 10, 10);
    body->setSpacing(8);
    auto* rrow = new QHBoxLayout;
    target_edit_ = new QLineEdit(card);
    target_edit_->setPlaceholderText(QStringLiteral("受控方账号（如 liwen）"));
    chk_view_ = new QCheckBox(QStringLiteral("查看"), card);
    chk_view_->setChecked(true);
    chk_mouse_ = new QCheckBox(QStringLiteral("鼠标"), card);
    chk_kb_ = new QCheckBox(QStringLiteral("键盘"), card);
    btn_request_ = new QPushButton(QStringLiteral("发起协助"), card);
    rrow->addWidget(target_edit_, 1);
    rrow->addWidget(chk_view_);
    rrow->addWidget(chk_mouse_);
    rrow->addWidget(chk_kb_);
    rrow->addWidget(btn_request_);
    body->addLayout(rrow);
    auto* n = new QLabel(
        QStringLiteral("consent 只属受控方——发起后等对方批准；对方可缩权实"
                       "批（实批 ⊆ 申请）。"),
        card);
    n->setWordWrap(true);
    body->addWidget(n);
    left->addWidget(card);
  }

  // 会话台账（原型 .ledger：头行＋五态行）
  {
    auto* led_head = new QHBoxLayout;
    led_head->addWidget(bold_lbl(QStringLiteral("会话台账（过程持续可见）")));
    led_head->addWidget(new QLabel(
        QStringLiteral("五态：待批 / 已批待启动 / 进行中 / 已结束 / 已拒绝"),
        this));
    led_head->addStretch(1);
    left->addLayout(led_head);
  }
  sessions_ = new QListWidget(this);
  left->addWidget(sessions_, 2);

  // 动作行（原型 .btnrow：启动/结束/审计/刷新＋尾端选中提示）
  auto* orow = new QHBoxLayout;
  btn_start_ = new QPushButton(QStringLiteral("启动选中"), this);
  btn_end_ = new QPushButton(QStringLiteral("结束选中"), this);
  btn_audit_ = new QPushButton(QStringLiteral("看审计链"), this);
  btn_refresh_ = new QPushButton(QStringLiteral("刷新"), this);
  orow->addWidget(btn_start_);
  orow->addWidget(btn_end_);
  orow->addWidget(btn_audit_);
  orow->addWidget(btn_refresh_);
  orow->addStretch(1);
  sel_lbl_ = new QLabel(this);
  orow->addWidget(sel_lbl_);
  left->addLayout(orow);

  // 远端画面（原型 .remote：深底画面区）
  frame_ = new QLabel(
      QStringLiteral("（远端画面——选中进行中的会话自动显示）"
                     " · 点击即回发鼠标事件"),
      this);
  frame_->setMinimumSize(320, 190);
  frame_->setAlignment(Qt::AlignCenter);
  frame_->setStyleSheet(
      QStringLiteral("background:#222;color:#bbb;font-size:12px;"));
  frame_->installEventFilter(this); // 点画面→发鼠标事件
  left->addWidget(frame_, 1);

  // 审计链 sec-h（计数副题随拉取更新）
  {
    auto* aud_head = new QHBoxLayout;
    aud_head->addWidget(bold_lbl(QStringLiteral("审计链（状态迁移留痕）")));
    audit_sub_ = new QLabel(this);
    aud_head->addWidget(audit_sub_);
    aud_head->addStretch(1);
    left->addLayout(aud_head);
  }
  audit_ = new QListWidget(this);
  {
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    audit_->setFont(mono);
  }
  audit_->setMaximumHeight(140);
  left->addWidget(audit_);
  cols->addLayout(left, 1);

  // —— 右列：别人协助我（受控方）——
  auto* right = new QVBoxLayout;
  right->setSpacing(8);

  // 卡：待我批（原型 .card＋pending 虚线框：待批行＋批准勾选项＋配对密码行）
  {
    auto* card = new QGroupBox(
        QStringLiteral("待我批（consent 只属受控方）"), this);
    auto* body = new QVBoxLayout(card);
    body->setContentsMargins(10, 6, 10, 10);
    body->setSpacing(8);
    pending_ = new QListWidget(card);
    body->addWidget(pending_, 1);
    auto* crow = new QHBoxLayout;
    crow->addWidget(bold_lbl(QStringLiteral("批准勾选项")));
    gview_ = new QCheckBox(QStringLiteral("查看"), card);
    gview_->setChecked(true);
    gmouse_ = new QCheckBox(QStringLiteral("鼠标"), card);
    gkb_ = new QCheckBox(QStringLiteral("键盘"), card);
    crow->addWidget(gview_);
    crow->addWidget(gmouse_);
    crow->addWidget(gkb_);
    crow->addStretch(1);
    body->addLayout(crow);
    auto* gnote = new QLabel(
        QStringLiteral("实批可小于申请（缩权批准）；批准前输入配对密码。"),
        card);
    gnote->setWordWrap(true);
    body->addWidget(gnote);
    auto* prow = new QHBoxLayout;
    pwd_edit_ = new QLineEdit(card);
    pwd_edit_->setPlaceholderText(QStringLiteral("远程控制配对密码"));
    pwd_edit_->setEchoMode(QLineEdit::Password);
    btn_approve_ = new QPushButton(QStringLiteral("批准"), card);
    btn_deny_ = new QPushButton(QStringLiteral("拒绝"), card);
    btn_deny_->setStyleSheet(
        QStringLiteral("border-color:rgba(196,58,45,0.45);color:#c43a2d;"));
    prow->addWidget(pwd_edit_, 1);
    prow->addWidget(btn_approve_);
    prow->addWidget(btn_deny_);
    body->addLayout(prow);
    right->addWidget(card, 1);
  }

  // 共享横幅（原型 .share-banner：共享中红色警示，未共享降级说明）
  share_label_ = new QLabel(this);
  share_label_->setWordWrap(true);
  share_label_->setStyleSheet(
      QStringLiteral("background:rgba(192,57,43,0.10);"
                     "border:1px solid rgba(192,57,43,0.40);"
                     "border-radius:6px;padding:8px 10px;color:#c0392b;"
                     "font-weight:600;"));
  right->addWidget(share_label_);
  {
    auto* srow = new QHBoxLayout;
    btn_share_ = new QPushButton(QStringLiteral("开始共享屏幕"), this);
    srow->addWidget(btn_share_);
    srow->addStretch(1);
    srow->addWidget(new QLabel(
        QStringLiteral("未共享时显示「有进行中的协助会话（未在共享）」"),
        this));
    right->addLayout(srow);
  }

  // 卡：对方操作（原型 .card .ops：受控侧输入日志，取走即清）
  {
    auto* card = new QGroupBox(QStringLiteral("对方操作（取走即清）"), this);
    auto* body = new QVBoxLayout(card);
    body->setContentsMargins(10, 6, 10, 10);
    body->setSpacing(6);
    body->addWidget(new QLabel(QStringLiteral("受控方视角 · 只显示最近"), card));
    inputs_ = new QListWidget(card);
    {
      QFont mono(QStringLiteral("monospace"));
      mono.setStyleHint(QFont::TypeWriter);
      inputs_->setFont(mono);
    }
    body->addWidget(inputs_, 1);
    right->addWidget(card, 1);
  }

  // 尾注（原型 .note：五态/consent/不落盘红线）
  {
    auto* tail = new QLabel(
        QStringLiteral("五态固定：待批 → 已批待启动 → 进行中 → 已结束 / 已拒"
                       "绝；consent 只属受控方，配对密码防「误批」；屏幕帧"
                       "与操作流不落盘，仅审计链存状态迁移。"),
        this);
    tail->setWordWrap(true);
    tail->setStyleSheet(
        QStringLiteral("background:rgba(47,109,219,0.08);border-radius:6px;"
                       "padding:7px 10px;"));
    right->addWidget(tail);
  }
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
    // 前须输对配对密码（内联框直输，同 try_approve 一条路）；拒绝不走
    // 此门（密码关着时直批同旧路径）。
    if (remote_control::enabled()) {
      if (pwd_edit_->text().isEmpty()) {
        set_status(QStringLiteral("批准前请输入配对密码"), true);
        return;
      }
      const QString err = try_approve(pwd_edit_->text(), grant_checks());
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
    sel_lbl_->setText(QStringLiteral("选中 %1").arg(short_id(selected_session())));
  });

  // 轮询：台账保鲜＋受控侧推帧＋发起侧拉帧＋受控侧拉输入
  timer_ = new QTimer(this);
  connect(timer_, &QTimer::timeout, this, [this] {
    if (!is_connected()) return;
    client_->fetch_assist_sessions();
    if (sharing_) {
      const QString share_id = active_id_as_target();
      if (!share_id.isEmpty()) {
        // 测试注帧优先（离屏抓屏为垃圾帧——push_frame_once 的绕行口径）；
        // 生产路径无注入，恒走真抓屏
        if (!injected_frame_b64_.isEmpty()) {
          client_->push_assist_frame(share_id, frame_seq_ + 1,
                                     injected_frame_b64_);
        } else {
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
    }
    const QString view_id = active_id_as_requester();
    if (!view_id.isEmpty()) client_->pull_assist_frame(view_id);
    const QString in_id = active_id_as_target();
    if (!in_id.isEmpty()) client_->fetch_assist_input(in_id);
  });
}

bool AssistDialog::eventFilter(QObject* watched, QEvent* ev) {
  // 画面框尺寸变化（含隐藏期首帧后 show 首次布局）→按新框重缩放原图，
  // 目标尺寸未变则跳过（setPixmap 会再触发布局，守卫防回环）
  if (watched == frame_ && ev->type() == QEvent::Resize) {
    const auto* re = static_cast<const QResizeEvent*>(ev);
    if (!frame_src_.isNull()) {
      const QSize target = frame_src_.size().scaled(re->size(),
                                                    Qt::KeepAspectRatio);
      const QPixmap cur = frame_->pixmap();
      if (cur.isNull() || cur.size() != target) {
        frame_->setPixmap(frame_src_.scaled(
            target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      }
    }
  }
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
  injected_frame_b64_ = jpeg_b64; // 后续轮询推帧同源（离屏抓屏垃圾帧口径）
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
