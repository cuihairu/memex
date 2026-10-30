// 二期·设置窗口（原型 settings-general/account/message/privacy/advanced
// 统一设置面）：220px 侧栏导航（通用/账号/消息/隐私/高级）＋右侧按页
// 堆叠内容。逐页控件与真实功能接线；尚未实现的行置灰＋「未提供」注
// （不造假控件，同原型 footnote 口径）。主题走 ThemeManager；开机启动
// 与截图快捷键（含快捷键设置对话框）走 MainWindow（判权与冲突检测
// 权威在主窗）。
#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QShowEvent;
class QStackedWidget;

namespace memex::client {

class MainWindow;

class SettingsWindow : public QDialog {
  Q_OBJECT
 public:
  // win 为空＝测试隔离（无主窗接线——开机启动/快捷键行置灰＋「未接入」注）
  explicit SettingsWindow(MainWindow* win = nullptr,
                          QWidget* parent = nullptr);

  // 程序化切页（与侧栏点选同源；返回是否切换成功）
  bool show_page(const QString& id);
  QString current_page() const; // "general"/"account"/"message"/"privacy"/"advanced"
  QString page_title() const;   // 当前页标题

  // —— 通用页观察点（测试/走查）——
  QString theme_mode() const;        // 主题下拉当前值（"system"/"light"/"dark"）
  QString download_dir_text() const; // 下载位置字段（只读真实目录）
  QString shortcut_text() const;     // 快捷键按钮文案
  bool autostart_checked() const;    // 开机启动开关态

 private:
  void build_ui();
  QWidget* build_general_page();
  QWidget* build_account_page();
  QWidget* build_stub_page(const QString& title, const QString& sub,
                           const QString& note);
  void refresh_account_page(); // 头像/昵称/登录态开关按真实状态回填
  void showEvent(QShowEvent* e) override;

  MainWindow* win_;
  QListWidget* nav_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  // 通用页控件
  QComboBox* theme_combo_ = nullptr;
  QCheckBox* autostart_check_ = nullptr;
  QPushButton* shortcut_btn_ = nullptr;
  QLineEdit* download_dir_ = nullptr;
  // 账号页控件
  QLabel* acct_avatar_ = nullptr;   // 头像（42px 圆角；登录态回填）
  QLineEdit* acct_name_ = nullptr;  // 昵称只读字段
  QPushButton* acct_avatar_btn_ = nullptr;  // 更换头像（登录门＋文件面门）
  QPushButton* acct_profile_btn_ = nullptr; // 编辑资料（签名编辑对话框）
  QPushButton* acct_logout_btn_ = nullptr;  // 退出登录
};

} // namespace memex::client
