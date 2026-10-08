#include "notify_prefs.hpp"

#include <QCoreApplication>
#include <QSettings>
#include <QSet>

namespace memex::client {

namespace {

// QSettings 键（与主题偏好 appearance/* 同一落盘面：org=memex app=Memex）
constexpr auto kPopupNormal = "notify/popup_normal";
constexpr auto kPopupImportant = "notify/popup_important";
constexpr auto kPopupUrgent = "notify/popup_urgent";
constexpr auto kFlashAlert = "notify/flash_alert";
constexpr auto kDnd = "notify/dnd";
constexpr auto kDndStart = "notify/dnd_start";
constexpr auto kDndEnd = "notify/dnd_end";
constexpr auto kFullscreen = "notify/fullscreen_policy"; // defer | allow
constexpr auto kPopupPeerOnline = "notify/popup_peer_online";
constexpr auto kPopupMessage = "notify/popup_message";
constexpr auto kPopupFileArrive = "notify/popup_file_arrive";
constexpr auto kPopupTransferDone = "notify/popup_transfer_done";
constexpr auto kSoundMode = "notify/sound_mode";
constexpr auto kSoundMsg = "notify/sound_msg";
constexpr auto kSoundFile = "notify/sound_file";
constexpr auto kSoundOnline = "notify/sound_online";

QSettings make_settings() {
  return QSettings(QCoreApplication::organizationName(),
                   QCoreApplication::applicationName());
}

} // namespace

NotifyPrefs NotifyPrefs::load() {
  NotifyPrefs p;
  const auto s = make_settings();
  p.popup_normal = s.value(kPopupNormal, p.popup_normal).toBool();
  p.popup_important = s.value(kPopupImportant, p.popup_important).toBool();
  p.popup_urgent = s.value(kPopupUrgent, p.popup_urgent).toBool();
  p.flash_alert = s.value(kFlashAlert, p.flash_alert).toBool();
  p.dnd = s.value(kDnd, p.dnd).toBool();
  p.dnd_start = s.value(kDndStart, p.dnd_start).toString();
  p.dnd_end = s.value(kDndEnd, p.dnd_end).toString();
  p.fullscreen_allow =
      s.value(kFullscreen, QStringLiteral("defer")).toString() ==
      QStringLiteral("allow");
  p.popup_peer_online = s.value(kPopupPeerOnline, p.popup_peer_online).toBool();
  p.popup_message = s.value(kPopupMessage, p.popup_message).toBool();
  p.popup_file_arrive =
      s.value(kPopupFileArrive, p.popup_file_arrive).toBool();
  p.popup_transfer_done =
      s.value(kPopupTransferDone, p.popup_transfer_done).toBool();
  p.sound_mode = s.value(kSoundMode, p.sound_mode).toInt();
  if (p.sound_mode < 0 || p.sound_mode > 2) p.sound_mode = 0; // 坏值回落全关
  p.sound_msg = s.value(kSoundMsg, p.sound_msg).toBool();
  p.sound_file = s.value(kSoundFile, p.sound_file).toBool();
  p.sound_online = s.value(kSoundOnline, p.sound_online).toBool();
  return p;
}

void NotifyPrefs::save() const {
  auto s = make_settings();
  s.setValue(kPopupNormal, popup_normal);
  s.setValue(kPopupImportant, popup_important);
  s.setValue(kPopupUrgent, popup_urgent);
  s.setValue(kFlashAlert, flash_alert);
  s.setValue(kDnd, dnd);
  s.setValue(kDndStart, dnd_start);
  s.setValue(kDndEnd, dnd_end);
  s.setValue(kFullscreen,
             fullscreen_allow ? QStringLiteral("allow")
                              : QStringLiteral("defer"));
  s.setValue(kPopupPeerOnline, popup_peer_online);
  s.setValue(kPopupMessage, popup_message);
  s.setValue(kPopupFileArrive, popup_file_arrive);
  s.setValue(kPopupTransferDone, popup_transfer_done);
  s.setValue(kSoundMode, sound_mode);
  s.setValue(kSoundMsg, sound_msg);
  s.setValue(kSoundFile, sound_file);
  s.setValue(kSoundOnline, sound_online);
}

bool dnd_active(const NotifyPrefs& p, const QTime& now) {
  if (!p.dnd) return false;
  const QTime start = QTime::fromString(p.dnd_start, QStringLiteral("HH:mm"));
  const QTime end = QTime::fromString(p.dnd_end, QStringLiteral("HH:mm"));
  if (!start.isValid() || !end.isValid() || start == end) return false;
  if (start < end) return now >= start && now < end;
  return now >= start || now < end; // 跨零点：22:00–08:00
}

PopupAction decide(NoticeLevel level, const NotifyPrefs& p, bool dnd_on,
                   bool fullscreen_on) {
  // ① 该级弹窗关闭＝仅站内消息
  switch (level) {
  case NoticeLevel::Important:
    if (!p.popup_important) return PopupAction::ChatOnly;
    break;
  case NoticeLevel::Urgent:
    if (!p.popup_urgent) return PopupAction::ChatOnly;
    break;
  case NoticeLevel::Normal:
  default:
    if (!p.popup_normal) return PopupAction::ChatOnly;
    break;
  }
  // ② 免打扰：普通／重要静默；紧急不静默（需确认收悉的提醒不因免打扰丢）
  if (dnd_on && level != NoticeLevel::Urgent) return PopupAction::ChatOnly;
  // ③ 全屏／演示模式：策略为递延则先进队列，退出全屏后补弹
  if (fullscreen_on && !p.fullscreen_allow) return PopupAction::Defer;
  return level == NoticeLevel::Urgent ? PopupAction::Modal
                                      : PopupAction::Toast;
}

NoticeLevel level_from_urgency(int urgency) {
  switch (urgency) {
  case 2: return NoticeLevel::Important;
  case 3: return NoticeLevel::Urgent;
  default: return NoticeLevel::Normal; // 0 未指定与未知值按普通
  }
}

bool sound_should_play(int mode, bool sound_msg, bool sound_file,
                       bool sound_online, SoundEvent ev) {
  switch (mode) {
  case 1: return true; // 每条都播
  case 2:              // 按事件类型
    switch (ev) {
    case SoundEvent::Message: return sound_msg;
    case SoundEvent::File: return sound_file;
    case SoundEvent::Online: return sound_online;
    }
    return false;
  default: return false; // 全部关闭（未知 mode 值同此）
  }
}

QStringList presence_joined(const QStringList& prev, const QStringList& now) {
  const QSet<QString> old(prev.cbegin(), prev.cend());
  QStringList out;
  for (const QString& a : now) {
    if (!old.contains(a) && !out.contains(a)) out << a;
  }
  return out;
}

} // namespace memex::client
