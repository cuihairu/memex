# 原型符合度台账

> 台账用途：把 `docs/design/prototypes/` 下每一张原型钉到具体的 C++ / Kotlin 界面上，作为下一步 UI 落地的验收基准。
> 验收口径（用户 2026-10-24 令）：**与 `prototypes/xxx.html` 逐屏比对一致（控件 / 布局 / 文案 / 间距）**。

## 状态定义

| 状态 | 含义 |
| --- | --- |
| **已对齐** | C++/Kotlin 侧界面已按原型实现，控件、布局、文案、间距均与原型一致（或差异仅为平台控件外观） |
| **有偏差** | 界面已实现，但与原型存在可见差异：原型含示意数据/参考栏，或真实界面的字段顺序、按钮组、留白与原型不一致 |
| **未实现** | 该原型对应的界面在代码中尚无实现（含桌面与移动端两侧） |

## 主表

| 原型文件 | 对应 C++ / Kotlin 界面文件 | 现状 | 备注 |
| --- | --- | --- | --- |
| `desktop-chat.html` | `client/app/main_window.cpp`（群聊页：`build_group_view` 等） | 已对齐 | 跨态会话与撤回语义；2026-10-29 逐屏复核补齐：头像列/系统胶囊行/文件卡片/日期分割线/输入区说明条（`--warn-soft`→`warning_wash` 令牌）/未归档徽标（afa99fd）。近似项：日期分割线为「── 今天 ──」全宽样式（原型为行内短划）；文件卡片仅会话内存态，不持久化历史 |
| `desktop-collab.html` | `client/app/main_window.cpp`（协作态会话列表）+ `client/app/rail_nav.cpp` | 已对齐 | 登录协作态列表；2026-10-30 逐屏复核补齐：原型会话行（40px 首字头像＋在线点/600 姓名/11px 时间/12px 预览/brand 未读胶囊）、置顶与最近联系分区（群聊并入两区，撤独立「群聊·服务端归档」块）、side-head「+」发起单聊钮（cceec84）。近似项：预览文本取消息库末条（文件传输不入库故只显文本）；行内「已读/归档自」徽标未按行建模；「+」菜单成员未拉到组织架构时回落会话表/在线表 |
| `desktop-direct.html` | `client/app/main_window.cpp`（未登录直连列表态） | 已对齐 | 无对话不摆聊天面板；2026-10-30 逐屏复核补齐：直连行控件化（头像/在线点/「账号·设备」名/时间/末条预览/未读胶囊）、「在线设备」分区、侧栏头「自动发现 本机子网/24 · UDP 2425」注脚、计数绿点、搜索占位「搜索设备 / IP」（d52bfc2）。近似项：「离线设备」分区未渲染（发现服务超时即摘除对端，无离线留存表）；行名「账号 id·设备名」对齐原型「人名·设备名」（直连态无显示名映射）；预览＝本地库末条文本，文件传输不入库故无「[文件] …」形态，无历史走「IP · 端口」元信息兜底；标题栏状态徽章＝原生标题栏（平台外观差异） |
| `desktop-direct-chat.html` | `client/app/main_window.cpp`（`open_direct_peer` / `direct_peer_target`） | 已对齐 | 点好友后展开态；2026-10-30 逐屏复核补齐：会话头 28px 首字头像、标题「账号·设备名」、icon-btn 动作位（文件＝生产发文件路径；更多＝既有会话操作菜单）、系统胶囊行「已通过 UDP 广播发现对方 · 本对话不归档」、输入区「直连 · 无归档」描边胶囊随形态换字（128c3ec）。近似项：语音钮无实现不摆（不造假控件）；更多菜单直聊侧＝振屏/按日期/搜本机记录（原型未列明菜单内容）；头部搜索钮为应用侧真实功能（消息检索）保留 |
| `desktop-friends.html` | `client/app/main_window.cpp`（好友列表，来自组织架构通讯录） | 已对齐 | 好友与组织通讯录同源，不另建原型 |
| `desktop-group.html` | `client/app/main_window.cpp`（群聊页）+ `group_tools_dialog.cpp` | 已对齐 | 群基础面 |
| `desktop-files.html` | `client/app/main_window.cpp`（文件传输面，`/files/*` FileServer 路由） | 已对齐 | 单聊/群文件传输 |
| `desktop-org.html` | `client/app/main_window.cpp:3041`「组织架构（服务端下发）」（`build_org_tree`） | 已对齐 | 全公司唯一通讯录面 |
| `desktop-search.html` | `client/app/message_search_dialog.cpp` | 已对齐 | 归档检索与导出 |
| `main-empty.html` | `client/app/main_window.cpp`（联系人空态） | 已对齐 | 空态参考页 |
| `mobile-chat.html` | `apps/android/.../ui/ChatActivity.kt` + `res/layout/activity_chat.xml` | 已对齐 | 移动端聊天 |
| `mobile-init.html` | `.../ui/InitActivity.kt`、`LoginActivity.kt` + `activity_init.xml`、`activity_login.xml` | 已对齐 | 服务器地址初始化 + 登录 |
| `mobile-me.html` | `.../ui/MeActivity.kt` + `activity_me.xml` | 已对齐 | 移动端「我」 |
| `mobile-sessions.html` | `.../ui/MainActivity.kt` + `activity_main.xml` | 已对齐 | 移动端会话列表 |
| `settings-general.html` | `client/app/theme_settings_page.cpp`、`theme_pack.cpp`、`brand_settings_page.cpp`（推断映射） | 有偏差 | 主题/主题包/品牌；原型含品牌色板示意 |
| `settings-account.html` | `client/app/main_window.cpp:4998`「个人资料 · 个性签名」+ 登录设备管理（推断映射） | 有偏差 | 账号与设备 |
| `settings-message.html` | `client/app/main_window.cpp:1722`「表情」+ `emoji_pack_dialog.cpp`（推断映射） | 有偏差 | 表情/截图偏好 |
| `settings-privacy.html` | `client/app/away_settings.cpp`、`lock_screen.cpp`、`notify_prefs.cpp`（推断映射） | 有偏差 | 离开/锁屏/通知偏好 |
| `settings-advanced.html` | `client/app/net_remote_settings.cpp`、`shortcut_settings.cpp`、`main_window.cpp:3884`「连接设置」（推断映射） | 有偏差 | 网络与远程/快捷键/连接 |
| `desktop-mailbox.html` | ——（`client/` 下无 mailbox 实现） | **未实现** | 设计见 `mailbox.md`；邮箱＝视图+已读态，非新通道 |
| `desktop-tasks.html` | `client/app/task_dialog.cpp`（`task_provider_store.cpp` / `task_provider_settings.cpp` / `task_template_settings.cpp`） | 有偏差 | 原型含日历月历、看板、领导视角三块参考栏，真实弹窗未渲染 |
| `desktop-approval.html` | `client/app/approval_dialog.cpp` | 有偏差 | 四态与权限说明为原型补充栏 |
| `desktop-report.html` | `client/app/report_dialog.cpp` | 有偏差 | 原型含周报补写与下属日报两栏示意 |
| `desktop-group-tools.html` | `client/app/group_tools_dialog.cpp` | 有偏差 | 投票/接龙/群任务三页；原型以并列展示模拟 Tab |
| `desktop-group-memo.html` | `client/app/group_memo_dialog.cpp` + `main_window.cpp:2913`「公告编辑历史」 | 有偏差 | 修订历史与凭据检测提示 |
| `desktop-group-vault.html` | `client/app/group_vault_dialog.cpp`（`group_vault_crypto.cpp`） | 有偏差 | 端到端密文，客户端零凭据 |
| `desktop-group-ci.html` | `client/app/group_ci_dialog.cpp` | 有偏差 | 客户端只见按钮不见凭据 |
| `desktop-group-pack.html` | `client/app/group_pack_dialog.cpp` | 有偏差 | 群配置快照不含消息归档 |
| `desktop-group-servers.html` | `client/app/group_server_dialog.cpp` | 有偏差 | 查看+连接，高危动作走审批 |
| `desktop-group-record.html` | `client/app/main_window.cpp:2747`（`MainWindow::group_record_export_dialog`，入口 `main_window.cpp:2560`「导出群记录…」） | 有偏差 | 分页导出 + AI 降级 + truncated 续传标记；右侧产物结构为导出文档示意 |
| `desktop-assist.html` | `client/app/assist_dialog.cpp` | 有偏差 | 5 态台账 + 审计链 + 屏幕永不落盘 |
| `desktop-office-map.html` | `client/app/office_map_dialog.cpp` | 有偏差 | 自绘 10×10 工位面（真机为 `paintEvent` 自绘，原型以网格近似） |
| `desktop-audit.html` | `client/app/audit_dialog.cpp` | 有偏差 | 检索与查阅日志均须 auditor 角色 |
| `desktop-file-assistant.html` | `client/app/file_assistant.cpp` | 有偏差 | 同内容秒传去重；外部只读 token 下载 403 |
| `mobile-mailbox.html` | ——（`apps/android/` 下无 MailboxActivity） | **未实现** | 移动端邮箱面，与桌面邮箱同设计 |
| `mobile-tasks.html` | `apps/android/.../ui/TasksActivity.kt` + `res/layout/activity_tasks.xml` | 有偏差 | 列表 + 横滑看板（列宽 160dp，<600dp 自动降级） |

