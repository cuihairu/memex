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
| `desktop-friends.html` | `client/app/main_window.cpp`（好友视图：rail「好友」钮，`refresh_devices` friends 分支） | 已对齐 | 协作态通讯录侧栏；2026-10-30 逐屏复核补齐：rail「好友」入口切真·好友视图（星标好友＋部门分组头「全路径 · N 人」）、行控件复用会话行工厂（40px 头像/在线点/「人名 · 部门末级」/时间/末条预览/未读胶囊）、side-head「好友」＋「搜索好友 / 工号」＋「来自组织架构通讯录」注脚＋「N / M 在线」绿点计数、tooltip 账号·部门（d94d616）。近似项：部门分组头用全路径非仅末级（末级同名会撞）；无加友审批流故注脚不照抄「须经对方同意」；无历史行预览走「暂无消息」非在线兜底；离线行 opacity .55 未渲染（通讯录即全量在线表驱动）；「发消息」悬浮钮以双击/回车打开会话实现 |
| `desktop-group.html` | `client/app/main_window.cpp`（群聊天面板：`open_chat`/`update_group_panel`/`rebuild_at_menu`）+ `group_tools_dialog.cpp` | 已对齐 | 群聊天面；2026-10-30 逐屏复核补齐：右栏 infopanel（群成员·N＋28px 头像行＋群主胶囊＋「查看全部 N 人」开群资料＋群文件≤3＋归档说明虚线卡）、会话头 meta「N 人 · 群主：X · 全量归档」（群号挪标题悬浮）、头部 📢群公告/🗂群文件(切右栏)/👥成员(brand)钮、横幅公告口径（「群公告（群主：X）：…」品牌底；无公告走通用归档口径）、气泡 meta 群主标签（10px 品牌色）、「@ 唤起成员选择」提示＋占位「输入消息，@ 成员可加提醒…」＋@词头弹成员菜单回填「@显示名 」（0148ddc/5735bda）、「群消息全量归档」描边胶囊、群行副行「N 人 · 群公告已更新」、临群行控件化（opacity .7＋「免服务端 · 仅本网段 · 不归档」＋「未归档」徽标）。近似项：公告横幅无「更新于 HH:mm」（服务端只存公告文本不存更新时刻，署群主）；群文件列＝本会话窗口内最近 3 个文件卡（文件不落消息库、重开窗口从零累计），空态如实「暂无群文件」；已读显示为 ✓/✓✓ 回执勾（在案设计决策，非原型「已读 N/M」计数）；「查看全部 N 人」开既有群资料弹窗（非列表内展开）；侧栏群行处于共享会话列表（随 desktop-collab 对齐口径），原型左栏「群聊」筛选态未单列 |
| `desktop-files.html` | `client/app/main_window.cpp`（传输管理面：`enter_transfers_view`/`refresh_transfers_view`/`rebuild_transfer_side`） | 已对齐 | 整屏传输管理器；2026-10-30 逐屏复核落地（937ba5d）：rail「文件传输」钮切真·传输面（会话面互斥显隐＋同步 activate 结算几何）、220px「传输通道」侧栏（40px 头像行：点对点直传 active brand 底／共享空间置灰「Samba · 未接入」＋tooltip／旁路设计虚线卡按真实授权留痕口径／「本机已发现同网段直传伙伴 N 台」品牌卡 N=发现表实时数）、头部「文件传输」＋meta「正在传输 N · 今日完成 N · 累计 X」（真实台账聚合）＋↑发送文件 brand 钮（发往当前会话）、标签行 全部／传输中 N／已完成／已暂停、传输卡（36px 图标：文件夹 ▸／首字头像；名「目录名/（文件夹 · N 个文件）」；副行「发给/来自 X · 大小 · 完成 HH:mm」；状态胶囊 已完成/传输中/已中断/已取消；6px 进度条完成＝ok 色；foot「✓ 接收方已确认／已保存至本机 · SHA-256 已校验」＋操作钮 取消/打开目录/所在会话（回跳会话面））、页脚授权留痕说明卡。近似项：已暂停 tab＝诚实空态（本版无暂停 API，文案给「取消后重发自动断点续传」口径）；共享空间行置灰不接（无 Samba 功能体）；旁路卡/页脚卡按真实授权留痕口径（无原型「跨网段」能力声明）；台账会话内存级（文件不入消息库，重启从零累计，页脚如实注明）；✓SHA-256 双向如实（发送完成＋接收落盘均校验）；速度/剩余＝进度差分（无真实速率通道）；无「已续传 N 次」计数；发送文件＝发往当前会话（无跨会话选择）；↑发送文件用基础箭头字形（原型 SVG 上箭头，⬆ 图标面字符在抓图环境渲染豆腐块）；传输卡纵向 Maximum 封顶（滚动区多余空间让尾部 stretch，防卡片拉成半空高卡） |
| `desktop-org.html` | `client/app/main_window.cpp`（组织架构整屏视图：`enter_org_view`/`rebuild_org_view`/`refresh_org_members`/`show_org_profile`）+ `rail_nav.cpp` | 已对齐 | 全公司唯一通讯录面；2026-10-30 逐屏复核落地（6ef8efb）：rail「组织架构」钮切三栏整屏——240px 左栏（「搜索部门 / 成员」过滤＋QTreeWidget 部门树带右对齐子树计数列＋「全部成员」根＋通讯录可见性虚线说明卡）、中栏（「部门 · N 人」头＋部门负责人胶囊＋成员卡网格：44px 头像/姓名/部门·职务副行/在线点，「（我）」卡品牌底）、右栏档案面板（64px 头像/在线徽标/汇报链路沿 manager 上溯（环防御 6 跳）/kv 直属上级·部门·职务·在线时长/「发消息」「设为常用联系人」钮/数据口径注脚）。近似项：无机要室行（服务端已按可见性裁剪，隐藏数据不出服务端，以虚线说明卡代之）；部门负责人为结构推导（部门内 manager 不在本部门子树者唯一时给出，多候选省略不冒充）；kv 只摆真实字段（办公位置/手机协议未开，如实不摆；无工号字段）；在线态取真实在线表；星标走真实常用联系人表（T4.5 fav） |
| `desktop-search.html` | `client/app/archive_console.cpp`（管理台「归档检索台…」整窗控制台；主窗菜单挂接 `main_window.cpp`） | 已对齐 | 2026-10-30 逐屏复核落地：管理台菜单整窗控制台（auditor 专属，判权全在服务端）——头行连接徽章＋「归档与检索 · 审计岗」胶囊、连接行（未连接呈现/连上收起）、控制台标题与副题照原型、条件行（人员账号 170px/时间窗勾选＋起止至/关键词 stretch/「精确匹配」说明胶囊＝真实子串 AND 语义/检索 brand 钮）、五列命中表（关键词品牌色加粗高亮（服务端同款分词大小写不敏感）／会话对端「甲 ⇄ 乙（单聊）· →（群）」／归档时间 MM-dd HH:mm:ss／标记列「已撤回 · 原文留存」warning 胶囊／操作列 查看上下文·撤回事件 平钮）、页脚命中计数＋「导出留证」钮＋证据包回显行、查阅日志台账（每次检索/上下文/撤回事件/导出各落一条服务端 audit_reads 行，含被拒尝试留痕）、状态行。新面全部真实后端：服务端三路 `/files/audit/` 端点（context/recall-event/export）＋store 两法（`context_around` 会话窗口 span 5/`last_recall_event`）；撤回原文保留可检可导。近似项：人员条件按账号过滤（控制台无组织树/显示名映射面）；命中单页 ≤200（默认，无分页异步加载）；导出为页脚级证据包（非逐行导出）；文件传输类消息不经服务端归档无对应行（不造数据）；「归档起点」说明行无真实起点数据未摆；精确匹配胶囊为静态语义说明（无 fuzzy 切换）。会话审计对话框（desktop-audit）分立保留，同后端两形态 |
| `main-empty.html` | `client/app/main_window.cpp`（登录后空态：`refresh_devices` 空表 hint ＋ `render_welcome_guide`/`render_empty_state`） | 已对齐 | 2026-10-30 逐屏复核落地：登录后零会话列表态——侧栏两行引导「还没有会话／从通讯录选择联系人，开始聊天」（muted 12px，与原型 `.empty-list` 逐字一致；有会话行即撤、登出即撤），主区欢迎页（`.first-guide`：标记＋「欢迎使用 Memex」＋副题＋四链接卡文案）按布局令（2026-10-08 列表态坍缩 320px 面板隐藏）潜伏渲染就位，`close_chat`/`refresh_devices` 共用 `render_empty_state` 出口（替换对已登录用户误导的「请登录协作态」文案）；实况进 docs（layout-empty-*.png 浅深配平）。近似项：✦ 字形以 ● 代（抓图环境未验证）；动作钮/链接卡为 QTextEdit 不可点交互件——以说明文案呈现不造假控件（rail 有真钮）；hint 落列表首行非整栏垂直居中；side-nav「全部/未读/群聊」页签由协作态分区取代；「已登录」徽章＝原生标题栏 |
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
| `desktop-tasks.html` | `client/app/task_dialog.cpp`（`task_provider_store.cpp` / `task_provider_settings.cpp` / `task_template_settings.cpp`） | 已对齐 | 双栏 body-split（298px 录入列：月历/录入区/外部任务登记/脚注；视图列：任务清单列表/看板/领导视角泳道/状态行）＋连接区（已连接徽章）＋工具行（拉取外部并入右侧）。近似：连接区可编辑兼连接入口（原型只读徽章）；列表↔看板按「看板」按钮互斥切换（原型静态并示两栏）；截止/开始为单 QDateTimeEdit（原型拆日期+时间两格）；月历默认收起、QCalendarWidget（原型自绘网格）；状态行文案沿用测试口径「清单已刷新」；只看未完成默认关 |
| `desktop-approval.html` | `client/app/approval_dialog.cpp` | 有偏差 | 四态与权限说明为原型补充栏 |
| `desktop-report.html` | `client/app/report_dialog.cpp` | 有偏差 | 原型含周报补写与下属日报两栏示意 |
| `desktop-group-tools.html` | `client/app/group_tools_dialog.cpp` | 已对齐 | 三页 Tab（原型自注「实现为三页 Tab，原型并列展示三页」）；每页 ts-head 规则副题＋单行发起区（录入字段吸收余宽、操作钮贴右）＋连接区「已连接」徽章＋脚行判权/结果卡说明；窗题「群工具 — 群 N」。近似：连接区可编辑兼连接入口（原型只读徽章）；条目行 QListWidget 文本行（[进行中]/[已截止] 前缀，原型 pill 徽标＋计数条卡片）——行文案为测试契约口径 |
| `desktop-group-memo.html` | `client/app/group_memo_dialog.cpp` + `main_window.cpp:2913`「公告编辑历史」 | 有偏差 | 修订历史与凭据检测提示 |
| `desktop-group-vault.html` | `client/app/group_vault_dialog.cpp`（`group_vault_crypto.cpp`） | 已对齐 | 双栏 split（400px 条目列：解锁行／掩码列表／条目动作行／箱管理区「群主/管理员：」＋四钮＋右注独占一行≈原型 flex-wrap；详情列：条目详情卡＋⚠ 硬性分离告警）＋连接区右上「已连接（箱密码仅本地参与解密，服务端不可找回）」徽章；窗 1000×660。近似项：连接区可编辑兼连接入口（原型只读徽章）；条目行 QListWidget 单行文本「名称·账号·维护人·时间」（原型两行卡：名称/账号/掩码点＋解锁态/维护人/时间元行，行文案为测试契约口径）；详情卡查看后以 QLabel 呈现（原型 kv 网格＋卡内复制/审计钮）；审计与新建条目为独立对话框（原型并列卡） |
| `desktop-group-ci.html` | `client/app/group_ci_dialog.cpp` | 已对齐 | 双栏 split（左流水线列：sec-head「流水线（红绿灯）未跑过＝灰 · 最近成功绿 / 失败红」＋红绿灯列表＋触发行（参数 JSON＋触发构建＋「点触发=留痕到群日志，可追溯到人」）＋管理区两行（流水线名/描述/新建更新＋删除/开放成员触发/刷新，≈原型 flex-wrap）；右列：运行历史卡（「谁触发可追溯」＋「actor · 参数 · 结果 · 留痕」副题）＋工具凭据卡（「仅服务端保存」「客户端零凭据」＋掩码态＋「外部 token 只存服务端；复用 R24 保险柜加密面」）＋动作语义/危险动作两枚说明条）＋连接区右上「已连接」徽章与「群号 N」位；窗 1000×640。近似项：连接区可编辑兼连接入口（原型只读徽章）；流水线/run 行 QListWidget 单行文本（原型两行卡：状态点＋名/描述/维护人＋最近行、run 行 #id 状态列＋时间右贴）——行文案为测试契约口径；凭据卡无「ghp_••••」掩码串展示（协议无值回显，只有文字掩码态） |
| `desktop-group-pack.html` | `client/app/group_pack_dialog.cpp` | 已对齐 | 双栏 split（左出产物列：sec-head「出产物 一键打包 · 群文件面归档 · 打包动作留痕」＋产物台账＋白名单说明条＋打包行（产物名/版本/备注＋打包）＋动作行（删除选中/开放成员打包/刷新＋「删除产物＝危险动作（可配二次确认）」注）；右列：sec-head「群配置快照导出 只读 JSON · 脱敏（凭据只出状态不出值）」＋快照只读面＋「导出群配置快照…」「另存为文件…」＋同源留痕/不含消息归档说明条）＋连接区右上「已连接」徽章与「群号 N」位；窗 1000×640。近似项：连接区可编辑兼连接入口（原型只读徽章）；产物行 QListWidget 单行文本（原型图标卡：PKG/CFG 徽标＋名/副题＋时间右贴）——行文案为测试契约口径；快照为 QPlainTextEdit 纯文本（原型等宽字体带键着色） |
| `desktop-group-servers.html` | `client/app/group_server_dialog.cpp` | 已对齐 | 双栏 split（左服务器列：sec-head「共享服务器（红绿灯 + 负载）入群即视 · 退群即失」＋红绿灯台账＋登记行（服务器名/地址＋登记服务器/刷新）＋注册令牌一次性框＋「查看 + 连接/高危走审批/访问留痕」说明条；右列：远程会话卡（「一次性短票」＋「#id · 服务器 · 协议 · actor · 状态 · 时长」副题）＋发起会话卡（「SSH 先行 · RDP/VNC 后续」＋发起/关闭＋本地命令框＋零凭据注）＋服务器凭据卡（存/删凭据＋「只存服务端加密面（同 R24 保险柜）」注））＋连接区右上「已连接（agent 心跳 15s · 掉线 60s 判离线）」徽章与「群号 N」位；窗 1000×640。近似项：连接区可编辑兼连接入口（原型只读徽章）；服务器行 QListWidget 三行文本（原型指标格卡：CPU/内存/磁盘/负载四格＋心跳滞后角标）——行文案为测试契约口径；发起会话卡无「粘贴短票」输入框（实现为选中服务器直发——短票服务端签发即兑现，永不出框）；原型「远程命令（登记期一次性命令）」卡无对应协议未实现 |
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
