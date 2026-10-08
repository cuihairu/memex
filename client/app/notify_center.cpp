#include "notify_center.hpp"

#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>

namespace memex::client {

namespace {

// 当前是否有顶层窗口处于全屏（演示模式按全屏对待——投屏／演示即全屏）
bool any_fullscreen() {
  const auto tops = QApplication::topLevelWidgets();
  for (const auto* w : tops) {
    if (w->isVisible() && w->isFullScreen()) return true;
  }
  return false;
}

QString toast_text(const QString& title, const QString& content,
                   const QString& jump) {
  QString s = title + QStringLiteral("：") + content;
  if (!jump.isEmpty()) s += ' ' + jump;
  return s;
}

} // namespace

NotificationCenter& NotificationCenter::instance() {
  static NotificationCenter center;
  return center;
}

NotificationCenter::NotificationCenter() {
  if (qApp) qApp->installEventFilter(this); // 监听全屏进出以做递延补弹
  fullscreen_ = any_fullscreen();
}

void NotificationCenter::on_notice(const QString& from, const QString& title,
                                   const QString& content, int urgency,
                                   const QString& jump_url, qint64 ts_ms,
                                   const QString& msg_id) {
  Q_UNUSED(from)     // 发送方恒为「通知」（kNoticeSender）
  Q_UNUSED(ts_ms)    // 归档已带时间戳，弹窗不再用
  Q_UNUSED(msg_id)   // 消息级 ACK 已由引擎处理
  dispatch(Pending{level_from_urgency(urgency), title, content, jump_url});
}

void NotificationCenter::dispatch(const Pending& n) {
  const NotifyPrefs prefs = NotifyPrefs::load();
  const bool dnd = dnd_active(prefs, QTime::currentTime());
  fullscreen_ = any_fullscreen();
  const PopupAction act = decide(n.level, prefs, dnd, fullscreen_);
  switch (act) {
  case PopupAction::ChatOnly:
    return;
  case PopupAction::Toast:
    emit want_tray_notify(n.title, toast_text(n.title, n.content, n.jump));
    return;
  case PopupAction::Modal:
    urgent_queue_.append(n);
    show_next_urgent();
    return;
  case PopupAction::Defer:
    deferred_.append(n);
    return;
  }
}

void NotificationCenter::flush_deferred() {
  if (deferred_.isEmpty()) return;
  const auto items = std::exchange(deferred_, {});
  for (const auto& n : items) dispatch(n); // 按退出全屏后的偏好重新裁决
}

void NotificationCenter::show_next_urgent() {
  if (urgent_open_ || urgent_queue_.isEmpty()) return;
  const Pending n = urgent_queue_.takeFirst();
  urgent_open_ = true;

  auto* dlg = new QDialog; // 顶层：可盖在任何窗口之上
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setObjectName(QStringLiteral("notice_urgent_dialog"));
  dlg->setWindowTitle(QStringLiteral("紧急通知 — 需确认收悉"));
  dlg->setWindowFlags(Qt::Dialog | Qt::WindowStaysOnTopHint |
                      Qt::WindowTitleHint | Qt::WindowCloseButtonHint);

  auto* lay = new QVBoxLayout(dlg);
  auto* title = new QLabel(n.title, dlg);
  title->setObjectName(QStringLiteral("notice_title"));
  QFont tf = title->font();
  tf.setBold(true);
  tf.setPointSize(tf.pointSize() + 2);
  title->setFont(tf);
  title->setWordWrap(true);
  lay->addWidget(title);

  auto* body = new QLabel(n.content, dlg);
  body->setObjectName(QStringLiteral("notice_content"));
  body->setWordWrap(true);
  body->setMinimumWidth(360);
  lay->addWidget(body);

  if (!n.jump.isEmpty()) {
    auto* jump = new QLabel(
        QStringLiteral("<a href=\"%1\">%1</a>").arg(n.jump), dlg);
    jump->setObjectName(QStringLiteral("notice_jump"));
    jump->setTextFormat(Qt::RichText);
    jump->setOpenExternalLinks(true);
    lay->addWidget(jump);
  }

  auto* hint = new QLabel(
      QStringLiteral("来自：%1 — 点「%2」后关闭，未确认的通知会继续排队弹出")
          .arg(QStringLiteral("通知"), QStringLiteral("确认收悉")),
      dlg);
  hint->setObjectName(QStringLiteral("notice_hint"));
  lay->addWidget(hint);

  auto* row = new QHBoxLayout;
  row->addStretch(1);
  auto* ack = new QPushButton(QStringLiteral("确认收悉"), dlg);
  ack->setObjectName(QStringLiteral("btn_ack"));
  ack->setDefault(true);
  row->addWidget(ack);
  lay->addLayout(row);

  connect(ack, &QPushButton::clicked, dlg, &QDialog::close);
  connect(dlg, &QDialog::finished, this, [this](int) {
    urgent_open_ = false;
    show_next_urgent(); // 队列里还有就继续弹
  });

  dlg->show();
  dlg->raise();
  dlg->activateWindow();
}

void NotificationCenter::show_settings() {
  const NotifyPrefs prefs = NotifyPrefs::load();
  QDialog dlg;
  dlg.setObjectName(QStringLiteral("notify_settings_dialog"));
  dlg.setWindowTitle(QStringLiteral("通知偏好"));
  auto* lay = new QVBoxLayout(&dlg);

  auto* cb_normal = new QCheckBox(
      QStringLiteral("普通通知弹窗（默认关＝仅站内会话消息）"), &dlg);
  cb_normal->setObjectName(QStringLiteral("chk_normal"));
  cb_normal->setChecked(prefs.popup_normal);
  lay->addWidget(cb_normal);

  auto* cb_important = new QCheckBox(
      QStringLiteral("重要通知桌面提醒（强提醒，不看窗口激活态）"), &dlg);
  cb_important->setObjectName(QStringLiteral("chk_important"));
  cb_important->setChecked(prefs.popup_important);
  lay->addWidget(cb_important);

  auto* cb_urgent = new QCheckBox(
      QStringLiteral("紧急通知置顶弹窗（需点击确认收悉）"), &dlg);
  cb_urgent->setObjectName(QStringLiteral("chk_urgent"));
  cb_urgent->setChecked(prefs.popup_urgent);
  lay->addWidget(cb_urgent);

  auto* cb_flash = new QCheckBox(
      QStringLiteral(
          "新消息闪烁提醒（收消息时窗口/任务栏闪烁；仅窗口非激活时，"
          "激活中不闪）"), &dlg);
  cb_flash->setObjectName(QStringLiteral("chk_flash"));
  cb_flash->setChecked(prefs.flash_alert);
  lay->addWidget(cb_flash);

  auto* dnd_title = new QLabel(QStringLiteral("免打扰时段"), &dlg);
  dnd_title->setObjectName(QStringLiteral("lbl_dnd"));
  QFont bf = dnd_title->font();
  bf.setBold(true);
  dnd_title->setFont(bf);
  lay->addWidget(dnd_title);

  auto* cb_dnd = new QCheckBox(
      QStringLiteral("启用免打扰时段（普通／重要静默；紧急仍弹）"), &dlg);
  cb_dnd->setObjectName(QStringLiteral("chk_dnd"));
  cb_dnd->setChecked(prefs.dnd);
  lay->addWidget(cb_dnd);

  auto* dnd_row = new QHBoxLayout;
  auto* t_start = new QTimeEdit(&dlg);
  t_start->setObjectName(QStringLiteral("time_dnd_start"));
  t_start->setDisplayFormat(QStringLiteral("HH:mm"));
  t_start->setTime(QTime::fromString(prefs.dnd_start, QStringLiteral("HH:mm")));
  auto* t_end = new QTimeEdit(&dlg);
  t_end->setObjectName(QStringLiteral("time_dnd_end"));
  t_end->setDisplayFormat(QStringLiteral("HH:mm"));
  t_end->setTime(QTime::fromString(prefs.dnd_end, QStringLiteral("HH:mm")));
  dnd_row->addWidget(new QLabel(QStringLiteral("从"), &dlg));
  dnd_row->addWidget(t_start);
  dnd_row->addWidget(new QLabel(QStringLiteral("到"), &dlg));
  dnd_row->addWidget(t_end);
  dnd_row->addStretch(1);
  lay->addLayout(dnd_row);

  auto* fs_label = new QLabel(QStringLiteral("全屏／演示模式策略"), &dlg);
  fs_label->setObjectName(QStringLiteral("lbl_fullscreen"));
  lay->addWidget(fs_label);
  auto* cmb_fs = new QComboBox(&dlg);
  cmb_fs->setObjectName(QStringLiteral("cmb_fullscreen"));
  cmb_fs->addItem(QStringLiteral("递延弹窗（退出全屏后补弹）"));
  cmb_fs->addItem(QStringLiteral("照常弹窗"));
  cmb_fs->setCurrentIndex(prefs.fullscreen_allow ? 1 : 0);
  lay->addWidget(cmb_fs);

  // —— 事件通知开关组（用户令 2026-10-08：每项独立开关）——
  auto* ev_title = new QLabel(QStringLiteral("事件通知"), &dlg);
  ev_title->setObjectName(QStringLiteral("lbl_events"));
  ev_title->setFont(bf);
  lay->addWidget(ev_title);
  auto* cb_peer_online = new QCheckBox(
      QStringLiteral("联系人上线→弹通知窗（我上线会出现在对方在线列表，"
                     "弹不弹由对方此开关决定）"), &dlg);
  cb_peer_online->setObjectName(QStringLiteral("chk_peer_online"));
  cb_peer_online->setChecked(prefs.popup_peer_online);
  lay->addWidget(cb_peer_online);
  auto* cb_message = new QCheckBox(
      QStringLiteral("收到消息→弹通知窗（窗口非激活时；显示会话名＋摘要）"),
      &dlg);
  cb_message->setObjectName(QStringLiteral("chk_popup_message"));
  cb_message->setChecked(prefs.popup_message);
  lay->addWidget(cb_message);
  auto* cb_file_arrive = new QCheckBox(
      QStringLiteral("收到文件→弹通知窗"), &dlg);
  cb_file_arrive->setObjectName(QStringLiteral("chk_popup_file"));
  cb_file_arrive->setChecked(prefs.popup_file_arrive);
  lay->addWidget(cb_file_arrive);
  auto* cb_transfer_done = new QCheckBox(
      QStringLiteral("文件传输成功→弹通知窗（默认关＝成功是常态不逐次打扰）"),
      &dlg);
  cb_transfer_done->setObjectName(QStringLiteral("chk_popup_transfer"));
  cb_transfer_done->setChecked(prefs.popup_transfer_done);
  lay->addWidget(cb_transfer_done);

  // —— 提示音三档（用户令 2026-10-08，设置项单列）——
  auto* sound_title = new QLabel(QStringLiteral("提示音"), &dlg);
  sound_title->setObjectName(QStringLiteral("lbl_sound"));
  sound_title->setFont(bf);
  lay->addWidget(sound_title);
  auto* cmb_sound = new QComboBox(&dlg);
  cmb_sound->setObjectName(QStringLiteral("cmb_sound_mode"));
  cmb_sound->addItem(QStringLiteral("全部关闭"));
  cmb_sound->addItem(QStringLiteral("每条都播"));
  cmb_sound->addItem(QStringLiteral("按事件类型"));
  cmb_sound->setCurrentIndex(prefs.sound_mode);
  lay->addWidget(cmb_sound);
  auto* snd_row = new QHBoxLayout;
  auto* cb_sound_msg = new QCheckBox(QStringLiteral("消息"), &dlg);
  cb_sound_msg->setObjectName(QStringLiteral("chk_sound_msg"));
  auto* cb_sound_file = new QCheckBox(QStringLiteral("文件"), &dlg);
  cb_sound_file->setObjectName(QStringLiteral("chk_sound_file"));
  auto* cb_sound_online = new QCheckBox(QStringLiteral("上线"), &dlg);
  cb_sound_online->setObjectName(QStringLiteral("chk_sound_online"));
  for (auto* cb : {cb_sound_msg, cb_sound_file, cb_sound_online}) {
    snd_row->addWidget(cb);
  }
  snd_row->addStretch(1);
  lay->addLayout(snd_row);
  const auto sync_sound_row = [cmb_sound, cb_sound_msg, cb_sound_file,
                               cb_sound_online] {
    const bool on = cmb_sound->currentIndex() == 2;
    cb_sound_msg->setEnabled(on);
    cb_sound_file->setEnabled(on);
    cb_sound_online->setEnabled(on);
  };
  cb_sound_msg->setChecked(prefs.sound_msg);
  cb_sound_file->setChecked(prefs.sound_file);
  cb_sound_online->setChecked(prefs.sound_online);
  sync_sound_row();
  connect(cmb_sound, &QComboBox::currentIndexChanged, &dlg,
          [sync_sound_row] { sync_sound_row(); });

  auto* note = new QLabel(
      QStringLiteral("免打扰时段支持跨零点（如 22:00–08:00）；"
                     "紧急通知不受免打扰影响，必须确认收悉。闪烁开关只管"
                     "窗口/任务栏闪烁——托盘气泡走上方分级偏好，不受它影响。"),
      &dlg);
  note->setObjectName(QStringLiteral("lbl_note"));
  note->setWordWrap(true);
  lay->addWidget(note);

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                           &dlg);
  buttons->setObjectName(QStringLiteral("btn_settings"));
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  lay->addWidget(buttons);

