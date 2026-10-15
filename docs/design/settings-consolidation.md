# 设置页收口：单一设置入口与配置源收敛（设计 2026-10-15）

> 状态：**设计稿**（2026-10-15 用户令「先出设计，本轮不实现」）。范围：桌面端
> 设置收口（单一设置页）＋服务器地址配置源收敛＋迁移兼容。六项功能盘点如实
> 登记见 todo「企业内网办公 IM 缺口盘点」§五（2026-10-15 核实）。

## 0. 现状盘点（实读事实，先核对再设计）

### 0.1 设置入口现状

设置菜单（`client/app/main_window.cpp:588-665`）共 9 项、6 个独立弹窗，
互不相干：个人资料…／通知偏好…／发送已读回执（勾选）／主题…／品牌物料…／
连接设置…／快捷键…／离开与锁屏…／网络与远程…／开机启动（勾选）；任务提供方
设置藏在任务窗内（`client/app/task_dialog.cpp:454`）。rail 齿轮弹同一菜单
（`main_window.cpp:590`）。

### 0.2 服务器地址「是否真的多处」——核实结论

**落盘只有一份，填写 UI 散在 14 处。**

- **存储单源（已是收敛的）**：`QSettings("memex","collab")` 的
  `host/port/files_port/account` 四键。唯一写点＝登录协作态对话框
  （`main_window.cpp:3867-3871`，入口「登录协作态…」`main_window.cpp:468`）。
  唯一例外：审批对话框会回写 `files_port`（`client/app/approval_dialog.cpp:190`）
  ——第二写点，收敛时拔掉。
- **默认值单源（已是收敛的）**：`client/app/collab_defaults.hpp`
  （`default_collab_host()` :15、`default_collab_port()` :19、
  `default_files_port()` :24、`apply_host_rules()` :34、`resolve_conn_host()` :51）。
- **填写 UI 分散（要收口的）**：除登录对话框外，13 个工具对话框各自带
  「服务器地址」填框，打开时读 `settings.value("host", default_collab_host())`
  回填、可改、改完只对该会话生效不落盘：`task_dialog.cpp:161`、
  `brand_settings_page.cpp:91`、`assist_dialog.cpp:247`、
  `office_map_dialog.cpp:229`、`report_dialog.cpp:66`、`approval_dialog.cpp:93`、
  `group_tools_dialog.cpp:179`、`emoji_pack_dialog.cpp`、
  `group_vault_dialog.cpp`、`group_memo_dialog.cpp`、`group_server_dialog.cpp`、
  `group_pack_dialog.cpp`、`group_ci_dialog.cpp`、`file_assistant.cpp`、
  `audit_dialog.cpp`。
- **「连接设置…」不管地址**：该弹窗只编辑 `host_overrides` 域名解析覆盖
  （`main_window.cpp:3886-3920`），没有服务器地址字段——用户从菜单找不到
  地址填写点，这是「分散」观感的根源：改地址必须走语义不明的「登录协作态…」。

### 0.3 配置键全景（全端只有两个 QSettings 库）

