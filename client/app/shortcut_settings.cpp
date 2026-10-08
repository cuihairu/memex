// 二期·截图快捷键设置（实现）。判冲突的唯一权威在 MainWindow
// （apply_screenshot_shortcut：应用内其他 QShortcut／带键 QAction）；
// 本页只做输入、预填与结果展示——冲突红字不关窗，成功即存即生效。
#include "shortcut_settings.hpp"

#include <QKeySequenceEdit>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QVBoxLayout>

#include "main_window.hpp"

namespace memex::client {

ShortcutSettingsDialog::ShortcutSettingsDialog(MainWindow* win,
                                               QWidget* parent)
    : QDialog(parent), win_(win) {
  setWindowTitle(QStringLiteral("快捷键设置"));
  resize(420, 160);
  auto* lay = new QVBoxLayout(this);

  lay->addWidget(new QLabel(QStringLiteral("截图快捷键"), this));
  editor_ = new QKeySequenceEdit(
      QKeySequence(MainWindow::screenshot_shortcut()), this);
  editor_->setObjectName(QStringLiteral("kse_screenshot"));
  lay->addWidget(editor_);

  auto* row = new QHBoxLayout;
  auto* btn_save = new QPushButton(QStringLiteral("保存"), this);
  btn_save->setObjectName(QStringLiteral("btn_shortcut_save"));
  row->addWidget(btn_save);
  row->addStretch(1);
  lay->addLayout(row);

  status_ = new QLabel(
      QStringLiteral("与窗口内其他快捷键冲突时会提示并保持原键；"
                     "系统级全局热键按平台能力另批（Wayland 不放行全局钩子）。"),
      this);
  status_->setObjectName(QStringLiteral("lbl_shortcut_status"));
  status_->setWordWrap(true);
  lay->addWidget(status_);

  connect(btn_save, &QPushButton::clicked, this,
          [this] { save_sequence(editor_->keySequence());
});
}

void ShortcutSettingsDialog::save_sequence(const QKeySequence& seq) {
  const QString conflict = win_->apply_screenshot_shortcut(seq);
  if (!conflict.isEmpty()) {
    status_->setText(conflict);
    status_->setStyleSheet(QStringLiteral("color:#c0392b;"));
    return; // 冲突不关窗（明示后可改）
  }
  status_->setText(QStringLiteral("已保存：") +
                   MainWindow::screenshot_shortcut());
  status_->setStyleSheet(QString());
  accept();
}

QString ShortcutSettingsDialog::status_text() const {
  return status_->text();
}

} // namespace memex::client