  connect(&dlg, &QDialog::finished, &dlg, [&](int result) {
    if (result != QDialog::Accepted) return;
    NotifyPrefs out;
    out.popup_normal = cb_normal->isChecked();
    out.popup_important = cb_important->isChecked();
    out.popup_urgent = cb_urgent->isChecked();
    out.dnd = cb_dnd->isChecked();
    out.dnd_start = t_start->time().toString(QStringLiteral("HH:mm"));
    out.dnd_end = t_end->time().toString(QStringLiteral("HH:mm"));
    out.fullscreen_allow = cmb_fs->currentIndex() == 1;
    out.flash_alert = cb_flash->isChecked();
    out.popup_peer_online = cb_peer_online->isChecked();
    out.popup_message = cb_message->isChecked();
    out.popup_file_arrive = cb_file_arrive->isChecked();
    out.popup_transfer_done = cb_transfer_done->isChecked();
    out.sound_mode = cmb_sound->currentIndex();
    out.sound_msg = cb_sound_msg->isChecked();
    out.sound_file = cb_sound_file->isChecked();
    out.sound_online = cb_sound_online->isChecked();
    out.save();
  });
  dlg.exec();
}

bool NotificationCenter::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::WindowStateChange) {
    const bool now = any_fullscreen();
    if (fullscreen_ && !now) flush_deferred(); // 刚退出全屏：补弹
    fullscreen_ = now;
  }
  return QObject::eventFilter(watched, event);
}

} // namespace memex::client
