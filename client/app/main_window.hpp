// Memex 主窗口（T1.4 直连态界面 + T2.4 模式切换与降级）：
// 局域网设备列表（替代联系人）+ 聊天窗 + 「未归档」本地态标记 + 空态引导。
// 协作态经菜单登录／登出（不重启切换形态）；双态共用同一份本地库，
// 历史按 source 字段合并展示；服务端不可达回落直连态并常驻提示「消息不进归档」。
#pragma once

#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTextBrowser>
#include <QVector>

#include <engine/direct/direct_engine.hpp>
#include <engine/collab/collab_engine.hpp>

#include <app/screenshot_tool.hpp>

class QAction;
class QCloseEvent;
class QSystemTrayIcon;

namespace memex::client {

class FileAssistantDialog;
class GroupMemoDialog;
class GroupVaultDialog;
class GroupCiDialog;
class GroupPackDialog;
class GroupServerDialog;
class TaskDialog;
class ApprovalDialog;
class ReportDialog;
class AuditDialog;
class OfficeMapDialog;
class AssistDialog;
class GroupToolsDialog;

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
  // 最近一次常用联系人数据（T4.5；星标置顶＋最近排序，服务端换机保留）
  QString fav_json() const;
  // 当前会话的星标切换（服务端常用联系人；未登录协作态时提示）
  void toggle_current_fav_star();
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
  void show_emoji_panel(); // T4.5：表情面板（内置按频次＋自定义导入）
  // 自定义表情导入（测试缝：面板「导入」按钮即此函数；同名覆盖、失败回 false）
  bool import_emoji(const QString& src);
  static QString emoji_dir(); // 表情目录（MEMEX_TEST_EMOJI_DIR 可覆盖，测后无污染）

  // 状态断言面（验收测试）
  bool collab_logged_in() const;
  QString banner_text() const;   // 归档提示条文案（直连／协作两态）
  QString status_text() const;   // 状态栏当前文案（降级提示在此）
  QString chat_html() const;     // 聊天区富文本（合并展示断言）
  // —— 平台-7 降级显式化（验收面）——
  QString input_hint() const;    // 输入框提示（降级态明示「不进归档」）
  QString chat_meta() const;     // 会话元信息行（降级态「已降级 · 未归档」前缀）
  int alert_count() const;       // 新消息闪烁累计次数（合并窗内只记 1）
  // —— T4.3 消息状态与多端（验收面）——
  QString delivery_text() const; // 最近一条发出消息的状态（发送中／已送达／失败／已读）
  QString kick_text() const;     // 最近一次互踢提示（空=本会话未被踢）
  QStringList online_accounts() const; // 最近一次在线账号表（推送即刷新）
  // —— T4.7 系统集成（验收面）——
  QString last_notify() const; // 最近一条系统通知（标题＋正文；托盘不可用也记录）
  bool tray_available() const; // 托盘是否可用（offscreen 等环境为假）
  bool autostart_enabled() const; // 开机启动是否已登记
  void set_autostart(bool on);    // 登记／撤销开机启动（freedesktop .desktop）
  // —— BUG-004 托盘激活（验收面）——
  // 单击/双击托盘 → 激活主界面（右键 Context 留给菜单，不接线）；
  // 测试用裸 QSystemTrayIcon 复用生产接线（发 activated 信号即走同一路径）。
  void wire_tray_activation(QSystemTrayIcon* tray);
  void activate_from_tray(); // 隐藏→弹出、最小化→还原（保留最大化），随后置顶聚焦

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
  void render_guidance();  // 空态引导正文（颜色走令牌，主题切换可重渲）
  // —— T4.9 主题（R19）——
  // 控件样式统一出口：所有颜色只从 ThemeTokens 取，切换后重刷即生效。
  void apply_theme_styles();
  // 聊天区富文本重渲：气泡/@高亮/系统行的颜色是内联的，随主题重放记录。
  void rerender_chat();
  void show_theme_settings();  // 「设置 → 主题…」入口
  // 聊天区一行（消息或系统行）的结构化记录——主题切换时据此重渲，
  // 不必重查本地库（重查会丢掉尚未落库的即时提示）。
  struct ChatRow {
    bool system{false}; // true=系统行（text 即正文）；false=消息气泡
    QString name;       // 气泡行：显示名＋来源标签（已 esc）
    QString text;
    qint64 ts_ms{0};
    bool outgoing{false};
    bool at_mode{false}; // 群聊：@账号 高亮
  };
  void show_status(const QString& text);
  // 新消息闪烁提醒（用户令 2026-10-05）：仅窗口非激活时；同窗多条合并；
  // 设置「通知偏好→新消息闪烁提醒」可关（默认开）。
  void alert_attention();
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
  void apply_favs(const QString& fav_json); // 常用联系人刷新（T4.5）
  // 群条目右键菜单：拉人／公告／退群（服务端群）；解散（临时群）
  void show_group_menu(const QPoint& pos);
  // 当前会话为服务端群时返回群号与其条目；否则 0／nullptr
  quint64 current_group_id() const;
  GroupEntry* current_group();
  // 拉人进群／设置群公告弹窗（成员数据源：组织架构成员）
  void group_invite_dialog(quint64 group_id);
  void group_announce_dialog(quint64 group_id);
  // 公告编辑历史查看（R24-1）：请求→等专用回执（超时兜底）→列表展示
  void group_announce_history_dialog(quint64 group_id);
  // 免服务端临时群（直连态多选设备扇出，不进归档）
  void dgroup_dialog();
  QString dgroup_title(const QString& dgroup_id) const; // 「临时群 N」标题
  QStringList org_accounts() const; // 组织架构全部账号（建群/拉人数据源）

