// Memex 主窗口（T1.4 直连态界面 + T2.4 模式切换与降级）：
// 局域网设备列表（替代联系人）+ 聊天窗 + 「未归档」本地态标记 + 空态引导。
// 协作态经菜单登录／登出（不重启切换形态）；双态共用同一份本地库，
// 历史按 source 字段合并展示；服务端不可达回落直连态并常驻提示「消息不进归档」。
#pragma once

#include <QDate>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QKeySequence>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTextBrowser>
#include <QVector>

#include <engine/direct/direct_engine.hpp>
#include <engine/collab/collab_engine.hpp>

#include <app/screenshot_tool.hpp>
#include <app/notify_prefs.hpp>
#include <app/lock_screen.hpp>

class QAction;
class QCloseEvent;
class QShortcut;
class QTimer;
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
class BrandSettingsDialog;
class ShortcutSettingsDialog;
class AwayLockSettingsDialog;
class NetRemoteSettingsDialog;
class FilesClient;

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
  void start_screenshot(); // 按钮与截图快捷键入口（键可改，见下）
  // 截图快捷键当前值（QSettings shortcuts/screenshot；默认 Ctrl+Alt+A）
  static QString screenshot_shortcut();
  // 改键（设置页与测试共用）：冲突检测（应用内其他 QShortcut/QAction
  // 键序列）→无冲突＝重绑＋落盘，返回空串；有冲突＝返回冲突文案不改现状。
  QString apply_screenshot_shortcut(const QKeySequence& seq);
  // —— 离开锁屏（用户令 2026-10-08 ④；验收面与设置页共用）——
  // 重读 away_lock 设置：重启无操作计时器（设置对话框关闭即调＝即时生效）
  void apply_away_lock_settings();
  // 重读 net_blacklist/remote_control：黑名单开关+段表转过滤器注入直连
  // 引擎（运行中改即按新表判；开关关=不装过滤器全放行）
  void apply_net_settings();
  // 截图发送流（需求批④，与截图确认回调同一路径；测试缝）：持久拷贝＋
  // 走直连文件通道＋本端图片气泡；返回 false＝未发出（状态行明示原因）
  bool send_shot_to_current_chat(const QString& path);
  // 文件夹发送流（需求批⑤，与「发文件夹」按钮同一路径；测试缝）：递归
  // 遍历走直连文件通道＋作业聚合状态。返回作业 id，空＝未发出（守卫拦/
  // 目录空/对端不可达，状态行明示原因）
  QString send_folder_to_current_chat(const QString& dir);
  // 文件接收验收面（需求批④调试缝）：「from|path」（未收＝空）
  QString last_received_file() const { return last_received_file_; }
  // 最近一次文件传输终态错误（授权链修复·验收面）：空=无失败；含
  // deny:server-unreachable＝未登录 fail-closed 本地拒
  QString last_file_error() const { return last_file_error_; }
  // 振屏发送流（需求批⑥，与「振屏」按钮同一路径；测试缝）：守卫（无
  // 会话/群会话）＋防刷限频（同一会话 10s 冷却）。true＝已发出
  bool send_nudge_to_current_chat();
  // 振屏接收验收面（需求批⑥）：窗口抖动生效次数（接收侧效果限频内计 1）
  int shake_count() const { return shake_count_; }
  // 布局验收面：会话面板当前是否展开（列表态＝false）
  bool chat_panel_visible() const {
    return chat_panel_ && chat_panel_->isVisible();
  }
  bool lock_screen_visible() const; // 锁屏遮罩当前是否在屏
  int lock_unread_count() const;    // 锁屏期间新到消息条数（未锁＝0）
  QString lock_screen_text() const; // 锁屏面未读行文案（断言只含数量不含内容）
  void lock_try_unlock(const QString& pwd); // 解锁（设置对话框/测试同一路径）
  void show_emoji_panel(); // T4.5：表情面板（内置按频次＋自定义导入＋图标组）
  // 自定义表情导入（测试缝：面板「导入」按钮即此函数；同名覆盖、失败回 false）
  bool import_emoji(const QString& src);
  static QString emoji_dir(); // 表情目录（MEMEX_TEST_EMOJI_DIR 可覆盖，测后无污染）
  // 需求批①文字颜色：请求＝读选区弹 QColorDialog（取消无动作）；
  // apply＝把选区包上受控颜色标记〔#RRGGBB〕…〔/〕（带参＝测试直调不弹框）
  void request_input_color();
  void apply_input_color(const QColor& color);

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
  // —— 需求批①（验收面）——
  // 注入一条消息走气泡渲染（测试缝：生产路径＝引擎信号驱动 append_message，
  // 纯 UI 测试无对端时经此面断言 colorize 渲染；msg_id/receipt＝需求批⑦
  // 回执标注验收用）
  void inject_message(const QString& from_id, const QString& text,
                      bool outgoing, const QString& msg_id = QString(),
                      const QString& receipt = QString(),
                      qint64 ts_ms = 0); // 0=现在（分日分隔线腿注昨日时戳）
  // 注入一条图片消息走气泡渲染（需求批②验收缝：右键「收藏到表情包」
  // 需图片气泡在场；生产路径＝引擎文件回执驱动 append_image_message）
  void inject_image(const QString& from_id, const QString& image_path,
                    bool outgoing);
  // 聊天区控件（验收缝：右键菜单/cursorRect 定位）
  QTextBrowser* chat_widget() { return chat_view_; }
  // —— 需求批⑦ 消息回执（验收面）——
  // 已读回执上报开关（会话级覆盖全局；全局默认开，QSettings 持久化）
  bool read_receipts_enabled(const QString& peer) const;
  void set_read_receipts_enabled(const QString& peer, bool enabled); // 空 peer=全局
  // 回执态落行重渲（测试缝：生产路径＝引擎 message_delivered/
  // message_read/receipts_received 信号驱动同款；state=delivered/read）
  void apply_message_receipt(const QString& msg_id, const QString& state);
  // —— 需求批⑧ 聊天记录按日期（验收面与「按日期」对话框同一路径）——
  // 范围筛选（毫秒含端点）：本地索引重建当前会话行；协作/群登录态同时向
  // 服务端补拉该窗（回包落本地索引后自动重渲）。
  void apply_date_filter(qint64 from_ms, qint64 until_ms);
  // 清除筛选恢复全量（幂等：无筛选即无动作）。
  void clear_date_filter();
  // 跳转到目标日首条消息（锚点滚动；缺数据的日期先向服务端补拉，到达后
  // 自动重渲再定位）。
  void jump_to_date(const QDate& date);
  // —— 需求批⑪ 个性签名（验收面）——
  // 直接发送签名设置（带参＝测试直调不弹框；须登录协作态，回执异步）
  void apply_signature(const QString& signature);
  // 本人当前签名（org 数据缓存；空=未设/未登录）
  QString own_signature() const;
  // —— 需求批⑩ 在线时长（验收面）——
  // 本人在线时长展示串（org 数据缓存；空=未登录/数据未到）
  QString own_online() const;
  // —— 需求批⑫ 头像（验收面）——
  // 本人头像版本戳（org 数据缓存；0=未设置——展示默认头像，按账号 hash 取）
  qint64 own_avatar_ver() const;
  // —— BUG-004 托盘激活（验收面）——
  // 单击/双击托盘 → 激活主界面（右键 Context 留给菜单，不接线）；
  // 测试用裸 QSystemTrayIcon 复用生产接线（发 activated 信号即走同一路径）。
  void wire_tray_activation(QSystemTrayIcon* tray);
  void activate_from_tray(); // 隐藏→弹出、最小化→还原（保留最大化），随后置顶聚焦

