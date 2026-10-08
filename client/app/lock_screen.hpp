// 离开锁屏（用户令 2026-10-08 ④）：无操作超时自动锁屏——
// 锁屏面只显示未读消息数量，不显示任何内容；输密码解锁。
// away_lock 命名空间＝设置落盘面（QSettings，密码只存盐＋SHA-256，不存明文）。
#pragma once

#include <QDialog>

#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;

namespace memex::client {

namespace away_lock {
bool enabled();
void set_enabled(bool on);
int timeout_seconds();          // 无操作超时（秒；默认 300＝5 分钟）
void set_timeout_seconds(int seconds);
bool has_password();
bool set_password(const QString& pwd); // 空串拒（返回 false）；成功＝生成新盐落盘
bool verify_password(const QString& pwd);
} // namespace away_lock

// 锁屏遮罩面：无边框置顶全屏；Esc/关闭钮一律拒（closeEvent 吞掉），
// 唯一出路＝密码校验通过（verifier 由主窗注入＝away_lock::verify_password）。
// 期间新到消息经 bump_unread() 只累计条数——弹窗与提示音由主窗 event_notify
// 统一抑制（内容不出锁屏面）。
class LockScreenDialog : public QDialog {
  Q_OBJECT
public:
  explicit LockScreenDialog(std::function<bool(const QString&)> verifier,
                            QWidget* parent = nullptr);
  void bump_unread();       // 锁屏期间新到一条消息/文件（上线不计）
  int unread_count() const { return unread_; }
  QString unread_text() const; // 锁屏面未读行文案（验收面：只含数量不含内容）
  void try_unlock(const QString& pwd); // 对＝accept 解锁；错＝红字不动

protected:
  void keyPressEvent(QKeyEvent* event) override; // 吞 Esc，防键盘绕过
  void closeEvent(QCloseEvent* event) override;  // 吞关闭，防任务栏/Alt+F4 绕过

private:
  std::function<bool(const QString&)> verifier_;
  QLabel* lbl_head_{nullptr};
  QLabel* lbl_unread_{nullptr};
  QLineEdit* edit_pwd_{nullptr};
  QLabel* lbl_state_{nullptr};
  QPushButton* btn_unlock_{nullptr};
  int unread_{0};
};

} // namespace memex::client