  QString current_peer_;      // 当前会话对端（设备标识或协作账号或群键）
  QString current_kind_{QStringLiteral("direct")}; // 会话形态：direct／collab／group／dgroup
  QSet<QString> collab_peers_; // 协作会话列表（登录后与本地库历史并集）
  bool collab_was_logged_in_{false}; // 上一轮登录态（降级提示去抖）
  // 平台-7 DEGRADED：本应归档但服务端不可达（断线／掉线后登录失败）。
  // 独立于 collab_was_logged_in_（后者被 connection_lost 即刻清零，
  // 随后的登录失败就无从判「曾归档」）；仅登录恢复／显式登出清零。
  bool collab_degraded_{false};
  QString last_org_json_;     // 最近一次组织架构数据（T3.1）
  bool org_dialog_pending_{false}; // 已请求组织架构、等待弹窗
  bool anonymous_allowed_{true};   // T3.4 生效策略：允许免登录使用（默认宽松）
  bool cross_state_allowed_{true}; // T3.4 生效策略：允许与未登录设备通信
  QMap<QString, quint64> file_sent_; // 文件名 → 最近一次进度字节（节流）
  bool chat_showing_guidance_{false}; // 聊天区当前是否为引导态
  QVector<ChatRow> chat_rows_;       // 当前会话的聊天行记录（重渲用）
  QString status_hint_;               // 状态栏事件提示（引擎态前缀实时拼）
  bool alert_active_{false};          // 闪烁合并窗进行中（窗内消息不叠加）
  int alert_count_{0};                // 实际触发的闪烁次数（验收断言面）

  QString last_groups_json_;          // 最近一次群列表数据（T4.1）
  QString last_fav_json_;             // 最近一次常用联系人（T4.5）
  bool fav_is_starred_(const QString& peer) const; // 星标查询（解析 last_fav_json_）
  qint64 fav_last_ms_(const QString& peer) const;  // 最近联系时刻（同上）
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

  // —— T4.7 系统集成 ——
  // 托盘：关闭进托盘（可用时）、托盘菜单（显示／开机启动／退出）；
  // 系统通知：窗口未激活时的新消息与互踢提示走托盘气泡（不可用仅记录）；
  // 开机启动：freedesktop autostart .desktop 登记（UOS/麒麟同为 Linux 生效）。
  void setup_tray(); // 托盘可用才建（offscreen 等环境跳过，不崩）
  void tray_notify(const QString& title, const QString& text);
  void show_about(); // A23 图标面：关于页（logo＋版本）
  void closeEvent(QCloseEvent* event) override; // 托盘可用时关闭即最小化进托盘
  static QString autostart_dir(); // 登记目录（MEMEX_TEST_AUTOSTART_DIR 可覆盖，测后无污染）
  QSystemTrayIcon* tray_{nullptr}; // 托盘不可用时保持空（全部调用判空）
  QAction* act_autostart_{nullptr}; // 开机启动菜单项（与登记态同步勾选）
  QString last_notify_;             // 最近一条系统通知（测试断言面）

  // T4.2 跨态会话表：设备 id → 会话建立时刻（已上报 start、未闭环）
  QHash<QString, qint64> cross_open_;
  qint64 collab_login_ms_{0}; // 本次协作态登录时刻（A8 归档起点展示）

  DirectEngine direct_engine_;
  CollabEngine collab_engine_;
  QPointer<FileAssistantDialog> file_assistant_; // R23-3 文件助手窗口（懒建）
  QPointer<GroupMemoDialog> group_memo_; // R24-2 群备忘录窗口（懒建）
  QPointer<GroupVaultDialog> group_vault_; // R24-3 群密码箱窗口（懒建）
  QPointer<GroupCiDialog> group_ci_; // R25-2 群 CI/CD 窗口（懒建）
  QPointer<GroupPackDialog> group_pack_; // R25-3 打包/导出窗口（懒建）
  QPointer<GroupServerDialog> group_server_; // R26-2 群服务器面板窗口（懒建）
  QPointer<TaskDialog> task_dialog_; // R27-1 任务清单窗口（懒建）
  QPointer<ApprovalDialog> approval_dialog_; // 二期·审批窗口（懒建）
  QPointer<ReportDialog> report_dialog_; // 二期·日报周报窗口（懒建）
  QPointer<AuditDialog> audit_dialog_; // 二期·会话审计窗口（懒建）
  QPointer<OfficeMapDialog> office_dialog_; // 二期·办公室位置图窗口（懒建）
  QPointer<AssistDialog> assist_dialog_; // 二期·远程协助窗口（懒建）
  QPointer<GroupToolsDialog> group_tools_dialog_; // 二期·群工具三件窗口（懒建）

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
  QAction* act_theme_{nullptr}; // 「设置 → 主题…」菜单项

  // 参与 apply_theme_styles 的控件（构造期为局部变量，主题重刷需长期持有）
  QWidget* side_{nullptr};
  QLabel* side_title_{nullptr};
  QLabel* local_badge_{nullptr};
  QWidget* head_{nullptr};
  QWidget* input_row_{nullptr};
  QPushButton* file_btn_{nullptr};
  QPushButton* shot_btn_{nullptr};
  QPushButton* emoji_btn_{nullptr};
};

} // namespace memex::client