private:
  // 离开锁屏（用户令 2026-10-08 ④）：全局输入喂计时器＋超时上锁
  bool eventFilter(QObject* watched, QEvent* event) override;
  void maybe_lock_screen();
  void build_ui();
  void wire_engines();
  void wire_collab(); // 协作信号接入界面（T2.4）
  void refresh_devices();
  void open_peer(const QString& device_id);
  void open_chat(const QString& kind, const QString& id);
  // 关会话回列表态（布局令：无对话＝「左菜单+好友列表」长方形面板，
  // 对话面板只在 open_chat 后展开；再点当前好友／点标题栏 ✕ 同此路）
  void close_chat();
  void append_message(const QString& from_id, const QString& text,
                      qint64 ts_ms, bool outgoing, const QString& source,
                      const QString& msg_id = QString(),
                      const QString& receipt = QString());
  // 图片消息气泡（需求批④）：气泡内直接渲染图片（<img>，非文件系统行）
  void append_image_message(const QString& from_id, const QString& image_path,
                            qint64 ts_ms, bool outgoing, const QString& source);
  QString direct_peer_target() const; // 直连文件通道目标设备（collab 按账号匹配）
  void append_system_line(const QString& text);
  void show_guidance();
  void render_guidance();  // 空态引导正文（颜色走令牌，主题切换可重渲）
  // —— T4.9 主题（R19）——
  // 控件样式统一出口：所有颜色只从 ThemeTokens 取，切换后重刷即生效。
  void apply_theme_styles();
  // 聊天区富文本重渲：气泡/@高亮/系统行的颜色是内联的，随主题重放记录。
  void rerender_chat();
  // —— 需求批⑧ 聊天记录按日期 ——
  // 「按日期」对话框：日历选择起始/结束日，跳转/筛选/清除三动作。
  void show_date_dialog();
  // 会话历史渲染（open_chat 尾段抽取，跳转/清筛选复用）：跨态标记→空历史
  // 引导→双态合并行→已读上报（⑦开关裁决）→回执态补查（⑦）。
  void render_open_history();
  // 筛选态渲染：本地索引按时间窗重建当前会话行（头部注明范围）。
  void render_filtered_history();
  // 滚动定位到目标日首条消息行（锚点＝本机日期 yyyyMMdd）。
  void scroll_to_day(const QDate& date);
  void show_theme_settings();  // 「设置 → 主题…」入口
  // 「设置 → 个人资料…」（需求批⑪）：签名编辑对话框（登录门；保存走
  // apply_signature，回执后重拉 org 即时刷新悬浮）
  void show_profile_dialog();
  // —— 需求批⑫ 头像：文件面接线（懒建 FilesClient；登录复用协作口令）——
  FilesClient* files_client();  // 懒建＋信号接线（头像域；回执/失败统一入口）
  bool files_ensure_login();    // 未登录则发起登录（异步；logged_in 后自动续）
  void change_avatar();         // 「更换头像…」入口（登录门＋文件面门）
  void pick_and_upload_avatar(); // 选图→QPixmap 门→裁剪→四档编码→排队上传
  void start_next_avatar_upload(); // 串行传队列；末档完成即落本机缓存＋重拉 org
  void queue_avatar_download(const QString& account, qint64 ver);
  void on_avatar_fetched(const QString& account, int size,
                         const QByteArray& bytes);
  void on_avatar_uploaded(int size, qint64 ver);
  // 聊天区一行（消息或系统行）的结构化记录——主题切换时据此重渲，
  // 不必重查本地库（重查会丢掉尚未落库的即时提示）。
  struct ChatRow {
    bool system{false}; // true=系统行（text 即正文）；false=消息气泡
    QString name;       // 气泡行：显示名＋来源标签（已 esc）
    QString text;
    qint64 ts_ms{0};
    bool outgoing{false};
    bool at_mode{false}; // 群聊：@账号 高亮
    QString image;      // 非空＝图片消息（需求批④：text 存文件路径，气泡渲染 <img>）
    QString msg_id;     // 协作态消息标识（回执通知按它命中本行；直连恒空）
    QString receipt;    // 我发出消息的回执态（''/delivered/read，需求批⑦）
  };
  // 分日分隔线判定（需求批⑧余量收口）：ts_ms 行前是否需先插日头——
  // chat_rows_ 倒扫最近一条消息行（系统行不参与日序），日不同（或尚无
  // 消息行）即为该日首条。状态即 chat_rows_ 本身：clear 与主题重放
  //（分隔线作为系统行入列）天然同步，无需额外跟踪成员。
  static bool needs_day_divider(const QVector<ChatRow>& rows, qint64 ts_ms);
  void show_status(const QString& text);
  // 新消息闪烁提醒（用户令 2026-10-05）：仅窗口非激活时；同窗多条合并；
  // 设置「通知偏好→新消息闪烁提醒」可关（默认开）。
  void alert_attention();
  // 事件通知统一出口（用户令 2026-10-08）：弹窗受事件开关裁决，提示音
  // 三档独立裁决（声音单列＝弹窗关了声音仍可按档播；beep 兜底无
  // Multimedia 依赖，真音频另批）。
  void event_notify(bool enabled, SoundEvent ev, const QString& title,
                    const QString& text);
  void update_banner();          // 按当前形态切换归档提示条
  void seed_collab_peers();      // 登录后从本地库补入历史协作会话
  void show_collab_login_dialog();
  void show_org_dialog();        // 菜单入口：查询＋组织架构弹窗（T3.1）
  void build_org_tree(const QString& org_json); // 弹窗内构建部门/成员树
  void apply_policy(const QString& org_json); // 解析本人生效策略（T3.4）
  // 直连发送是否被策略放行：免登录使用／跨态通信两项开关（T3.4）
  bool direct_send_allowed();
  // 文件夹发送守卫（需求批⑤，按钮与发送缝单源）：无会话/群会话/策略闸门；
  // false＝已给状态文案
  bool folder_send_allowed();
  // 振屏到达（需求批⑥，直连/协作两域单源）：当前会话渲染 "[振屏]" 系统行
  // ＋窗口抖动与提示音（效果限频）；不在当前会话只提示（历史重载可见）
  void on_nudge_received(const QString& from, qint64 ts_ms,
                         const QString& source);
  // 窗口抖动（需求批⑥）：短促左右移位后复位（离屏/测试环境同样生效）
  void shake_window();
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
  // 成员签名缓存（需求批⑪）：账号 → 个性签名（org 数据到达即刷新；
  // 会话列表悬浮 tooltip 数据源，空=未设）
  QHash<QString, QString> member_signatures_;
  // 成员在线时长缓存（需求批⑩）：账号 → 已格式化展示串（滚动 24h/7d/30d
  // 并集时长，org 数据到达即刷新；会话列表 tooltip 与个人资料对话框共用）
  QHash<QString, QString> member_online_;
  // 成员头像版本戳缓存（需求批⑫）：账号 → org 下发的 avatar_ver（0=未设置，
  // 展示按账号 hash 取内置默认头像）
  QHash<QString, qint64> member_avatar_ver_;
  // 已取到字节并落缩放缓存的版本戳（404 也记＝未设置免反复拉；ver 变即失效）
  QHash<QString, qint64> avatar_fetched_ver_;
  // 在途下载：账号 → 发起时的版本戳（回包对账，防旧包盖新缓存）
  QHash<QString, qint64> avatar_fetching_ver_;
  FilesClient* files_client_{nullptr};    // 文件面（头像域专用；懒建）
  bool avatar_pick_pending_{false};       // 文件面登录在途时挂起的选图续流程
  QSet<QString> avatar_pending_accounts_; // 未登录时挂起的头像下载（登录后冲账）
  QVector<QPair<int, QByteArray>> avatar_upload_queue_; // 待传四档（256→32）
  QVector<QPixmap> avatar_upload_pixmaps_; // 同序档位图（传完落本机缓存先见）
  int avatar_uploaded_count_{0};          // 已成功档数（状态行进度）
  qint64 avatar_new_ver_{0};              // 服务端最新版本戳（上传回包带）
  QString collab_host_;                   // 协作登录主机（文件面同源默认）
  bool org_dialog_pending_{false}; // 已请求组织架构、等待弹窗
  bool anonymous_allowed_{true};   // T3.4 生效策略：允许免登录使用（默认宽松）
  bool cross_state_allowed_{true}; // T3.4 生效策略：允许与未登录设备通信
  QMap<QString, quint64> file_sent_; // 文件名 → 最近一次进度字节（节流）
  bool chat_showing_guidance_{false}; // 聊天区当前是否为引导态
  QVector<ChatRow> chat_rows_;       // 当前会话的聊天行记录（重渲用）
  // 需求批⑧ 按日期查看：筛选窗（毫秒含端点，0=未筛选）与挂起跳转日
  //（服务端补拉到达后自动重渲再定位；换会话即复位）
  qint64 filter_from_ms_{0};
  qint64 filter_until_ms_{0};
  QDate pending_jump_;
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
  // 文件夹作业状态（需求批⑤）：job_id → {显示名, 已发完数, 总文件数}
  //（directory_progress 刷新计数，directory_finished 出表）
  struct FolderJobState {
    QString name;
    quint64 done{0};
    quint64 total{0};
  };
  QHash<QString, FolderJobState> folder_jobs_;

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
  QPointer<BrandSettingsDialog> brand_settings_dialog_; // 二期·品牌物料设置页（懒建）
  QPointer<ShortcutSettingsDialog> shortcut_dialog_; // 二期·快捷键设置页（懒建）
  QPointer<AwayLockSettingsDialog> away_dialog_; // 二期·离开锁屏设置页（懒建）
  QPointer<NetRemoteSettingsDialog> net_dialog_; // 二期·网络与远程设置页（懒建）

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
  QString last_received_file_; // 文件接收验收面（「from|path」）
  QString last_file_error_;    // 最近一次文件传输终态错误（验收面；空=无失败）
  // 振屏（需求批⑥）：发送冷却（peer→上次发送时刻）＋接收效果限频＋
  // 抖动生效计数（测试缝）
  QHash<QString, qint64> last_nudge_sent_;
  qint64 last_nudge_effect_ms_{0};
  int shake_count_{0};
  QShortcut* shot_sc_{nullptr}; // 截图快捷键（键可改：apply_screenshot_shortcut）
  // 离开锁屏（用户令 2026-10-08 ④）：无操作计时器＋锁屏遮罩（只显未读数）
  QTimer* idle_timer_{nullptr};
  LockScreenDialog* lock_{nullptr};
  QAction* act_theme_{nullptr}; // 「设置 → 主题…」菜单项

  // 参与 apply_theme_styles 的控件（构造期为局部变量，主题重刷需长期持有）
  QWidget* side_{nullptr};
  QWidget* chat_panel_{nullptr}; // 会话面板（布局令：空态隐藏＝列表态长方形）
  QLabel* side_title_{nullptr};
  QLabel* local_badge_{nullptr};
  QWidget* head_{nullptr};
  QWidget* input_row_{nullptr};
  QPushButton* file_btn_{nullptr};
  QPushButton* folder_btn_{nullptr};
  QPushButton* shot_btn_{nullptr};
  QPushButton* emoji_btn_{nullptr};
  QPushButton* color_btn_{nullptr};
  QPushButton* nudge_btn_{nullptr};
  QPushButton* date_btn_{nullptr}; // 「按日期」（需求批⑧：跳转/筛选入口）

  // 品牌物料（设计稿 docs/design/品牌物料.md）：侧栏品牌行（无牌隐藏=
  // 没配就不变）＋brand_applied 驱动的整窗换牌（标题/图标/托盘）
  QWidget* brand_row_{nullptr};
  QLabel* brand_logo_{nullptr};
  QLabel* brand_name_{nullptr};
  void apply_brand();
};

} // namespace memex::client
