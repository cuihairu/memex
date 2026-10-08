// 二期·截图快捷键设置（用户令 2026-10-08 ③：Ctrl+Alt+A 可在设置改键，
// 冲突检测提示）。冲突检测在 MainWindow::apply_screenshot_shortcut
// （应用内已注册键序列；系统级占用按平台能力另批，页面注明）。
#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QKeySequenceEdit;

namespace memex::client {

class MainWindow;

class ShortcutSettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ShortcutSettingsDialog(MainWindow* win,
                                  QWidget* parent = nullptr);

  // 程序化保存（与「保存」按钮同一路径；冲突＝状态行明示不关窗）
  void save_sequence(const QKeySequence& seq);
  QString status_text() const;

 private:
  MainWindow* win_;
  QKeySequenceEdit* editor_;
  QLabel* status_;
};

} // namespace memex::client
