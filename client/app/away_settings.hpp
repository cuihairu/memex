// 二期·离开锁屏设置（用户令 2026-10-08 ④）：离开密码＋无操作超时＋启用
// 开关。落盘走 away_lock 命名空间（lock_screen.hpp）；保存成功回调主窗
// apply_away_lock_settings() 即时生效。
// 口径：锁屏是本机遮罩（只显未读数不显内容）；presence 在线表无 away
// 状态字段，对端看不到「离开」标记（协议扩列另批，页面注明）。
#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace memex::client {

class MainWindow;

class AwayLockSettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit AwayLockSettingsDialog(MainWindow* win, QWidget* parent = nullptr);

  // 程序化保存（与「保存」按钮同一路径；校验不过＝状态行明示不关窗）
  QString save_settings(bool enable, const QString& pwd,
                        const QString& pwd_confirm, int timeout_min);

  QString status_text() const;

 private:
  MainWindow* win_;
  QCheckBox* chk_enabled_;
  QLineEdit* edit_pwd_;
  QLineEdit* edit_confirm_;
  QSpinBox* spin_timeout_;
  QLabel* status_;
};

} // namespace memex::client
