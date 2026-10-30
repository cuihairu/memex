// 二期·设置窗口（原型 settings-general/account/message/privacy/advanced
// 统一设置面）：220px 侧栏导航（通用/账号/消息/隐私/高级）＋右侧按页
// 堆叠内容。逐页控件与真实功能接线；尚未实现的行置灰＋「未提供」注
// （不造假控件，同原型 footnote 口径）。主题走 ThemeManager；开机启动
// 与截图快捷键（含快捷键设置对话框）走 MainWindow（判权与冲突检测
// 权威在主窗）；消息页通知/免打扰/提示音走 NotifyPrefs（load→改→save，
// 只动本页字段，不碰未展示的分级/全屏/传输完成/声音子位），已读回执走
// MainWindow（空 peer＝全局）；隐私页网段黑名单/离开锁屏走 MainWindow
// 共用入口，存储说明列真实本机数据目录；高级页网段黑名单开关走
// net_blacklist 真实键，域名解析覆盖/关于走 MainWindow 共用入口，版本标签
// 列真实安装版本，代理/开发者选项/运行日志置灰「暂未提供」。
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
class QTimeEdit;

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

  // —— 消息页观察点（测试/走查）——
  bool msg_desktop_notif() const; // 桌面通知（popup_message）
  bool msg_flash() const;         // 窗口闪烁（flash_alert）
  bool msg_peer_online() const;   // 联系人上线通知（popup_peer_online）
  bool msg_file_arrive() const;   // 文件到达通知（popup_file_arrive）
  int msg_sound_mode() const;     // 提示音三档（0 关/1 每条/2 按事件）
  bool msg_dnd() const;           // 启用免打扰（dnd）
  QString msg_dnd_start() const;  // 免打扰起始 "HH:mm"
  QString msg_dnd_end() const;    // 免打扰结束 "HH:mm"
  bool msg_read_receipts() const; // 已读回执全局（receipts/send_read）

 private:
  void build_ui();
  QWidget* build_general_page();
  QWidget* build_account_page();
  QWidget* build_message_page();
  QWidget* build_privacy_page();
  QWidget* build_advanced_page();
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
  // 消息页控件（NotifyPrefs 直写；已读回执走主窗）
  QCheckBox* msg_desktop_notif_ = nullptr;  // popup_message
  QCheckBox* msg_flash_ = nullptr;          // flash_alert
  QCheckBox* msg_peer_online_ = nullptr;    // popup_peer_online
  QCheckBox* msg_file_arrive_ = nullptr;    // popup_file_arrive
  QComboBox* msg_sound_mode_ = nullptr;     // sound_mode 0/1/2
  QCheckBox* msg_dnd_ = nullptr;            // dnd
  QTimeEdit* msg_dnd_start_ = nullptr;      // dnd_start "HH:mm"
  QTimeEdit* msg_dnd_end_ = nullptr;        // dnd_end "HH:mm"
  QCheckBox* msg_read_receipt_ = nullptr;   // receipts/send_read（空 peer）
  // 高级页控件
  QCheckBox* adv_blacklist_ = nullptr;      // net_blacklist 真实键开关
};

} // namespace memex::client
