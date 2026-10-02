// Memex 主窗口（T1.4 直连态界面 + T2.4 模式切换与降级）：
// 局域网设备列表（替代联系人）+ 聊天窗 + 「未归档」本地态标记 + 空态引导。
// 协作态经菜单登录／登出（不重启切换形态）；双态共用同一份本地库，
// 历史按 source 字段合并展示；服务端不可达回落直连态并常驻提示「消息不进归档」。
#pragma once

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QPushButton>
#include <QTextBrowser>

#include <engine/direct/direct_engine.hpp>
#include <engine/collab/collab_engine.hpp>

class QAction;

namespace memex::client {

class MainWindow : public QMainWindow {
public:
  explicit MainWindow(QWidget* parent = nullptr);

  // —— T2.4 模式切换入口（菜单与验收测试共用）——
  // 连接并登录协作态（不重启；结果异步：logged_in／login_failed 信号）。
  void login_collab(const QString& host, quint16 port, const QString& account,
                    const QString& password);
  // 登出协作态，回到直连态（本地历史保留，合并展示不受影响）。
  void logout_collab();
  // 打开（或发起）与某账号的协作会话。
  void open_collab_peer(const QString& account);
  // 按当前会话形态路由发送；true=已受理（回执异步）。
  bool send_in_current_chat(const QString& text);

  // 状态断言面（验收测试）
  bool collab_logged_in() const;
  QString banner_text() const;   // 归档提示条文案（直连／协作两态）
  QString status_text() const;   // 状态栏当前文案（降级提示在此）
  QString chat_html() const;     // 聊天区富文本（合并展示断言）

private:
  void build_ui();
  void wire_engines();
  void wire_collab(); // 协作信号接入界面（T2.4）
  void refresh_devices();
  void open_peer(const QString& device_id);
  void open_chat(const QString& kind, const QString& id);
  void append_message(const QString& from_id, const QString& text,
                      qint64 ts_ms, bool outgoing, const QString& source);
  void append_system_line(const QString& text);
  void show_guidance();
  void show_status(const QString& text);
  void update_banner();          // 按当前形态切换归档提示条
  void seed_collab_peers();      // 登录后从本地库补入历史协作会话
  void show_collab_login_dialog();

  QString current_peer_;      // 当前会话对端（设备标识或协作账号）
  QString current_kind_{QStringLiteral("direct")}; // 会话形态：direct／collab
  QSet<QString> collab_peers_; // 协作会话列表（登录后与本地库历史并集）
  bool collab_was_logged_in_{false}; // 上一轮登录态（降级提示去抖）
  QMap<QString, quint64> file_sent_; // 文件名 → 最近一次进度字节（节流）
  bool chat_showing_guidance_{false}; // 聊天区当前是否为引导态
  QString status_hint_;               // 状态栏事件提示（引擎态前缀实时拼）

  DirectEngine direct_engine_;
  CollabEngine collab_engine_;

  QLabel* device_count_{nullptr};
  QLineEdit* search_box_{nullptr};
  QListWidget* device_list_{nullptr};
  QLabel* chat_title_{nullptr};
  QLabel* chat_meta_{nullptr};
  QLabel* banner_{nullptr}; // 归档提示条（objectName: mode_banner）
  QTextBrowser* chat_view_{nullptr};
  QLineEdit* input_box_{nullptr};
  QPushButton* send_btn_{nullptr};
  QAction* act_collab_logout_{nullptr};
};

} // namespace memex::client
