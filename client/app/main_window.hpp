// Memex 主窗口（T1.4 直连态界面 + T2.4 模式切换与降级）：
// 局域网设备列表（替代联系人）+ 聊天窗 + 「未归档」本地态标记 + 空态引导。
// 协作态经菜单登录／登出（不重启切换形态）；双态共用同一份本地库，
// 历史按 source 字段合并展示；服务端不可达回落直连态并常驻提示「消息不进归档」。
#pragma once

#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QPushButton>
#include <QStringList>
#include <QTextBrowser>

#include <engine/direct/direct_engine.hpp>
#include <engine/collab/collab_engine.hpp>

#include <app/screenshot_tool.hpp>

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
  // 请求组织架构（登录后；结果缓存于 org_json，界面弹窗与验收共用）
  void request_org();
  QString org_json() const;
  // —— T4.1 群聊（验收面）——
  QString groups_json() const; // 最近一次群列表数据
  void create_group_dialog();  // 服务端群：建群后全量归档
  void open_group(const QString& group_key); // group_key 形如 "group:7"

  // —— T4.2 跨态互通（验收面）——
  // 打开（或发起）与某局域网设备的直连会话。
  void open_direct_peer(const QString& device_id);
  // 是否已发现该局域网设备（发现表在窗体私有引擎里）。
  bool has_direct_peer(const QString& device_id) const;

  // —— T4.4 截图与标注 ——
  // 截图工具（区域选择→标注→确认即发送；测试缝见 ScreenshotTool 注释）。
  ScreenshotTool* screenshot_tool() { return &screenshot_tool_; }
  void start_screenshot(); // 按钮与 Ctrl+Alt+A 快捷键入口

  // 状态断言面（验收测试）
  bool collab_logged_in() const;
  QString banner_text() const;   // 归档提示条文案（直连／协作两态）
  QString status_text() const;   // 状态栏当前文案（降级提示在此）
  QString chat_html() const;     // 聊天区富文本（合并展示断言）
  // —— T4.3 消息状态与多端（验收面）——
  QString delivery_text() const; // 最近一条发出消息的状态（发送中／已送达／失败／已读）
  QString kick_text() const;     // 最近一次互踢提示（空=本会话未被踢）
  QStringList online_accounts() const; // 最近一次在线账号表（推送即刷新）

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
  void show_org_dialog();        // 菜单入口：查询＋组织架构弹窗（T3.1）
  void build_org_tree(const QString& org_json); // 弹窗内构建部门/成员树
  void apply_policy(const QString& org_json); // 解析本人生效策略（T3.4）
  // 直连发送是否被策略放行：免登录使用／跨态通信两项开关（T3.4）
  bool direct_send_allowed();
  // 截图确认后发送（PNG 临时文件走既有文件通道；与「发文件」同口径）
  void on_screenshot_confirmed(const QString& path);

  // —— T4.2 跨态互通 ——
  // 跨态＝恰一边登录（我已登录而对端未登录，或反之）。跨态会话固定标
  // 「未归档」且不可关闭；已登录端上报会话建立/结束日志（时间/双方/时长）。
  bool is_cross_state(const QString& device_id) const;
  // 会话首触：首次收发即上报 start（幂等，只记第一条）。
  void cross_touch(const QString& device_id);
  // 会话闭环：上报 end 并出表（登出/对端登录/对端离线时调用）。
  void cross_end(const QString& device_id);
  void cross_end_all(); // 登出前全量闭环
  // 发现表变化时巡检跨态表：对端离线或对端已登录 → 该会话闭环（end）。
  void cross_sweep();

  // —— T4.1 群聊 ——
  struct GroupEntry {
    QString name;
    QString owner;
    QString announcement; // 空=未设
    QStringList members;  // 含群主
  };
  // 群列表数据到达（GROUP_DATA）：解析入 groups_ 并刷新列表／当前会话标题
  void apply_groups(const QString& groups_json);
  // 群条目右键菜单：拉人／公告／退群（服务端群）；解散（临时群）
  void show_group_menu(const QPoint& pos);
  // 当前会话为服务端群时返回群号与其条目；否则 0／nullptr
  quint64 current_group_id() const;
  GroupEntry* current_group();
  // 拉人进群／设置群公告弹窗（成员数据源：组织架构成员）
  void group_invite_dialog(quint64 group_id);
  void group_announce_dialog(quint64 group_id);
  // 免服务端临时群（直连态多选设备扇出，不进归档）
  void dgroup_dialog();
  QString dgroup_title(const QString& dgroup_id) const; // 「临时群 N」标题
  QStringList org_accounts() const; // 组织架构全部账号（建群/拉人数据源）

  QString current_peer_;      // 当前会话对端（设备标识或协作账号或群键）
  QString current_kind_{QStringLiteral("direct")}; // 会话形态：direct／collab／group／dgroup
  QSet<QString> collab_peers_; // 协作会话列表（登录后与本地库历史并集）
  bool collab_was_logged_in_{false}; // 上一轮登录态（降级提示去抖）
  QString last_org_json_;     // 最近一次组织架构数据（T3.1）
  bool org_dialog_pending_{false}; // 已请求组织架构、等待弹窗
  bool anonymous_allowed_{true};   // T3.4 生效策略：允许免登录使用（默认宽松）
  bool cross_state_allowed_{true}; // T3.4 生效策略：允许与未登录设备通信
  QMap<QString, quint64> file_sent_; // 文件名 → 最近一次进度字节（节流）
  bool chat_showing_guidance_{false}; // 聊天区当前是否为引导态
  QString status_hint_;               // 状态栏事件提示（引擎态前缀实时拼）

  QString last_groups_json_;          // 最近一次群列表数据（T4.1）
  QMap<quint64, GroupEntry> groups_;  // 群号 → 条目（登录后 query_groups 拉取）
  QHash<QString, QStringList> dgroup_members_; // 临时群 id → 成员设备（本机视图）
  int next_dgroup_{1};                // 临时群序号（标题与 id 用）
  QHash<QString, QString> shot_paths_; // 截图传输 id → PNG 路径（发送完成后清理）

  ScreenshotTool screenshot_tool_;

  // —— T4.3 消息状态与多端 ——
  QString delivery_text_;             // 最近一条发出消息的状态（测试断言面）
  QString kick_text_;                 // 最近一次互踢提示（测试断言面）
  QSet<QString> online_accounts_;      // 在线账号表（服务端推送，含自己）
  void set_delivery_state(const QString& text); // 同步状态面＋状态栏提示

  // T4.2 跨态会话表：设备 id → 会话建立时刻（已上报 start、未闭环）
  QHash<QString, qint64> cross_open_;
  qint64 collab_login_ms_{0}; // 本次协作态登录时刻（A8 归档起点展示）

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