**统计**：原型 36 张（桌面 23 / 移动 4 / 设置 5 / 空态参考 1 / 管理台 1）；已对齐 14、有偏差 20、未实现 2。

## 留痕铁律（所有涉及归档的界面统一适用）

1. **清空 / 标记已读只动视图状态**：归档行、公告行、告警台账行一概不动。
2. **每次查看 / 复制 / 明文呈现均落服务端审计行**：密码箱 view/copy、邮箱确认已读、群记录导出、消息检索与查阅日志、远程协助状态迁移、群服务器访问。
3. **被拒请求同样留痕**：审计检索（`audit.message.search`）、外部 token 下载、非成员群操作（幽灵 gid 统一 403 不泄露存在性）。
4. **不静默截断**：导出达上限必须在产物内标注 `truncated` 并给出续传入口（`after_id`）。

## 未入型清单（有界面、无原型）

以下界面无对应原型，属台账未覆盖区，后续按需补：

- **主窗口内联面**：盯列表（`main_window.cpp:3424`「盯列表」）、表情面板（`main_window.cpp:1722`「表情」）、截图工具（`screenshot_tool.cpp` / `crop_dialog.cpp`）、按日期查看聊天记录（`main_window.cpp:4654`）、关于（`main_window.cpp:3472`）。
- **群相关补面**：新建群聊（`:2340`）、拉人进群（`:2663`）、群资料（`:2723`）、新建临时群（点对点扇出，不进归档）（`:2970`）、群公告置顶区与 @全员强提醒。
- **设置子页**：离开与锁屏（`away_settings.cpp` / `lock_screen.cpp`）、通知偏好（`notify_prefs.cpp`）、网络与远程（`net_remote_settings.cpp`）、快捷键（`shortcut_settings.cpp`）、主题/主题包/品牌（`theme_settings_page.cpp` / `theme_pack.cpp` / `brand_settings_page.cpp`）、连接设置（`:3884`）、个人资料与个性签名（`:4998`）、外部任务模板与任务提供方设置（`task_template_settings.cpp` / `task_provider_settings.cpp`）、云表情包（`emoji_pack_dialog.cpp`）。
- **移动端补面**：文件助手（`FileAssistantActivity.kt` + `activity_file_assistant.xml`，桌面已有原型）、移动审批 / 日报 / 群工具 / 邮箱。
- **登录与恢复**：OIDC 登录页（`LoginActivity.kt`，桌面侧为品牌套件 `brand_kit.cpp` 渲染）、崩溃报告（`crash_report.cpp`）。