| 配置域 | 库（org/app） | 键 | 当前编辑入口 |
| --- | --- | --- | --- |
| 连接地址 | memex/collab | host, port, files_port, account | 登录协作态对话框（唯一写点） |
| 域名解析覆盖 | memex/collab | host_overrides | 连接设置… |
| 已读回执 | memex/Memex（默认库） | receipts/send_read, receipts/peer/\<peer\> | 设置菜单勾选＋会话右键（`main_window.cpp:3170-3191`） |
| 盯提醒 | memex/Memex | watch/remind_minutes, watch/expire_hours | **无 UI**（只能手改 INI，`main_window.cpp:206-219`） |
| 截图快捷键 | memex/Memex | shortcuts/screenshot | 快捷键…（`main_window.cpp:2038`） |
| 主题 | memex/Memex | appearance/theme_mode | 主题…（`theme.cpp:48,404`） |
| 通知偏好 | memex/Memex | notify/* | 通知偏好…（NotificationCenter::show_settings） |
| 离开锁屏 | memex/Memex | away_lock/* | 离开与锁屏…（lock_screen.hpp:16） |
| 网段黑名单/远程控制 | memex/Memex | net_blacklist/*, remote_control/* | 网络与远程…（net_guard.hpp:38-44） |
| 任务提供方凭据 | TaskProviderStore 加密落盘 | — | 任务窗内按钮（task_dialog.cpp:454） |
| 开机启动 | autostart .desktop 文件 | — | 设置菜单勾选（`main_window.cpp:3704`） |
| 品牌物料 | 服务端档案（本机不落盘） | — | 品牌物料… |

### 0.4 R17 初始化向导现状（与「连接」同源的基线）

- 移动端已实现：鸿蒙 `InitPage.ets`（PING→PONG 连通校验通过才落盘，
  preferences 库 `memex_init`，`AppState.ets:57,68-75`）；Android
  `InitActivity.kt`＋`PrefsInitStore.kt`（键 `server_host`/`server_port`，
  `PrefsInitStore.kt:29-30`）。
- 桌面端**无**强制首启向导（直连态可免协作使用，R1 口径）；地址改写唯一
  入口＝登录协作态对话框。
- 已有同源基线：todo:452「客户端默认服务器地址……首次启动向导与默认配置
  同源」（默认值 `server.memex.cuihairu.site`，`collab_defaults.hpp:15`）。

## 1. 目标（用户令 2026-10-15）

1. 单一设置页：左侧分类／右侧内容两栏布局。
2. 分类至少含：连接（服务器地址、端口、连通校验、初始化向导——与 R17
   合并）、外观与品牌、快捷键、任务提供方、离开/状态、通知与消息状态
   （已读回执、盯提醒）。
3. 服务器地址只有一个填写点，其它地方引用不得再各存一份。
4. 现有 5 个设置页能力一个不少，只是归到统一入口下。
5. R17 手机端初始化向导与「连接」分类共用同一份配置源，不许两套。

## 2. 目标布局（分类布局拍板）

**形态**：独立 `SettingsDialog`（QDialog，约 880×560），与现有对话框生态
一致，不侵入主窗 rail 布局。左列 `QListWidget` 分类（图标＋名），右列
`QStackedWidget` 内容页；底部状态行沿用现有 `status_` 保存反馈模式。

**分类清单（六类，展开项＝迁入来源）**：

1. **连接**——服务器地址／端口／文件口／账号（从登录对话框迁入，
   `main_window.cpp:3840-3871`）＋域名解析覆盖（从「连接设置…」迁入，
   `main_window.cpp:3886-3920`）＋**连通校验**按钮（复用 R17 同款探测：
   PING→PONG 通过才允许保存，探测逻辑对齐移动端 `InitPage.ets:59`）＋
   R17 初始化向导说明行（移动端向导与本页同源的指引）。
2. **外观与品牌**——主题（`ThemeSettingsPage` 内容迁入）＋开机启动（勾选
   迁入）＋品牌物料（org-admin 拉取/保存表单迁入，`brand_settings_page.cpp`）。
3. **快捷键**——截图快捷键改键＋冲突检测（`ShortcutSettingsDialog` 内容
   迁入，`shortcut_settings.cpp`）。
4. **任务提供方**——provider 选择＋凭据＋口令门（`TaskProviderSettingsDialog`
   内容迁入；加密落盘 `TaskProviderStore` 不变）。
5. **离开与状态**——离开密码／超时／启用（`AwayLockSettingsDialog` 内容
   迁入）＋网段黑名单＋远程控制配对密码（`NetRemoteSettingsDialog` 内容
   迁入；「状态」含本机网络安全态）。
6. **通知与消息状态**——通知偏好（NotificationCenter 设置面板迁入）＋
   发送已读回执全局开关（菜单勾选迁入；会话级覆盖仍留会话右键，
   `main_window.cpp:3170-3191`）＋**盯提醒阈值/过期 UI**（新界面：
   `watch/remind_minutes`、`watch/expire_hours` 现只能手改 INI——补界面，
   默认 10 分钟／24 小时不变）。

**不收编**：个人资料…（个性签名是账号档案不是本机设置）保留独立菜单项。

**菜单收敛**：设置菜单收敛为「设置…」「个人资料…」两项；rail 齿轮弹
「设置…」（`main_window.cpp:590` 同源菜单改指统一页）。

## 3. 配置源收敛方案（单一填写点）

**存储不动**：两个 QSettings 库保持原样（memex/Memex 默认库＋memex/collab）。
改键名＝迁移负担，不值；收敛的是 **UI 写点**，不是键。

**UI 写点收敛铁律**：每个键的写点＝设置页对应分类，全端唯一。

- `host/port/files_port`：**连接分类唯一写点**。登录协作态对话框删
  host/port/files_port 三字段，只留账号/口令（地址从配置读、只读显示），
  「登录协作态」语义回归纯登录；`approval_dialog.cpp:190` 的 `files_port`
  回写拔掉（改读配置）。
- 13 处工具对话框「服务器地址」填框：全部改为**只读连接徽标**（显示
  resolve 后 `host:port`）＋「在设置中修改」按钮跳设置页连接分类；不落盘、
  不改配置。各对话框的账号/口令字段保留——那是文件面会话鉴权凭据，
  不是配置。
- `host_overrides`：并入连接分类（原「连接设置…」弹窗退役）。
- 代码层收口：扩展 `collab_defaults.hpp` 为连接配置读写封装
  （`collab_conn_host()/collab_conn_port()` 等），凡读连接配置只走它；
  写只走设置页保存路径。`resolve_host_rules` 保持纯函数（`test_host_override`
  照旧）。

**R17 同源约束（不许两套）**：

- 移动端向导落盘（Android `PrefsInitStore` 键 `server_host/server_port`；
  鸿蒙 preferences `memex_init`）与桌面 `QSettings("memex","collab")`
  `host/port` **语义同一**：同键义（地址＋端口）、同默认值
  （`default_collab_host()` 常量同源）、同校验（PING→PONG）。
- 桌面端连接分类今后若做首启向导，**必须复用 memex/collab 的 host/port
  键**，禁止新增第二键名；移动端将来加设置页同样复用 `PrefsInitStore`，
  不许另起。
- 验收口径：同一份配置在桌面设置页与移动端向导两处可见可改，改一处另一
  处下次打开即见（各自设备本地落盘，无同步要求——「同源」指键义与默认
  值同源，不是跨设备同步）。

## 4. 迁移兼容

- **现有对话框类不删**：五页（away/brand/net/shortcut/task_provider）的
  构建与保存逻辑原样保留；设置页内容页直接复用其 `build_ui` 与
  `save_settings` 程序化缝（这些缝本就是为测试设计的）。短期「分类页内嵌
  原对话框主体」，测试缝不动；二期再视情况抽公共 widget。
- **测试兼容**：`test_mode_switch`（NetRemoteSettingsDialog 等，
  `test_mode_switch.cpp:810`）、`test_brand_settings`、
  `test_task_provider_settings`、`test_theme`、`test_net_guard`、
  `test_watch_reminder`、`test_host_override` 照旧全绿；新增
  `test_settings_consolidation`：设置页打开六分类、连接分类保存后
  memex/collab `host` 落盘、工具对话框徽标只读、旧菜单项移除。
- **数据兼容**：不改键名→零迁移；老 INI 直接生效。
- **入口兼容**：旧菜单键不留快捷方式（菜单项本就浅，一步到位）；
  「登录协作态…」保留但语义收窄为纯登录。

## 5. 分期（本轮只设计，不排期实现）

- **一期**：设置壳＋六分类内嵌现有对话框主体；连接分类接管
  host/port/files_port/host_overrides 写点；登录对话框拔地址字段；
  13 处工具对话框改只读徽标。
- **二期**：盯提醒阈值 UI、连通校验 probe、主题/通知面板内嵌打磨。
