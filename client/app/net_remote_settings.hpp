// 二期·网络与远程设置（用户令 2026-10-08 ⑤）：网段黑名单＋远程控制
// 配对密码。落盘走 net_guard.hpp（net_blacklist/remote_control 命名空间）；
// 保存回调主窗 apply_net_settings() 即时生效（运行中改表下一批包即按新表判）。
// 口径：两开关默认关；远程控制默认关闭、开启须显式操作（页面注明）。
#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace memex::client {

class MainWindow;

class NetRemoteSettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit NetRemoteSettingsDialog(MainWindow* win, QWidget* parent = nullptr);

  // 程序化缝（与按钮同一路径）：加段过 validate_cidr 本地门（坏段红字
  // 不入表）、删选中段、保存（段表+两开关+配对密码落盘并即时生效）
  QString add_entry(const QString& cidr);
  void del_entry();
  QString save_settings();

  QString status_text() const;

 private:
  MainWindow* win_;
  QLineEdit* edit_cidr_;
  QCheckBox* chk_blacklist_;
  QCheckBox* chk_remote_;
  QLineEdit* edit_pwd_;
  QLineEdit* edit_confirm_;
  QListWidget* list_entries_;
  QLabel* status_;
};

} // namespace memex::client
