// 通知中心（T4.10）：分级推送执行体。引擎 notice_received → 按个人偏好裁决：
//   普通＝仅站内会话消息（不弹窗，默认关）；
//   重要＝桌面通知强提醒（托盘 toast，不看窗口激活态）；
//   紧急＝置顶弹窗，点「确认收悉」才关（多条排队）。
// 全屏／演示模式（策略为递延）期间进队列，退出全屏后按当时偏好补弹。
// 托盘句柄在主窗手里，本类经 want_tray_notify 信号交给主窗桥接。
#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "notify_prefs.hpp"

namespace memex::client {

class NotificationCenter : public QObject {
  Q_OBJECT
public:
  static NotificationCenter& instance();

  // 通知偏好设置对话框（三级弹窗开关／免打扰时段／全屏策略）
  void show_settings();

  // 递延队列补弹（退出全屏时由 eventFilter 自动调用；测试可手动触发）
  void flush_deferred();

  // 递延队列长度（测试断言用）
  int deferred_count() const { return deferred_.size(); }

  // 引擎 notice_received 直连入口（签名与信号一致，可直接 connect）
public slots:
  void on_notice(const QString& from, const QString& title,
                 const QString& content, int urgency,
                 const QString& jump_url, qint64 ts_ms,
                 const QString& msg_id);

signals:
  // 请求桌面通知（主窗桥接进托盘 toast——托盘句柄归主窗管）
  void want_tray_notify(const QString& title, const QString& text);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  NotificationCenter();
  ~NotificationCenter() override = default;

  struct Pending {
    NoticeLevel level;
    QString title;
    QString content;
    QString jump;
  };

  // 按当时裁决执行一条（Toast→信号；Modal→置顶弹窗；Defer→入队）
  void dispatch(const Pending& n);
  void show_next_urgent(); // 紧急弹窗队列头出队展示

  QList<Pending> deferred_;   // 全屏期间递延
  QList<Pending> urgent_queue_; // 紧急待确认队列（弹窗一次一条）
  bool urgent_open_{false};   // 当前有紧急弹窗在屏
  bool fullscreen_{false};    // 全屏／演示状态缓存
};

} // namespace memex::client
