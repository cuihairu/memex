#include "notify_prefs.hpp"

#include <QCoreApplication>
#include <QSettings>

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

} // namespace memex::client
