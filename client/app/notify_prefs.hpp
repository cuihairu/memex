// 个人通知偏好与分级推送裁决（T4.10）：紧急程度三级（普通／重要／紧急）
// 的弹窗开关、免打扰时段、全屏／演示模式策略。裁决是纯函数（decide），
// 状态判定（免打扰）也纯函数化，便于单测穷举覆盖。
#pragma once

#include <QString>
#include <QTime>

namespace memex::client {

// 紧急程度等级（与协议 Notice::Urgency 数值对齐：1 普通／2 重要／3 紧急；
// 0＝未指定，按普通处理）
enum class NoticeLevel { Normal = 0, Important = 1, Urgent = 2 };

// 分级推送动作：
//  ChatOnly —— 仅站内会话消息（不弹任何窗）
//  Toast    —— 桌面通知（系统托盘 toast；重要级＝不看窗口激活态的强提醒）
//  Modal    —— 置顶弹窗，需点击「确认收悉」才关（仅紧急级）
//  Defer    —— 全屏／演示模式期间递延（退出全屏后按当时偏好补弹）
enum class PopupAction { ChatOnly, Toast, Modal, Defer };

struct NotifyPrefs {
  bool popup_normal{false};     // 普通默认不弹（＝仅站内会话消息）
  bool popup_important{true};   // 重要默认桌面通知强提醒
  bool popup_urgent{true};      // 紧急默认置顶弹窗需确认收悉
  bool dnd{false};              // 免打扰时段启用
  QString dnd_start{QStringLiteral("22:00")}; // "HH:mm"
  QString dnd_end{QStringLiteral("08:00")};   // 可跨零点
  bool fullscreen_allow{false}; // 全屏／演示模式：false=递延弹窗，true=照常

  static NotifyPrefs load(); // QSettings（organizationName/applicationName）
  void save() const;
};

// 免打扰判定（纯函数）：区间支持跨零点（start > end 视为跨日）；时间格式
// 非法视为未启用（配置错误不放大成持续静默）。
bool dnd_active(const NotifyPrefs& p, const QTime& now);

// 分级裁决（纯函数）：级别开关 → 免打扰 → 全屏策略。
//  ① 该级弹窗关闭＝仅站内消息；
//  ② 免打扰期间普通／重要静默；紧急不静默（必须确认收悉，不因免打扰丢提醒）；
//  ③ 全屏／演示模式且策略为「递延」＝先进队列，退出全屏后补弹。
PopupAction decide(NoticeLevel level, const NotifyPrefs& p, bool dnd_on,
                   bool fullscreen_on);

// 协议紧急程度数值 → 等级（0 与未知值按普通）
NoticeLevel level_from_urgency(int urgency);

} // namespace memex::client
