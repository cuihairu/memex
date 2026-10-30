# 需求与任务清单（todo）

> 依据《建设方案评审报告 V10.0》拆解：R 条目＝需求与验收口径，A 条目＝第一期验收标准，任务按四期拆到可验收粒度。
> 追加需求（评审报告之后由建设方下达）以「R17／R18」编号进入本清单。状态：`[ ]` 未开始 · `[~]` 进行中 · `[x]` 完成。
> **鸿蒙后置（2026-10-04 用户令，长期有效）**：所有 HarmonyOS／NEXT 相关任务（含 T6.2 桌面矩阵项、T6.6 鸿蒙手机版）一律排到队列末尾，让位非鸿蒙任务；且降为**只写代码、不做本地测试验证**（当前无真机环境，验证做了也不作数，编译过即可）；代码照常提交推送但注明「待真机验证」。
> 2026-10-13 精简：每条目只留需求一句＋证据一行；实施细读全文见 git 历史与本文件旧版备份（todo.md.bak）。

## 零、留痕原则（铁律，任何任务不得与之冲突）

**连接服务端的唯一目的是留痕。** 以下功能永久排除，不排期、不留接口、不作为可配置项：

- 密聊／阅后即焚（不存在「发送后自动消失」的会话形态）；
- 撤回抹除归档（撤回仅作用于客户端显示，服务端保留原文＋撤回事件记录：谁、何时、撤回哪条）；
- 终端侧单方删除归档（清本地缓存不影响服务端记录，界面区分「本地缓存」与「云端归档」）；
- 面向使用者的留痕开关（留档策略仅管理侧按部门下发）。

衍生口径：归档以**消息**为单位判定（不以会话为单位）；跨态会话唯一留痕缺口必须以「未归档」标识显式暴露；媒体内容不落地存储，但「发生过这次通信」的元数据必须归档。

## 一、需求清单（R 条目）

| 编号 | 需求 | 优先级 | 验收口径 | 期次 |
| --- | --- | --- | --- | --- |
| R1 | 不登录即可使用，无需配置 | 必须 | 安装后不填地址、不注册即可自动发现同网段终端并收发消息，行为与飞秋一致 | 一 |
| R2 | 直连态消息不经服务器 | 必须 | 点对点传输，服务端无任何记录，抓包可验证 | 一 |
| R3 | 登录后聊天记录集中留存 | 必须 | 协作态消息统一落服务端；管理员可按人员／时间／关键词检索；终端删除不影响留存；不提供任何可由发送方消除已归档内容的功能 | 一 |
| R4 | 换机登录与多端管控 | 必须 | 账号可在不同电脑登录；桌面端同账号仅一台在线，重复登录前一个被踢并收到提示；每次登录记录设备类型与标识 | 一 |
| R5 | 双态集成于同一客户端 | 必须 | 单一安装包；未登录为直连态，填地址登录后切换协作态，切换不重启、不丢本地历史 | 一 |
| R6 | 局域网文件共享 | 必须 | 目录级共享与权限区分；文件字节不经公网（内网无公网出口） | 一 |
| R7 | 归档范围可标识 | 必须 | 跨态会话界面明确标注「未归档」且不可关闭；归档起点可追溯（「归档自 X 时刻」） | 一 |
| R8 | 国产操作系统 | 重要 | 至少覆盖统信 UOS 与麒麟 Linux，消息／文件／界面显示正常 | 一 |
| R9 | 跨网段与多地域、大文件 | 重要 | 跨网段与跨地域由协作态承接（专线接入同一服务端）；直连态限本网段；支持整目录发送与断点续传 | 一 |
| R10 | 功能范围参照企业微信 | 重要 | 按二期分期清单逐项验收，不要求一次性达成 | 二起 |
| R11 | 内网 AI 能力 | 计划 | 群内助手、会议纪要、需求整理；模型内网部署；归档数据只送本地模型 | 三 |
| R12 | 服务端高可用 | 重要 | 服务端故障时直连态仍可通信；具备备份与恢复预案 | 一 |
| R13 | 沟通体验 | 重要 | 内置截图与标注后直接发送；自定义表情包导入；常用联系人置顶与快速查找 | 一 |
| R14 | 组织架构含汇报关系与可见性 | 重要 | 成员资料可查看直属上级；按部门设置通讯录可见范围；敏感字段可隐藏 | 一 |
| R15 | 办公室位置图 | 计划 | 楼层平面图定位工位；搜索姓名图上高亮；按园区楼层限定可见 | 二 |
| R16 | 远程协助 | 计划 | 请求远程查看／控制；被控方显式授权、过程持续可见、全程留痕；默认仅 IT 运维 | 二 |
| **R17** | **手机端首次连接必须初始化服务器地址**（追加） | 必须 | 移动端首次启动进入初始化向导，第一步为域名／服务器地址设置，填写并连通校验通过后才能进入主界面；跳过无效——未初始化的移动端不出现任何可聊天界面 | 一 |
| **R18** | **手机端不提供匿名聊天**（追加） | 必须 | 直连态／免登录能力仅存在于桌面端；移动端全部为协作态，必须登录后使用；移动端界面不出现「免登录体验」「附近设备直连」类入口 | 一 |
| **R19** | **客户端主题切换**（追加） | 重要 | 亮／暗双主题：跟随系统＋设置内手动切换，选择持久化；颜色令牌化并预留多主题扩展位，品牌橙（#e16531）为主色 | 一（主线后） |
| **R20** | **局域网设备自发现**（追加，2026-10-04 用户反馈） | 必须 | 同一局域网的设备应能自动发现自己／互相发现（服务广播发现），不需手动添加；发现失败须有可见提示而非静默无反应 | 一（待排期） |
| **R21** | **自己给自己发消息＝备忘录**（追加，2026-10-04 用户反馈） | 重要 | 与自己对话即便签／备忘录场景：可给自己发文本与文件并回看，会话在会话列表中常驻可检索 | 一（待排期） |
| **R22** | **手机给自己发＝文件传输助手**（追加，2026-10-04 用户反馈） | 重要 | 手机↔自己互传文件，对齐微信「文件传输助手」体验：手机端与桌面端同账号给自己发文件双向可达 | 一（待排期） |

## 企业内网办公 IM 缺口盘点(2026-10-12)

> 用户令：照抄四档入册，不增删条目、不改期次；条目括注出处与验收口径锚点
> （R 条目的期次与验收口径以§一 R 表为准，此处不复制全文）。
> **R10「功能范围参照企业微信＝二期起」**——下列缺口属「未到期」，不是
> 遗漏；与 R 条目冲突时以 R 条目为准，不改 R 条目本身。

### 一、已落地（一期主体）

- 双态（直连免登录＋协作态）——R1/R2/R5（一期）
- 留痕铁律与检索——R3（一期；铁律见§零）
- 账号设备与同号单端在线踢前一个——R4（一期）
- 组织架构含汇报关系可见性——R14（一期）
- 文件共享/断点续传/文件面 S3/导出——R6/R9/R23（一期；R23 设计=docs/design/文件存储与外网单向传输.md）
- 群聊＋已读＋撤回＋免打扰＋置顶（一期协作态功能面，T4 系列）
- 截图标注——R13（一期）
- 主题切换——R19（一）
- 任务清单——R27（设计=docs/design/个人任务清单与外部工具接入.md）
- 多平台（分级如实）——R8（一期，验收口径见 R 表）
- Docker＋CI＋tunnel 部署（服务器部署批 2026-10-11；README 部署章）
- webhook 入口（T4.10；服务端 24361 口）
- 远程协助——已实现（R16；2026-10-15 代码实读核实，非关键词打勾：入口「远程协助…」main_window.cpp:577-585，UI assist_dialog.cpp:63，链路 /files/assist/* 服务端 HTTP 中介 files_client.cpp:1000-1124；consent/audit 全留痕符合 R16 口径；注：实读核实=代码在，未做真机走查）
- WebRTC 媒体底座待实现：当前远程协助用 500ms JPEG 抓屏轮询＋服务端 HTTP 中继（client/app/assist_dialog.cpp:3,395-420；client/engine/collab/files_client.cpp:1074-1097；server/src/files_server.cpp:1015-1019,3170-3197）；媒体库/连接与 WebRTC 路由未接入。来源：docs/review/gpt6-luna-review-2026-10-18.md「WebRTC 架构承诺领先于实现」核对项；docs/plan-review-report-v10.0.md:42,59,440 的媒体底座属于目标架构，不代表已交付。

### 二、在做或刚立项

- 聊天记录模糊搜索（2026-10-12 已合入）
- OIDC 统一身份（设计在写：docs/design/OIDC身份认证接入设计.md）
- LDAP/AD（todo 未完成项，「可靠性与安全边界」节内）
- admin 网页化（「admin 管理后台」节，2026-10-11 立项）
- 可靠性与安全边界批（「可靠性与安全边界」节，2026-10-11 立项）
- 飞秋/IPMSG 兼容（「飞秋/IPMSG 兼容」节，2026-10-12 落地）

### 三、只有设计稿未实现

- 审批与日报周报（设计=docs/design/审批与日报周报.md；最小闭环已闭，流程引擎等余量未做）
- 机器人平台（现只有 webhook 裸入口；设计=docs/design/机器人平台.md）
- 群组知识共享 R24（设计=docs/design/群组知识共享.md；公告/备忘录/密码箱已闭，余量未做）
- 群工具框架 R25（设计=docs/design/群工具框架.md）
- 群服务器工具 R26（设计=docs/design/群服务器工具.md）
- 办公室位置图 R15（二期）
- 群内智能助手与模型网关 R11（三期）

### 四、完全没有

- 视频会议——未实现（2026-10-15 实读：proto 无任何 VIDEO 帧类型 memex.proto:9-53 止于 MESSAGE_PAGE=58；采集/传输零符号）
- 语音通话——未实现（2026-10-15 实读：全仓零命中，proto 无 VOICE/AUDIO）
- 屏幕共享——部分实现（仅远程协助子功能，无独立入口：抓屏→JPEG base64 轮询 assist_dialog.cpp:400-412 经 /files/assist/frame，非实时流）
- 远程控制——仅设置壳（net_remote_settings.cpp:71,137-148 落盘开关＋配对密码 net_guard.hpp:38-44；引擎/协议零实现——与「远程协助」是两个功能）
- 消息编辑（全仓 0 处）
- LDAP/AD 落地
- 多端同时在线（R4 现为单端在线，与企业微信口径不同）

### 五、六项功能实现状态核实（2026-10-15 用户扫描＋代码实读复核）

> 用户令：逐项核实真实实现状态（找入口、传输链路、UI），不许看到中文字符串
> 就打勾；已实现给 file:line，未实现写清「未实现」。核实=代码实读，非真机走查。

| 功能 | 状态 | 入口 | 传输链路 |
| --- | --- | --- | --- |
| 截图 | ✅ 已实现 | main_window.cpp:904-914（按钮+Ctrl+Alt+A，可改键 shortcut_settings.cpp:23-57） | direct_engine.cpp:399 → file_transfer（FILE_META/RESUME/DONE memex.proto:17-19）；实现 screenshot_tool.cpp |
| 文件传输 | ✅ 已实现 | main_window.cpp:895-902（发文件/发文件夹）、file_assistant.cpp:162 | file_transfer.cpp:154-232,370-376；授权门 FILE_AUTHZ memex.proto:39-40 |
| 远程协助 | ✅ 已实现（HTTP 中介） | main_window.cpp:577-585 → assist_dialog.cpp:63 | files_client.cpp:1000-1124（/files/assist/*，帧=JPEG base64 POST/GET） |
| 视频会议 | ❌ 未实现 | — | proto 无 VIDEO（memex.proto:9-53）；采集/传输零符号 |
| 屏幕共享 | ⚠️ 部分实现（远程协助子功能） | assist_dialog.cpp:341,381-387 | assist_dialog.cpp:400-412 抓屏→JPEG→b64→files_client.cpp:1074-1087；轮询非实时流 |
| 语音通话 | ❌ 未实现 | — | 全仓零命中；proto 无 VOICE/AUDIO |

对用户扫描的三处修正：①「远程协助无实现符号」不成立——assist_dialog＋
files_client /files/assist/* 是完整实现，只是走服务端 HTTP 而非直连 proto；
②「远程控制」（net_remote_settings/net_guard）与「远程协助」（assist_dialog）
是两个功能，前者仅设置壳；③屏幕共享不是独立功能，是远程协助内的 JPEG
轮询抓屏。音视频是重功能（采集/编码/传输），自研 vs 复用见「待用户拍板」②，
本轮不实现。

### 待用户拍板

1. 多端同时在线改不改口径；
2. 音视频自研还是复用内网现成组件（建议复用，单独立项）；
3. 身份源先做 LDAP/AD 还是先做 OIDC（两案并存怎么排）。

补齐顺序建议（建议，非定案）：身份与管理 → 审批＋日报周报 → 机器人平台 → 音视频单独立项。

## 二、第一期任务拆解（可验收粒度）

### 阶段 0：工程骨架

- [x] T0.1 monorepo 聚合 CMake：client／server／common／admin 目录与 MEMEX_BUILD_* 构建开关，顶层构建＋ctest 通过。
  证据：顶层 CMakeLists.txt；当前全量 ctest 82/82 绿。
- [x] T0.2 协议骨架：帧格式（长度前缀＋JSON/proto 载荷）、消息类型枚举、序列化往返单测。
  证据：common/proto/memex.proto＋common/src/frame.cpp；common/tests/test_protocol.cpp。
- [x] T0.3 服务端骨架：asio TCP 监听、连接接入、握手与心跳、优雅退出、`[MEMEX]` 日志。
  证据：server/src/main.cpp＋server/src/server.cpp＋server/src/session.cpp。
- [x] T0.4 客户端骨架：Qt6 Widgets 空主窗可启动，DirectEngine／CollabEngine 双引擎注册，offscreen 冒烟。
  证据：client/app/main_window.cpp＋client/engine/{direct,collab}/；client/tests/test_mode_switch.cpp。

### 阶段 1：直连态（对应验收 A1、A2、A11、A13 的直连半边）

- [x] T1.1 UDP 广播发现：绑定 2425 周期宣告（设备名、账号占位、协议版本），同网段在线列表（加入／超时离开）。
  证据：client/engine/direct/discovery.cpp；client/engine/direct/tests/test_discovery.cpp。
- [x] T1.2 直连单聊文本：TCP 点对点（2426 起）送达确认＋本地 SQLite 落库（来源＝直连）。
  证据：client/engine/direct/direct_engine.cpp；client/core/local_store.cpp；client/engine/direct/tests/test_direct_chat.cpp。
- [x] T1.3 直连文件传输：文件与目录传输、进度、断点续传（分块＋偏移协商），哈希一致。
  证据：client/engine/direct/file_transfer.cpp；client/engine/direct/tests/test_direct_file.cpp。
- [x] T1.4 直连态界面：设备列表（替代联系人）、会话列表、聊天窗、「未归档」本地标记、无设备引导。
  证据：client/app/main_window.cpp（直连列表与聊天面）；client/tests/test_window_layout.cpp。

### 阶段 2：服务端与协作态（A3–A6、A8、A11、A16）

- [x] T2.1 账号与设备：密码摘要登录、设备指纹（SHA-256 分级）、登录记录（类型/名称/时间/来源/版本）、桌面单点在线互踢。
  证据：server/src/store.cpp（accounts/devices/logins 表）；server/tests/test_accounts.cpp。
- [x] T2.2 接入网关与消息通道：长连接、心跳、断线重连、离线补投、msg_id 去重、跨类型并存。
  证据：server/src/session.cpp；server/tests/test_gateway.cpp＋test_msg_dedupe.cpp。
- [x] T2.3 消息归档：协作态消息全量落库（撤回只置标记不清正文）、撤回事件独立记录、本地缓存与归档分离展示。
  证据：server/src/store.cpp（messages/recall_events）；server/tests/test_archive.cpp。
- [x] T2.4 模式切换与降级：登录／登出不重启切换，本地历史合并展示（来源字段），服务端不可达回落直连并明示「消息不进归档」。
  证据：client/engine/collab/collab_engine.cpp；client/tests/test_mode_switch.cpp。
- [x] T2.5 断线补传归档（决策事项 8 建议方案）：中断期消息本地暂存，恢复后按消息标识去重补传，无重复无缺失。
  证据：client/engine/collab/tests/test_collab_resend.cpp（SIGSTOP 冻结服务端崩溃恢复腿）。
- [x] T2.6 组织架构：部门树、成员资料、直属上级（至多一名）、上级链路上溯、CSV 批量导入与错误项校验。
  证据：server/src/store.cpp（org 表族）；server/tests/test_org.cpp。

### 阶段 3：管理后台与策略（A5、A6、A9、A20）

- [x] T3.1 管理后台骨架与成员部门管理：增删改查、批量导入、角色与权限分级。
  证据：server/src/main.cpp（account/org CLI 族）；server/tests/test_files_api.cpp。
- [x] T3.2 消息检索与导出：按人／时间／关键词检索、导出留证、查阅行为记日志。
  证据：server/src/main.cpp（messages CLI＋audit 留痕）；server/tests/test_archive.cpp。
- [x] T3.3 设备台账与登录记录：设备列表、责任人登记、启停解绑、登录记录查询。
  证据：server/src/main.cpp（device/logins CLI）；server/tests/test_accounts.cpp。
- [x] T3.4 策略开关下发（按部门）：允许免登录使用、允许与未登录设备通信、新设备登录需审批。
  证据：server/src/store.cpp（policies）；server/tests/test_visibility.cpp。

### 阶段 4：群聊、跨态与体验（A7、A13、A17、A18、A20）

- [x] T4.1 群聊：建群、拉人、群公告、@ 成员、群消息归档；免服务端临时群（直连态）单列。
  证据：server/src/session.cpp（GROUP_CMD）；server/tests/test_group.cpp。
- [x] T4.2 跨态互通：已登录端发现未登录端、「未归档」固定标识、跨态建立日志上报、归档起点「归档自 X 时刻」。
  证据：client/app/main_window.cpp（跨态标记/归档横幅）；client/tests/test_mode_switch.cpp。
- [x] T4.3 消息状态与多端：发送状态、已读回执、在线状态；桌面单点在线提示（与 T2.1 合并验收）。
  证据：server/src/session.cpp（READ/READ_NOTICE）；client/engine/collab/tests/test_read_presence.cpp。
- [x] T4.4 截图与标注：截屏、涂鸦／箭头／马赛克／文字、确认即发送；多显示器与高 DPI 适配（2026-10-12 freeze-first 重构＋真走查，见「客户端需求补批」）。
  证据：client/app/screenshot_tool.cpp；client/tests/test_screenshot.cpp。
- [x] T4.5 表情与常用联系人：内置表情、自定义表情包导入、常用表情按频次；常用联系人置顶/星标/最近排序且换机保留。
  证据：client/app/main_window.cpp（表情面板/常用联系人）；server/tests/test_fav.cpp。
- [x] T4.6 通讯录可见性：隐藏部门／成员（白名单例外）、部门限看本部门、敏感人员字段隐藏。
  证据：server/tests/test_visibility.cpp；server/src/store.cpp（org fields/visibility）。
- [x] T4.7 系统集成：托盘、系统通知、开机启动、互踢提示；安装包命名 `MemexClient-x.y.z-*`。
  证据：client/app/main_window.cpp（托盘）；client/tests/test_tray.cpp；install.sh＋install.ps1。
- [x] T4.8 手机端初始化向导（R17／R18）：首启强制服务器地址设置（含连通校验），移动端无任何免登录入口。
  证据：apps/android（InitActivity/RouteGuard/LoginActivity/MainActivity）；单测 39/39 绿＋走查 apps/android/docs/walkthrough-01~07。
- [x] T4.9 主题切换（R19）：亮／暗跟随系统＋手动切换持久化；颜色全部走主题令牌，预留多主题扩展位。
  证据：client/app/theme.cpp（ThemeTokens 23→27 令牌＋ThemeManager）；client/tests/test_theme.cpp（全量 21/21→25/25 绿）。
- [x] T4.10 通知子系统：webhook 接入、三级推送（普通站内/重要强提醒/紧急置顶确认）、个人通知偏好、群公告式通知同级。
  证据：2b8782b（webhook 面）＋3ee6309（CLI）＋8484905（客户端）；server/tests/test_webhook.cpp＋client/tests/test_notify.cpp；docs/src/guide/notify.md。

### 阶段 5：平台适配与验收（A12–A15、A21–A23）

- [x] T5.1 UOS／麒麟适配验证：消息、文件、界面、截图取屏（X11／Wayland 各自验证，不支持环境明确降级提示）。
  证据：docs/src/guide/delivery-matrix.md（UOS/麒麟列如实分级，仅基线档）；.github/workflows/ci.yml。
- [x] T5.2 备份恢复演练与预案文档。
  证据：docs/src/guide/backup-restore.md；scripts/backup_restore_drill.sh＋scripts/files_backup_drill.sh（2026-10-12 实跑 exit 0）。
- [x] T5.3 组件清单与许可核验、命名一致性检查。
  证据：docs/src/guide/compliance.md（vcpkg 六项＋移动端/Node/文档站 17 项补登，许可逐条实证）。
- [x] T5.4 跨地域接入联调与断线补传场景验证。
  证据：scripts/wan_drill.sh；client/engine/collab/tests/test_collab_resend.cpp。

### 阶段 6：平台矩阵扩展（手机端优先序 2026-10-04 用户令：Android 排前 → iOS 进行中·代码-only＋CI 验证 → 其他手机端排后·代码-only＋CI；桌面 macOS 已落地、鸿蒙 NEXT 照原计划不受手机端令影响）

- [x] T6.1 macOS 产物与每日构建三平台安装包：客户端 dmg（arm64 Qt 6.10.3＋vcpkg 静态）＋服务端 dmg、Linux deb/rpm、Windows Inno setup、install.sh／install.ps1 一键安装。
  证据：.github/workflows/nightly.yml；install.sh＋install.ps1。
- [x] T6.2 鸿蒙（HarmonyOS NEXT）客户端（桌面矩阵项，代码-only·待真机验证）：OHOS SDK 交叉编译链（arm64-v8a）、HAP 打包、真机验收（待 DevEco/签名证书/真机解锁）。
  证据：294b886（vcpkg server feature）＋92eebc0（crashpad 闸）＋63a66ab（ohos preset/HAP 集成）；scripts/ohos/build-client-hap.sh＋packaging/ohos/README.md；CI 终态全绿 c9c5e51。
- [x] T6.3 Android 手机端：登录（R17/R18）/会话列表/单聊群聊收发/三级推送横幅震动/移动端适配。
  证据：apps/android（骨架登录→会话收发→通知推送三块）；单测 39→53→57 绿；走查 apps/android/docs/walkthrough-01~21。
- [x] T6.4 iOS 手机端（代码-only＋CI 验证）：登录、会话列表、单聊群聊收发、APNs 三级推送、移动端适配。
  证据：apps/ios（SwiftPM MemexKit＋SwiftUI 壳）；.github/workflows/ios.yml（swift test 38 用例＋xcodebuild 全绿）。
- [x] T6.6 鸿蒙手机版（「其他手机端」首项，ArkTS 代码-only＋CI 逻辑验证；2026-10-04 用户令）：排在 Android／iOS 之后；只写代码不本地验证，验证走 CI 可用平台。注：早前「手机端只做 Android、iOS 不做」口径已被后两道令与同日鸿蒙后置令（卷首注）覆盖——最终口径＝Android 排前、iOS 进行中（代码-only＋CI）、鸿蒙排到队列末尾、只写代码不做测试验证（编译过即可，CI 逻辑腿照跑），提交注明待真机验证。编号空缺如实保留：T6.5 无任何文档引用，不虚设条目。
  进展（2026-10-04 鸿蒙手机版·两块全量代码-only）：`apps/harmony` 从零（ArkTS，HarmonyOS NEXT，stage 模型）——第一块 core 纯 TS 逻辑层（hvigor har @memex/core，Node 可测）：wire.ts 手写 protobuf 编解码（canonical proto3：零值标量/空串不上线、oneof presence 空 message 照写；与 protobufjs 加载 common/proto/memex.proto 单一事实源**双向字节交叉**背书）、frame_codec.ts 镜像 client/core frame.cpp（4 字节大端前缀/ZERO_LENGTH/TOO_LARGE 同口径）、address/route/chat_store/format 移植 Android 同语义、session/client/wire_channel 长连接会话与探测登录（互踢/ACK(seq) 受理/ACK(msg_id) 去重/群=group:N 归会话/KICK 断开/NOTICE 分级）；Node 单测 89 用例全绿（tsc strict＋node:test；wire 双向交叉、假服务端真 socket 全链路：登录/被拒/收发线格式/ACK/去重/群/KICK/三级通知/probe 四态；断言前一律有界轮询消读侧竞态；含 files_client 15）；CI `.github/workflows/harmony.yml`（ubuntu＋Node 22 npm test）绿。交叉验证揪出并修掉两处真实编码器缺陷（body 零值标量与 login_result ok=false 被无条件上线——被 protobufjs fromObject 非 canonical 行为掩盖成假匹配，canonical 对照后暴露）。第二块 ArkTS 壳（entry HAP，依赖 @memex/core file:../core）：HarmonyTransport（@ohos.net.socket 实现 core Transport）、AppState（preferences 初始化态＋内存登录态＋ChatHolder/ChatManager 合并桥）、五页（Index 守卫/InitPage R17 向导 PING→PONG 才落盘/LoginPage R18 attach 一次建连防互踢/MainPage 会话列表未读角标/ChatPage 气泡自动滚底清未读长按复制）、NoticeManager 三级推送（普通=仅站内；重要=横幅+震动不看前后台；紧急=前台确认弹窗排队/后台 isOngoing 常驻+震动，确认收悉撤销）、INTERNET/VIBRATE 权限。**验证边界（如实）**：本机与 CI 均无 OHOS 工具链（hvigor/ohpm/hdc NOT FOUND），Node 逻辑层 CI 全绿；ArkTS 编译腿（hvigor→HAP）待 OHOS runner＋SDK 可用后补，CI 与 apps/harmony/README.md 已如实注记，不造假绿。收口（2026-10-15）：最后一块代码项补齐——后台紧急横幅 wantAgent 动作按钮路径：NoticeManager publishUrgent 构建两个 wantAgent（横幅本体＝拉起应用；「确认收悉」按钮＝confirm_urgent＋msg_id 参数，isOngoing 常驻横幅；wantAgent 异步构建失败回退无按钮常驻横幅不丢提醒本体），EntryAbility onCreate/onNewWant dispatchWant 解参直走 confirmUrgent（撤横幅＋弹队列下一条；冷启动＝新实例通知 id 稳定映射照撤，队列留空即静默）；want 参数键以导出常量共用防字符串漂移。npm test 89/89 绿（逻辑层回归）。遗留＝环境门两项，非代码项：ArkTS 编译腿（本机与 CI 均无 OHOS 工具链）、真机走查（无设备）——apps/harmony/README.md 验证口径表已如实注记（新增 ArkTS 壳运行时 ⛔ 待真机行），不造假绿。

## 三、第二期（单独立项后细化）

- [x] 审批（表单＋流程引擎）· 日程与待办 · 公告与日报周报 · 群工具（机器人／投票／接龙／群任务）。
  证据：2026-10-08~10 七笔收口（审批最小闭环／日报／群工具三件 server／客户端三件 Dialog／审批通知＋周报补写／截止关票／余量三小件 0eaa175＋06bfe1f＋9741e0e）；client/app/tests/test_approval_dialog.cpp＋test_report_dialog.cpp＋test_group_tools_dialog.cpp；ctest 57/57；设计稿 docs/design/审批与日报周报.md＋群工具三件.md。
- [x] 会话审计（查阅权限、查阅日志、合规留痕）。
  证据：az 规则 audit-read＋/files/audit/search|reads（被拒尝试亦落 audit.denied）；client/app/audit_dialog.cpp；client/app/tests/test_audit_dialog.cpp；ctest 51/51。
- [x] 办公室位置图（平面图、拖拽式点位编辑器、搜索定位；默认仅本人楼层可见）。
  证据：office_seats 表＋az office-manage；client/app/office_map_dialog.cpp；client/app/tests/test_office_map_dialog.cpp；ctest 52/52。
- [x] 远程协助（屏幕查看＋受控方授权；受控方显式确认、过程持续可见、全程留痕、按部门开关；不做无人值守）。
  证据：/files/assist/* 生命周期路由＋媒体四路由；client/app/assist_dialog.cpp；client/app/tests/test_assist_dialog.cpp；ctest 53/53。
- [x] 机器人平台（bot 伪账号 `bot:<name>`、token 鉴权、收发/长轮询/细粒度白名单、与通知面同投递链）。
  证据：/bot/send|updates|ack 三路由＋CLI bot 子命令族＋examples/echo_bot.sh；server/tests/test_bot.cpp；ctest 54/54（真挂起长轮询＋白名单两笔追加收口）。

## 四、第三期（依赖 GPU 硬件与一期归档）

- [x] 模型网关（OpenAI 兼容、端点注册、路由、降级、调用审计；归档数据仅本地模型——写死在代码，不做成配置）。
  证据：--model-port 独立进程＋model_calls 审计表；server/src/model_gateway.cpp；server/tests/test_model_gateway.cpp；ctest 55/55。
- [x] 群内智能助手 · 会议纪要生成 · 需求整理 · 智能检索（@助手/@纪要/@整理/@检索 四指令）。
  证据：GroupAssistantWorker（server/src/group_assistant.cpp）；server/tests/test_group_assistant.cpp 九腿；ctest 56/56。

## 五、第四期（建议不自行开发，独立立项）

- [x] 音视频会议·协同文档：不自行开发、独立立项（收口＝集成边界核对：bot 伪账号对接、通知面触达、进程边界、归档红线）。
  证据：docs/src/guide/decisions.md #3（拍板状态留评审组）；本仓库无代码待办。

## 六、持续事项

- [x] 决策清单跟踪：docs/src/guide/decisions.md 十五项待评审组拍板，工程按「建议」项先行。
  证据：docs/src/guide/decisions.md（2026-10-10 关账：#1–#15＋附项全部落定且均采纳建议列，巡检对码无陈旧）。
- [x] 文档站持续更新：界面预览随功能实现逐步替换为实况截图。
  证据：docs/src/index.md（轮播十二席全实况，2026-10-11 关账）；docs/src/guide/prototypes.md；client/tests/test_live_shots.cpp＋test_mode_switch.cpp（实况抓取腿）。
- [x] 组件清单（third_party/）：引入任何第三方组件即登记名称、版本、许可证、引入方式。
  证据：docs/src/guide/compliance.md（2026-10-10 关单：vcpkg 六项＋非 vcpkg 17 项补登）。
- [x] BUGS 跟踪：缺陷登记在仓库根 BUGS.md，只登记不修、修一条关一条。
  证据：BUGS.md（2026-10-11 关账：BUG-001~007 全部已修并验收，台账零待修零挂起）。
- [x] 测试夹具换口重试补齐（CI 竞态加固）：free_port 探活与 serve bind 之间文件面端口被抢的夹具加固。
  证据：16 个起文件面的测试夹具补 3 次换口重试（探活窗 8s→15s）；ctest 66/66 绿。

## R23 文件存储与外网单向传输（2026-10-04 用户拍板，设计=docs/design/文件存储与外网单向传输.md）

- [x] R23-1 存储抽象层（S3 兼容）＋RustFS compose 集成：元数据表/群人两级配额/秒传（哈希去重）；客户端永不直连对象存储、权限只在元数据层判。
  证据：ceca13e；server/src/storage.hpp（S3Storage）；server/tests/test_files_meta.cpp＋test_s3_e2e.cpp（MEMEX_S3_E2E=1 门控，本机 docker+RustFS 全绿）。
- [x] R23-2 群文件＋个人文件（内网全功能）：群成员可读、群主/管理员可管（删/置顶/配额）；个人私有。
  证据：6fd7bee＋7b69240；server/src/authz.cpp＋server/src/files_server.cpp；test_authz＋test_files_api（四视角真 TCP）＋真容器 e2e。
- [x] R23-3 文件助手（自己↔自己）：备忘录文本＋文件传输统一收件箱（含「手机发自己=文件传输」）。
  证据：127ba66（服务端）＋5aa338a（桌面）＋bd37c87（Android）＋f50aa8f/1fcd788（iOS/鸿蒙）；client/app/file_assistant.cpp；server/tests/test_files_api.cpp。
- [x] R23-4 外网单向 uplink＋外网模式客户端：唯一落点=文件助手、两套路由两套 scope（upload-only，下载端点对外网 token 恒 403）、上传全审计、默认关闭显式开启。
  证据：9fe86e8（server 双实例 scope 闸）＋9d316be/9c00c52（Android UplinkClient）；server/tests/test_uplink.cpp（25 断言）。
- [x] R23-5 防护：杀毒扫描钩子＋类型/大小白名单＋外网登录二次验证。
  证据：8ad9987；server/src/scan.hpp（UploadScanner＋UplinkPolicy）；server/tests/test_uplink.cpp（31 断言）。

## R24 群组知识共享：公告 / 备忘录 / 密码箱（2026-10-04，设计=docs/design/群组知识共享.md）

- [x] R24-1 群公告：发布/置顶全员可见/编辑历史留痕/联动三级推送（公告=重要强提醒）。
  证据：b9aa8ab＋d0aa28d；group_announcement_log 只附加表；server/tests/test_group.cpp。
- [x] R24-2 群备忘录：条目化共享知识（标题+正文+代码块）、搜索、修订历史可回滚、权限（管理员维护或开放编辑留痕）；禁止放密码。
  证据：3995c6c＋80f292d；group_memos/group_memo_revisions；client/app/group_memo_dialog.cpp；client/app/tests/test_group_memo_dialog.cpp。
- [x] R24-3 群密码箱：wingman 同款加密（PBKDF2 600k→KEK→AES-256-GCM 包 DEK）、显式解锁、每次查看留痕、掩码展示、授权名单（默认全成员）、成员变更重包裹。
  证据：a20db2e＋45a4ed8＋2230ae6（改密重置验签修复）；client/app/group_vault_crypto.cpp＋group_vault_dialog.cpp；client/app/tests/test_group_vault_dialog.cpp。
- [x] R24-4 UI 边界：备忘录疑似密码提示、密码箱默认掩码、复制密码=显式动作+留痕、导出默认关闭。
  证据：6577c60（looks_like_secret 提示）；掩码/留痕/无导出面随 R24-3 落地；client/app/tests/test_group_vault_dialog.cpp。

## 品牌物料（2026-10-08 用户令：服务器端可配公司品牌，客户端接入后自动换牌；设计=docs/design/品牌物料.md）

- [x] 全链三批（服务端→客户端接入→设置页），公司品牌客户端自动换牌；实现差异已回填设计稿。
  证据：086c99d（服务端 branding 表＋路由）＋11e39f（素材清除路由）＋03fb60e（BrandKit 接入）＋设置页批；client/app/brand_kit.cpp＋brand_settings_page.cpp；ctest 59/59；docs/src/public/branding/before.png＋after.png。

## 客户端需求批（2026-10-08 用户连发令，15 件逐件分批实现；鸿蒙不涉；排期 2026-10-08 报批）

- [x] 批次 A（小件速收，5 件：①③④⑨⑪）。
  证据：④84e1128／⑪协议面 6f2e9d8；各件收口轮 ctest 全绿。
  - [x] ④ 截图发送：截图完成→直接作为图片消息发出（screenshot_tool 截图面接入聊天发送流）。
    证据：2026-10-08 收，图片消息流（84e1128）。
  - [x] ① 富文本·文字颜色：输入选色→HTML color span（受控标记〔#RRGGBB〕方案，线上/存储纯文本、渲染侧 esc 后换色，坏色降级）；输入行「颜色」按钮记忆上次色。
    证据：2026-10-09 收；rich-color.png 验收实况。
  - [x] ③ 消息内图标：输入面图标/表情插入（与②面板同源简版先行）。
    证据：2026-10-09 收（表情面板并入 16 图标组分区，点击落输入框）。
  - [x] ⑪ 个性签名：资料页设置＋字数上限＋会话列表悬浮可见（存储与服务端资料面扩列）。
    证据：2026-10-09 收；proto 面 6f2e9d8 先行；member_profiles.signature＋PROFILE_CMD＋ORG_QUERY 回带。
  - [x] ⑨ 查找联系人：搜索框按昵称/账号/备注（扩到隐藏列昵称/账号/设备id/IP/群名/群成员），模糊+精确。
    证据：2026-10-09 收；server/tests/test_search_fuzzy.cpp＋client/tests/test_local_search.cpp。
- [x] 批次 B（消息面中件，3 件：②⑤⑥）。
  证据：2026-10-09 收口轮 ctest 64/64 全绿；server/tests/test_nudge.cpp＋test_emoji.cpp。
  - [x] ⑤ 发文件/整个文件夹：文件夹=遍历逐个发+作业粒度进度聚合；授权链同步拒 fail-closed 修复（未登录本地拒，不乐观上屏）。
    证据：client/app/main_window.cpp「发文件夹」＋send_folder_to_current_chat；client/tests/test_chat_actions.cpp＋test_direct_file.cpp。
  - [x] ⑥ 振屏消息：协议新类型＋窗口抖动＋提示音＋防刷限频（发送端 10s 冷却＋接收端 2s 限频）。
    证据：协议 NUDGE=53（common/proto/memex.proto）；server/tests/test_nudge.cpp＋client/tests/test_mode_switch.cpp。
  - [x] ② 表情包：收藏/自定义上传 GIF；服务端素材存储＋面板管理（个人素材走文件面 BLOB 落库，收藏随账号）。
    证据：/files/emoji/{upload,list,download,delete} 四路由；server/tests/test_emoji.cpp＋client/app/tests/test_emoji_pack_dialog.cpp。
- [x] 批次 C（协议/存储大件，4 件：⑦⑧⑩⑫）。
  证据：2026-10-09 收口轮 ctest 66/66 全绿；server/tests/test_read_presence.cpp＋test_avatar.cpp＋test_org.cpp。
  - [x] ⑦ 消息回执：送达+已读两级、会话/全局开关、消息旁状态可见（协议扩＋归档扩，气泡 ✓ 已送达/✓✓ 已读）。
    证据：DELIVER_NOTICE=54＋RECEIPT_QUERY/DATA=55/56；server/tests/test_read_presence.cpp＋client/engine/collab/tests/test_read_presence.cpp；client/tests/test_chat_actions.cpp 第⑦腿。
  - [x] ⑧ 聊天记录按日期查询：日期选择器跳转+范围筛选；本地索引＋服务端同步查询；分日分隔线。
    证据：MESSAGE_QUERY=57/MESSAGE_PAGE=58；server/tests/test_read_presence.cpp（时间窗段）；client/tests/test_chat_actions.cpp 第⑧腿。
  - [x] ⑩ 在线时长：服务端上下线流水表＋日/周/月滚动窗统计；资料与会话旁显示。
    证据：presence_events 表＋online_ms_between 深度计数（server/src/store.cpp）；server/tests/test_org.cpp 库级/协议级腿。
  - [x] ⑫ 头像：上传裁剪+类型大小校验+内置默认头像组+各尺寸生成；默认头像组=4 张自绘几何（FNV-1a 账号取）。
    证据：member_avatars 表（版本戳=MAX(ts_ms)）＋/files/avatar/* 路由；server/tests/test_avatar.cpp＋client/tests/test_avatar_client.cpp；client/app/crop_dialog.cpp。
- [x] 批次 D（主题皮肤，3 件：⑬⑭⑮）。
  证据：2026-10-09 收口轮 ctest 66/66 串行绿；client/tests/test_theme.cpp 第⑥⑧⑩⑪⑫节。
  - [x] ⑬ 默认 20 套主题：register_theme 机制批量生成（品牌色/字体/密度成套，亮暗混合；品牌橙恒同）。
    证据：client/app/theme.cpp（ThemeTokens 增排版/密度五字段＋kDefaultThemes 静态生成表）；test_theme 第⑥节＋xvfb 实截三套像素差 100% 成立。
  - [x] ⑭ 自定义主题：面板改色（8 枚中性令牌放行，品牌橙/语义族锁死）＋毛玻璃特效开关（Windows Mica，不支持自动降级）。
    证据：ThemeManager kCustomTheme 槽＋customizable_tokens()；test_theme 第⑧⑩⑪节（拍板 2026-10-09：单一固定槽位/全局单开关默认关）。
  - [x] ⑮ 皮肤包体系：.zip 清单+版本+签名校验导入导出，防坏包硬底线（条目数/大小/路径穿越/压缩方式全门）。
    证据：client/app/theme_pack.cpp（stored zip 自实现＋HMAC-SHA256 验签）；test_theme 第⑫节（篡改拒/硬底线十腿/版本比较）。
- 每件验收截图、分批 commit 写清改动；批次间如有跨件依赖在实现时如实调整排期并注明。

## R25 群工具框架：CI/CD / 打包 / 配置导出（2026-10-04，设计=docs/design/群工具框架.md）

- [x] R25-1 群工具框架：工具白名单动作配置、入群即授权/退群即失、动作留痕（谁/何时/动作/参数/结果）、服务端代理调用骨架。
  证据：f87c031；group_tools＋group_tool_audit 两表；server/tests/test_files_api.cpp R25-1 段；ctest 37/37。
- [x] R25-2 CI/CD 工具：pipeline 状态视图（红绿灯+列表）、点击触发构建/打包（谁触发可回溯）、结果卡片回群。
  证据：5630f0b；group_ci_pipelines/group_ci_runs 两表；client/app/group_ci_dialog.cpp；client/app/tests/test_group_ci_dialog.cpp；ctest 38/38。
- [x] R25-3 打包工具＋配置导出工具（首批动作类示例）：台账重打换备注不增行；导出密文面永不进快照。
  证据：8623dc4；group_pack_artifacts＋/files/group-pack/*＋/files/group-export；client/app/tests/test_group_pack_dialog.cpp；ctest 39/39。
- [x] R25-4 凭据面：外部凭据只存服务端（R24 密码箱同款加密）、客户端零凭据、破坏性动作二次确认。
  证据：13438f1；group_tool_credentials＋gcm_seal/gcm_open（server/src/cred.cpp）；server/tests/test_files_api.cpp R25-4 段；ctest 39/39。

## R26 群服务器工具：agent + 负载状态 + 远程会话（2026-10-04，设计=docs/design/群服务器工具.md）

- [x] R26-1 memex agent：轻量常驻（Linux 起步），注册/心跳/负载上报（CPU/内存/磁盘/负载）；只读采样不开监听口不落盘。
  证据：b8bf382（服务端）＋e26063d（agent）；server/src/agent_main.cpp（201 行实读）；server/tests/test_agent.cpp（真 agent 进程×真 TCP）。
- [x] R26-2 群服务器面板：服务器列表红绿灯＋状态详情（在线=心跳 90s 新鲜度）。
  证据：83b19e8；client/app/group_server_dialog.cpp；client/app/tests/test_group_server_dialog.cpp（真 memex_agent --once 夹具）；ctest 41/41。
- [x] R26-3 远程会话：SSH 起步，一次性短票（60s TTL）＋接入留痕（谁/何时/连哪台/时长）。
  证据：cabaa2a（服务端）＋0f29916（客户端）；group_server_sessions（ticket_hash=SHA-256、重放 409）；server/tests/test_files_api.cpp R26-3 段；ctest 41/41。
- [x] R26-4 凭据面：服务器凭据只存服务端（R24 加密面）、客户端零凭据；memex 只做「看+连」。
  证据：3c81b1d；group_server_credentials；server/tests/test_files_api.cpp R26-4 段；ctest 41/41。

## 权限模型（横切，2026-10-04 用户定调「类似 Linux 账号管理」，设计=docs/design/权限模型.md）

- [x] 权限模型落地：群=组（入群即继承/退群即失/建群需特权/群能力管理员配全/组织架构可见/全程留痕）、群内角色（群主/管理员/成员）——R23-R26 各面统一按此模型实现。
  证据：202d626（建群特权＋group_capabilities）；2026-10-08 收口（OrgGroup 下发＋capability_gate 铺五面 37 路由）；2026-10-10 §八作用域维度（87eb0da 补 q() 第 5 参后真绿）；server/tests/test_authz.cpp 作用域矩阵；docs/design/权限模型.md §5。

## 服务器 agent（R26）现状与缺口（2026-10-12 实读）

背景（用户 2026-10-12）：「所有 agent 我看一点都没有实现」「加入 todo 描述清楚」。本节为逐条实读核对后的现状陈述——已核对 server/src/agent_main.cpp、files_server.cpp、authz.hpp 与 git 历史，与代码不符处已注明；不夸大也不自我贬低：R26 四笔确实落地，但离「堡垒机」目标的三块关键能力（按组授权、命令级/录像审计、流量代理）确实没有。

**一、已实现（有 commit 与测试数，均为历史时点数字；当前全量 ctest 已到 80/80+）**

1. R26-1 agent 注册/心跳/负载上报：memex_agent 201 行（server/src/agent_main.cpp，实读核实），只读采样（/proc/stat 两拍差分 CPU、/proc/meminfo、statvfs 磁盘、getloadavg），不开监听口、不落盘、不执行任何命令（实读核实：无 acceptor/listen、无 exec/system/popen，唯一出网动作=心跳 POST）；commit b8bf382（服务端）+e26063d（agent）；test_files_api R26-1 段＋新 test_agent.cpp（真 agent 进程×真 TCP），当时 ctest 40/40。
2. R26-2 群服务器面板：服务器列表红绿灯＋状态详情（在线=心跳 90s 新鲜度）。
3. R26-3 远程会话（SSH 起步）：60s TTL 一次性短票（kSessionTicketTtlMs=60×1000 实读核实）、库里只存 SHA-256 摘要（明文票只在签发回包出现一次、不进日志）、重放 409；接入留痕=谁/何时/连哪台/协议/时长；commit cabaa2a（服务端）+0f29916（客户端），当时 ctest 41/41。
4. R26-4 凭据面：服务器凭据 AES-256-GCM 只存服务端（commit 3c81b1d），客户端零凭据；memex 只做「看+连」，操作类归 croupier（files_server.cpp 两处注释原文实读核实）。

**二、确实没有的（用户以为缺的就是这些，如实列）**

1. 按组授权：现状=「入群即授权」——群成员都能看服务器列表、都能发起 SSH 会话（session/request 路由只查 tool_call_allowed 群成员身份，实读核实）；authz.hpp 无任何 group-server-* 规则（grep 零命中），没有「只有运维组能连」的细粒度规则。
2. 会话审计深度：只有接入留痕（谁/何时/哪台/时长），没有命令级日志、没有会话录像（grep 零命中）。
3. agent 不执行任何远程命令：只采样＋心跳，不收指令、不开监听口；执行类操作按 R26-4 口径归 croupier——这条是设计取舍，不是没做完。
4. 多 agent 管理面（分组、批量登记、离线告警）未立项：现有路由仅 enroll/heartbeat/list/session×4/credential 七个（实读核实），无批量/分组/告警端点。

**三、与「堡垒机＋按组授权＋日志审计」目标的差距（用户 2026-10-12 目标）＋分批建议**

- 差距对照：按组授权=缺；命令级/录像审计=缺；凭据不外露=已有（一次性短票＋服务端凭据加密）。
- 架构级前提（如实注明）：当前 SSH 会话是「客户端本地直连目标机」（服务端只签发票、不代理流量），命令流根本不经过 memex——所以命令级日志/会话录像的前提是先把流量改走 memex 代理面（堡垒机形态）或 agent 执行面（归 croupier 口径），这是架构取舍不是加个日志的事。
- 分批建议（建议，非定案）：批1=按组授权最小面（az 加 group-server-* 规则：server:view/server:session 两动作起步，用现有群当组、某群=运维组即可收口）；批2=SSH 流量代理面（memex 做 jump host，流量过服务端才有审计对象）；批3=审计深度（按拍板结果做命令日志或录像）。
- 待拍板三条：①agent 审计取命令日志还是会话录像（或都要）；②是否新增 group-server-* az 规则（还是沿用入群即授权＋群角色两档）；③用现有「群」当组还是独立角色层。

## R27 个人任务清单与外部工具接入（2026-10-04，调查分析=docs/design/个人任务清单与外部工具接入.md）

- [x] R27-1 本地 todolist：自建/完成/提醒＋他人分配（谁能分配按权限模型）；提醒=30s 轮询到点经通知中心（回执只提醒一次）。
  证据：tasks 表＋az task-assign；client/app/task_dialog.cpp；client/app/tests/test_task_dialog.cpp；ctest 44/44（2026-10-10 关窗后台常驻化＋日历视图两笔追加）。
- [x] R27-2 Provider SPI＋L1 跳转框架：capabilities 声明制（L1 跳转/L2 只读/L3 双向），detailUrl 必带。
  证据：client/engine/task/task_provider.cpp（SPI＋TaskProviderRegistry 内置预设六件）；client/engine/task/tests/test_task_provider.cpp；client/app/tests/test_task_template_settings.cpp。
- [x] R27-3 首批 provider：GitHub Issues、飞书任务（Task v2）、钉钉待办——各做到能力声明深度；凭据加密落盘＋设置面。
  证据：client/engine/task/{github,dingtalk,feishu}_provider.cpp；test_github/test_dingtalk/test_feishu_provider.cpp；client/app/task_provider_store.cpp（PBKDF2 600k→AES-256-GCM）＋task_provider_settings.cpp。
- [x] R27-4 二批 provider：Jira/Linear（按需启动）；禅道只接 L1 跳转；Teambition 不接。
  证据：registry 内置 L1 预设（Jira/Linear/禅道模板即挂即用）；client/engine/task/tests/test_task_provider.cpp 挂载解析腿。
- [x] R27-5 服务端 provider 集成与推送：定时拉取（GitHub 现接）→落 tasks 表→经既有长连接推在线客户端；拉取开关默认关（三道闸零外呼）。
  证据：--task-pull-secret 三道闸＋drogon HttpClient（进程内既有依赖）；server/src/task_pull.cpp；server/tests/test_task_pull.cpp＋admin/tests/test_admin_api.cpp ㊹。
- [x] R27-6 任务类型与时间字段与看板：type（服务端配置，初始四类）＋start_ms＋status；看板=同一批数据另一视图，三端同源。
  证据：tasks 四列迁移＋task_types 表＋/files/tasks/status 路由；client/app/task_dialog.cpp（看板三列）＋apps/android TasksActivity；server/tests/test_files_api.cpp R27-6 段；ctest 81/81。
- [x] R27-7 领导视角：下属=R14 汇报关系直接下属（manager==account 反查，只一级跨级关闭）；az 只认直接上级（无 org-admin 兜底）；默认开；负荷三数写死；A 式泳道。
  证据：az task-team-read＋GET /files/tasks/team（泳道拉取）；client/app/task_dialog.cpp（领导视角开关＋泳道）；server/tests/test_files_api.cpp R27-7 段＋client/app/tests/test_task_dialog.cpp；ctest 81/81。
- 六条拍板回执与五条补充令均定案（2026-10-12），已定案表全文见设计 §6.6，无遗留待拍板项。

## 平台化改造（2026-10-04 用户计划书，蓝图=docs/design/基础平台化改造计划（身份-组织-授权-归档）.md）

- [x] 平台-1 授权统一服务 AuthorizationService：subject/action/resource/context→ALLOW/DENY+reason；数据过滤（Filter→Allowed Records）；冲突规则（Explicit Deny>Allow>Inherited>Default）写死。
  证据：09a456a（骨架 6fd7bee 随 R23-2 立）＋filter_allowed 行级过滤；server/tests/test_authz.cpp＋test_files_api.cpp 平台-1 段。
- [x] 平台-2 Identity 模型补全：Credential 独立（Password/Token/Certificate/SSO/Device 预留）、Session 加 device_id/expire/logout_reason、IdentityBinding。
  证据：16c5aa3；credentials/sessions/identity_bindings 三表＋POST /files/logout；server/tests/test_files_api.cpp 平台-2 段。
- [x] 平台-3 Organization 与授权拆开：Account 拆 Membership/RoleAssignment/ReportingLine 三关联（一人多部门/多角色/临时代理）。
  证据：org_memberships/org_role_assignments/org_reporting_lines 三权威表＋effective_roles；server/tests/test_files_api.cpp 平台-3 段。
- [x] 平台-4 Archive Event Sourcing：Message immutable＋MessageEvent append-only（created/delivered/read/recalled/edited）；重投可重建。
  证据：bfd28bb；message_events＋rebuild_messages_from_events 重投演练；server/tests/test_archive.cpp 平台-4 段。
- [x] 平台-5 留存策略：废除「永久留存」口径→Retention Policy（30d/6m/1y/3y/Indefinite）；删除=Retention Purge（双人审批高风险）。
  证据：8a4d1cd；retention_policies/retention_purges＋三道闸；server/tests/test_archive.cpp 平台-5 段。
- [x] 平台-6 审计独立角色：SecurityAuditor≠SystemAdmin（SystemAdmin 不自动可读消息）；审计本身留痕（含被拒尝试）。
  证据：5fcd97c；--as 持证校验＋audit.message.search/export 规范名；server/tests/test_archive.cpp 平台-6 段。
- [x] 平台-7 降级显式化：DEGRADED 态「未归档通信」全面标识（顶部横幅/会话标记/输入框提示/恢复补传规则）。
  证据：a79aa4d；collab_degraded_（登录尝试不可达即降级）；client/tests/test_mode_switch.cpp 降级腿四断言。
- [x] 平台-8 Direct 安全：发现只做发现，TCP 上 Device Identity+握手+X25519+AEAD（消息/文件加密边界）。
  证据：ccee748；client/engine/direct/secure_channel.cpp（Ed25519 长期密钥＋每连接 X25519→HKDF→AES-256-GCM＋TOFU 定针）；client/engine/direct/tests/test_direct_secure.cpp。
- [x] 平台-9 协议幂等：客户端至少一次+服务端 message_id 去重；本地 Sync State 状态机（LOCAL/PENDING/SENDING/SERVER_ACKED/ARCHIVED/FAILED）。
  证据：c10fd77；sync_state 列＋set_sync_state_archived；client/engine/collab/tests/test_collab_resend.cpp 平台-9 段。
- [x] 平台-10 文件授权：FileAuthorization 走统一 AuthorizationService（收发/下载/跨部门/再转发四问）。
  证据：4346ee7；FILE_AUTHZ=49/FILE_AUTHZ_RESULT=50＋allow_cross_dept_file/allow_forward_file 策略；client/engine/collab/tests/test_file_authz.cpp 六腿。
- [x] 平台-11 远程协助 Security Domain 模型：consent/audit 两红线＋粒度权限 view/keyboard/mouse/clipboard/file_transfer。
  证据：50e10a9；assist_policies/assist_sessions/assist_audits 三表五态；server/tests/test_archive.cpp 平台-11 段 26 腿。
- [x] 平台-12 P2 起按计划书 P0-P4 分阶段推进（高级权限等二期，Authorization 框架一期已做）。
  证据：202d626（建群需特权＋group_capabilities）；2026-10-10 队列对账三段全清（§八作用域＋R27＋二期功能）。

## 客户端需求补批（2026-10-11 用户连发令）

- [x] 截图功能真实可用性排查修复（用户 2026-10-11 verbatim「memex 的截图也没有实现」）：三个实码 bug 修复＋真走查留证。
  证据：client/app/screenshot_tool.cpp（freeze-first 重构＋running_ 自愈＋hideEvent 取消）；client/tests/test_screenshot.cpp；Xvfb+xdotool 真走查（对端图片气泡 541×181 可见、双侧「文件已送达」）。
- [x] 原型图逐屏核对（用户 2026-10-11 verbatim「核对下原型图，很多都对不上，ui都对不上啊」「把核对不对的 整理到 todo.md 中」）：14 稿全对过，差距 32 条落本节；只整理不改码，修复另批。
  证据：docs/design/prototypes/（desktop 10＋mobile 4）↔ 实现逐屏比对（布局/字段/控件/文案/主题令牌）；差距清单见下。
  - **差距清单**（P0→P2，屏名｜差距｜原型 / 实现）：
    - P0 聊天共性｜撤回无任何显示：recalled 标记在库但渲染不读（按原文照常显示），横幅却承诺「撤回仅改显示，服务端保留原文与撤回记录」｜desktop-chat.html:126 / main_window.cpp:3666＋local_store.hpp:28
    - P0 组织架构屏｜三栏工作区（部门树＋成员卡＋资料侧栏）实现为 560×620 平铺树对话框（本版有意走对话框，原型深度未跟进）｜desktop-org.html:28 / main_window.cpp:2594＋rail_nav.cpp:32
    - P0 文件传输屏｜整屏未实现：原型传输管理器（通道侧栏/统计头/tabs/进度卡/暂停取消/打开目录），实现为「文件助手」收件箱对话框——传输管理器概念未立项｜desktop-files.html:27 / file_assistant.cpp:42
    - P1 聊天共性｜文件无卡片气泡：收发展示为系统行「[文件] …」（群文件卡片同缺）｜desktop-chat.html:104 / main_window.cpp:1017
    - P1 聊天共性｜无引用/回复（quote 块三稿都有）｜desktop-collab.html:143 / main_window.cpp:188
    - P1 聊天共性｜输入工具条：文本按钮（发文件/发文件夹/截图/表情/颜色/振屏/按日期）vs 原型图标，无 @ 选择器入口｜desktop-collab.html:171 / main_window.cpp:703
    - P1 聊天共性｜@ 成员选择器缺失：@ 高亮渲染有、输入 @ 唤起成员列表无｜desktop-group.html:137 / main_window.cpp:3881
    - P1 聊天共性｜归档起点 pill「归档自 HH:mm」常规协作会话无（仅跨态行有文本版）｜desktop-collab.html:63 / main_window.cpp:3629
    - P1 聊天共性｜群已读计数「已读 12/23」无（回执仅 ✓ 已送达/✓✓ 已读）｜desktop-group.html:96 / main_window.cpp:186
    - P1 协作列表屏｜chat-head meta 无部门/职务/直属上级；chat-head actions（语音通话/常用联系人/会话设置）与「发起单聊」按钮无；列表行无头像列、星标/常用联系人概念未实现｜desktop-collab.html:33 / main_window.cpp:572
    - P1 直连列表屏｜在线设备/离线设备分组缺失（平铺）｜desktop-direct.html:47 / main_window.cpp:3333
    - P1 群聊屏｜群成员/群文件 infopanel 无（会话内无面板）｜desktop-group.html:150 / main_window.cpp（无对应）
    - P1 组织架构屏｜部门/成员搜索无；成员资料侧栏（头像/职务/徽章/汇报链路/办公位置/手机隐藏/发消息/设为常用联系人）无；「发起群聊」入口与部门可见性说明（🔒机要室）无｜desktop-org.html:29 / main_window.cpp:2594
    - P1 归档检索台｜管理台工作区实现为「会话审计」对话框；结果平铺文本行无五列表格/查看上下文/撤回事件/导出操作列、无命中计数与导出留证｜desktop-search.html:12 / audit_dialog.cpp:26
    - P1 移动·聊天｜无气泡（平铺两行文本，无左右分侧/底色/头像）；chat-head 无 sub/头像；输入区无归档提示行、无表情/截图/文件入口｜mobile-chat.html:31 / ChatActivity.kt:138＋activity_chat.xml:6
    - P1 移动·会话列表｜列表行无头像/badge pill/归档标记；无搜索框/tabbar/通讯录页；「发起」=手输对端账号｜mobile-sessions.html:18 / MainActivity.kt:181
    - P1 移动·初始化｜连接成功无回显（已连通 badge/服务版本/组织/延迟 hint）｜mobile-init.html:31 / InitActivity.kt:77
    - P2 聊天共性｜foot 归档 pill（已归档/当前消息：不归档/群消息全量归档）无｜desktop-collab.html:180 / main_window.cpp（无对应）
    - P2 聊天共性｜每条气泡名带「· 协作·已归档/直连·仅本机」长 tag（原型气泡干净、归档态由 foot pill 表达）｜desktop-collab.html:180 / main_window.cpp:3876
    - P2 聊天共性｜单行输入：QLineEdit 无多行（原型 Shift+Enter 换行）｜desktop-direct-chat.html:165 / main_window.hpp:473
    - P2 直连列表屏｜搜索框文案差、网段提示行（自动发现 /24 · UDP 2425）无、空态文案与「✕ 关回列表态」说明差｜desktop-direct.html:35 / main_window.cpp:3306
    - P2 群聊屏｜群公告独立横幅无（塞 meta 前缀）；群主标注无；chat-head actions（群公告/群文件/成员）无｜desktop-group.html:68 / main_window.cpp:3565
    - P2 归档检索台｜过滤器少人员部门选择/精确匹配开关；连接行需手填；「已撤回 · 原文留存」简化为「（已撤回）」灰字｜desktop-search.html:24 / audit_dialog.cpp:62
    - P2 通讯录屏｜判「原型未更新」（rail 批 2026-10-10 已把好友概念并入消息列表＋组织架构对话框）：星标好友/「23 / 38 在线」计数/好友须同意 hint 无对应功能面｜desktop-friends.html:21 / main_window.cpp:3194
    - P2 跨态屏｜跨态说明实现为聊天区顶部系统行（口径一致），原型为 head 下横幅块；「服务端无记录」尾注无｜desktop-chat.html:67 / main_window.cpp:3629
    - P2 移动·初始化｜单页表单 vs 三步 wizard——R17 有意单页强制向导，判「原型超前」；按钮文案「校验并保存」vs「下一步 · 登录账号」｜mobile-init.html:18 / activity_init.xml:14
    - P2 移动·聊天｜placeholder「输入消息…」vs「发送消息…」；无分日分隔线；撤回置灰 40%｜mobile-chat.html:89 / ChatActivity.kt:146
    - P2 移动·我｜设备台账/登录记录/留存策略三项如实「未接入」（协议未开——诚实降级非偏差）；账号卡无二维码；无 tabbar｜mobile-me.html:43 / MeActivity.kt:26
    - P2 主题令牌｜品牌橙恒同 ✓、rail-w 60 ✓；其余整套有意偏离（对比度门槛 ≥3:1＋20 套主题体系自定值）——需拍板原型回抹或实现回对｜prototypes.css:4 / theme.cpp:46
  - **live-* 判定**（实现过时 vs 原型未更新，分列不混）：①判「实现过时（原型即最新意图，实现未跟进）」——live-chat/live-collab/live-group/live-direct 的气泡面各条（撤回显示/文件卡片/quote/@ 选择器/meta 信息量）、live-org（三栏工作区未实现）、live-search（管理台形态未实现）、live-files（传输管理器未立项，截图记录的是聊天内文件传输）、mobile-live-chat/mobile-live-sessions（气泡/tab/头像行）、mobile-live-init（连接成功回显）；②判「原型未更新（实现有意演进后未回写原型稿）」——好友列表屏（rail 批并入消息列表＋组织架构，desktop-friends.html 仍画独立通讯录屏）、气泡内「· 协作·已归档/直连·仅本机」tag（T2.4 双态合并展示为原型后增）、输入行 颜色/振屏/按日期/发文件夹（需求批⑥⑧后增）、主题 20 套超集（原型仅亮暗两套）；③非偏差——mobile-me 三项「未接入」为诚实降级（原型值为演示数据），live-org/live-search 截图如实记录对话框/CLI 现状、截图本身不过时。

## 服务器部署批（2026-10-11 用户令，verbatim：「提供部署到docker 中，需要ci 配置，然后我cloudfare tunnel 提供tcp 域名连接，这个服务器地址，我默认为server.memex.cuihairu.site」）

- [x] Docker 部署交付：服务端出 Dockerfile＋compose（数据卷持久化、端口映射、环境注入）；`docker compose up` 起后客户端可正常登录；对外 TCP 端口 8088（与用户 tunnel 回源对齐），webhook 24361。
  证据：server/Dockerfile（deps/build/runtime 三阶段，EXPOSE 24360/24361/24561）＋docker-compose.yml（rustfs+server+admin 三服务，8088:24360）；实机走查（真 UI 登录 logins 台账 ok＋webhook 200/401/400＋restart 数据不丢）。
- [x] CI 镜像流水线：workflow 构建服务端镜像并推 ghcr（latest＋sha-<完整 sha>），构建绿才推；不打 tag、不建 Release（版本发布红线，nightly/滚动 latest 除外）。
  证据：.github/workflows/docker.yml（memex-server＋memex-admin 双镜像 job）；run 38163938761 绿 29m6s，产出镜像 sha-2f6983ef 本机 docker pull 实拉部署。
- [x] Cloudflare tunnel TCP 对接（用户侧已配好 2026-10-11）：映射 server.memex.cuihairu.site→tcp://192.168.5.5:8088（用户 Cloudflare 侧代管）；端口对齐＋接入文档（回源改指、状态自查三步）。
  证据：docker port 24360→0.0.0.0:8088＋LAN nc accept＋真客户端经 192.168.5.188:8088 登录绿；README.md/README.zh.md「公网接入：Cloudflare tunnel（TCP 8088）」节。
- [x] 客户端默认服务器地址：默认值设为 server.memex.cuihairu.site（首次启动向导与默认配置同源）；地址始终可改——用户配置优先，常量仅无配置兜底。
  证据：client/app/collab_defaults.hpp（21 处 fallback 字面量收编，files_port 分叉 24362→24561 统一）；走查四腿 /tmp/memex-walk423/；ctest 75/75 绿。
- [x] 域名强制解析配置（局域网直连）：提供域名→内网 IP 覆盖配置，默认不覆盖（公网场景走 tunnel 正常解析）。
  证据：collab_defaults.hpp apply_host_rules()＋resolve_conn_host()＋设置菜单「连接设置…」对话框（13 个工具对话框 connect_to 全过解析器）；client/tests/test_host_override.cpp 8 腿；走查（域名被规则解析成 192.168.5.188 登录成功）/tmp/memex-walk424/；ctest 76/76。
- **验收（硬口径）**：①本机 `docker compose up` 真实起服务 → 客户端用默认地址 `server.memex.cuihairu.site` 经 tunnel **真连上并登录成功**（连接日志/截图留证）；②CI run 绿且 ghcr 镜像可拉；③数据卷重启不丢（重启容器后账号/消息仍在）；④无 tunnel 环境下改回内网地址仍可登录（证明默认值不是硬依赖）。测试绿才 push，commit 英文简短。

## admin 管理后台（2026-10-11 用户令：整理现有管理功能面并立项网页化）

> 现状盘点（扫代码非臆造）：`admin/` 仅 README 占位（规划「第一期第四阶段起建设」）；管理能力现散布在 **90 个 `/files/*` HTTP 路由 + 19 个 CLI 子命令**（account/org/policy/audit/messages/retention/webhook/bot/device/group/storage/assist/model/branding…共 40+ 二级命令）里，**全部没有 UI**。本批=把这些能力整理成网页后台的建设范围：

- [x] 部署口径（用户已配映射 2026-10-11）：后台对外 admin.memex.cuihairu.site→http://192.168.5.5:8089（HTTP tunnel）；后台容器监听 8089，随 Docker 批交付。
  证据：docker-compose.yml admin 服务（8089:8089）＋admin/Dockerfile＋.github/workflows/docker.yml memex-admin 镜像 job＋README 域名接入/首号 bootstrap 文档。
- [x] 后台骨架（技术选型=drogon）：独立 admin 进程监听 8089——不动现有 FileServer 与协议；登录与角色门（org-admin/auditor 分权，判权复用服务端 az 规则不自造）、导航、会话超时、静态资源与 CSRF 轻闸。
  证据：admin/src/admin_app.cpp（Drogon 1.9.13 独立进程；判权=server/src/authz.cpp 同一引擎镜像注册）；admin/tests/test_admin_api.cpp 19 腿。
- [x] 账号与组织：建号（基础角色白名单）、部门树、成员归属、资料建档（环拒绝）、角色授予/吊销（时间窗即时生效）。
  证据：admin/tests/test_admin_api.cpp（波次 1 面）＋admin/www/ 账号与组织页。
- [x] 设备与登录台账：设备列表/启停/责任人/解绑＋登录台账 UI（identity 面留波次 2）。
  证据：admin/tests/test_admin_api.cpp＋admin/www/ 设备与登录页。
- [x] 策略下发：policy 四开关（含部门行、文件跨部门/转发）下发与回读；retention 档位与 purge 双人审批红线（审批人≠执行人、理由必填、台账落库）。
  证据：admin/tests/test_admin_api.cpp＋admin/www/ 策略页。
- [x] 归档检索与导出：检索（账号/关键词/时间窗/对端）＋JSONL 导出＋查阅日志（含被拒留痕）；必须走 auditor 证（平台-6 红线：org-admin 不自动可读消息）。
  证据：admin/tests/test_admin_api.cpp（auditor 红线腿：org-admin 检索 403 且落 audit.denied）＋admin/www/ 检索页。
- [x] 通知与集成：webhook create/list/revoke（token 仅一次显示）、三级通知偏好只读展示、bot add/remove/token、model endpoint（13 条路由）。
  证据：admin/tests/test_admin_api.cpp ⑳~㉓＋admin/www/ 通知与集成页；ctest 76/76 全绿（显式退出码）。
- [x] 文件与配额：/files/manage/delete|pin|quota、两级配额（群+个人）读写、storage 状态只读探活（6 条路由）。
  证据：admin/tests/test_admin_api.cpp ㉔~㉗＋admin/www/ 文件与配额页；ctest 76/76 全绿。
- [x] 群组管理面：群台账与能力、群 CI、群服务器与远程会话、密码箱、备忘录回滚、投票/群任务、工具凭证（19 条路由；工具凭证只有掩码清单）。
  证据：admin/tests/test_admin_api.cpp ㉙~㊱＋admin/www/ 群组管理面页；ctest 76/76 全绿。
- [x] 业务流程页：审批列表与决定、日报团队聚合、办公室位置图管理、远程协助台账与策略、任务与备忘录（14 条路由；成员红线动作一律只读——后台不代写不代发）。
  证据：admin/tests/test_admin_api.cpp ㊲~㊶＋admin/www/ 业务流程页；ctest 76/76 全绿。
- [x] 品牌与外观：branding 读改/素材上传回读（原始字节 POST）、表情包元数据清点（不取字节）、头像代读（8 条路由）。
  证据：admin/tests/test_admin_api.cpp ㊷＋admin/www/ 品牌与外观页；ctest 76/76 全绿。
- [x] 健康与运维：健康页（admin 自身 ok/db open/uptime；files 面探活经 MEMEX_FILES_URL 显 up/down）＋查阅日志只读视图。
  证据：admin/src/admin_app.cpp＋admin/www/ 健康页。
- **验收（硬口径）**：真实部署起后台 → org-admin 登录完成每块至少一条端到端操作（造数据 → 页面操作 → API/库回读一致）；auditor 只读面**改不了数据**（越权 403 与 CLI 同口径）；截图留证；测试绿才 push
- **边界**：权限不自造（复用服务端 az）；消息内容不向 org-admin 开放（平台-6 红线）；本批只做后台页面，不动既有 CLI/协议

## 可靠性与安全边界（2026-10-11 外部评审输入：逐项对码后立项，非全盘照收）

> 外部评审给了一份 P0–P2 改进清单；其中相当一部分**已在仓内落地**（统一 az 引擎 `authz.cpp`＋default-deny＋403/审计留痕测试、留存双人审批＋独立台账表、消息幂等去重补传 `test_collab_resend`、备份恢复文档 `backup-restore.md`、撤回只标记/原文保留、审计留痕只附加）。以下只立**对码后确认缺**的项；优先级是评审口径，排序仍由用户定。

- [x] 服务端授权专项审查（评审 P0-1）：逐敏感操作盘点「有服务端 az 检查＋明确拒绝路径＋回归测试」三件套；补漏四处。
  证据：docs/src/guide/authorization.md（协作/文件/后台/CLI 四面盘点表）；server/tests/test_authz_audit.cpp 五腿。待用户拍板三条如实挂起（账号禁用控制、停用设备是否踢存量会话、files 令牌是否查设备闸）。
- [x] 消息与归档一致性故障注入矩阵（评审 P0-2）：生产最小修复＝archive_and_resolve 返回 std::optional（不再谎报受理）＋六腿矩阵。
  证据：server/tests/test_msg_faults.cpp（断网重发洪峰／服务端重启补投／重复 ACK 幂等／撤回与补传交错／DB 写失败／进程崩溃恢复）；ctest 79/79 绿。
- [x] 备份恢复扩展：对象存储字节入预案（评审 P0-3 对码缺口）＋files verify 核对工具＋干净机器恢复演练。
  证据：docs/src/guide/backup-restore.md（双资产叙事）；server/tests/test_files_verify.cpp 六腿；scripts/files_backup_drill.sh（2026-10-12 实跑 exit 0，库 626688B＋卷 7522B 成对，孤儿 0）；ctest 80/80 绿。
- [x] 归档完整性校验清单（评审四·远期加固）：message_events 分块封账＋链式摘要账本（archive_chain，不引入区块链）＋CLI archive status|seal|verify。
  证据：archive_fold_range 同事务封账＋校验共用折叠；server/tests/test_archive.cpp 扩八腿（直连第二 sqlite 连接当「DB 级管理员」篡改手）；ctest 81/81 绿。
- [x] 文件生命周期口径对齐（评审五·文档真缺口）：文件双通道口径（聊天文件点对点直传＋文件面字节经服务端代理），改「始终」过绝对表述。
  证据：docs/src/guide/file-storage.md（发布站口径页）；docs/src/guide/product.md＋report.md 同步对齐；文档项无代码改动。
- [x] LDAP / AD 可选企业身份后端（2026-10-11 外部建议三步走）：可插拔认证后端，不改轻量部署与免登录直连的独立信任模型；fail-closed＋组→角色显式映射＋目录禁用踢下线＋密码不明文。
  证据：server/src/ldap.hpp＋ldap.cpp（自写最小 BER 客户端，零新增外部依赖）；packaging/ldap.env.example；server/tests/test_ldap.cpp 16 腿假目录；ctest 82/82 全绿；docs/src/guide/authorization.md「企业目录认证（LDAP/AD）」节。
- [x] 交付验收矩阵（评审八-5）：平台（Win/UOS/麒麟/macOS/Android）×通信模式×账号状态×网络状态×权限角色的组合表，如实分级「实测通过／编译验证／仅代码存在」——CI 绿≠全平台可交付。
  证据：docs/src/guide/delivery-matrix.md（240 格：✅实测 40／🔧编译验证 14／⬛仅代码存在 153／—设计排除 33；实测 40 格零一格来自 Win/UOS/麒麟/macOS 本机）；文档项无代码改动。

## 飞秋/IPMSG 兼容（2026-10-11 用户令：同网段有飞秋客户端时 memex 进其在线表、单聊互通）

- [x] 飞秋/IPMSG 兼容（旁路，默认关）：同口 UDP 2425 首字节分流、GBK/CP936 优先（GBK 外回退 UTF-8＋UTF8OPT）、单聊互通、发现面 BR_ENTRY/ANSENTRY/BR_EXIT。
  证据：client/engine/direct/ipmsg.hpp＋ipmsg.cpp；client/engine/direct/tests/test_ipmsg.cpp；docs/src/guide/feiq-compat.md。已知边界（诚实注记）：真飞秋客户端实测未做（interop 证据止于协议对拍＋mock 对拨）；文件传输/加密/离开自动应答不实现。

## 群组两项（2026-10-12 用户令：建群主题/目的＋群记录导出）

> 设计=docs/design/建群主题与群记录导出.md；新节不并入 R24（不拆散既有编号）。

- [x] 群组-1 建群询问主题/目的（定案①已拍）：建群弹窗一屏＝主题＋目的，都可留空直接建群（弹窗仍默认弹出）；空主题＝群名下方不显示该行；群主/管理员可后期编辑；桌面/手机/admin 三端同源。
  证据：groups 加 topic/purpose＋create_group/group_set_profile＋GroupInfo 三字段；server/tests/test_group.cpp 扩库级段（含 ALTER 迁移）＋协议级腿；admin/tests/test_admin_api.cpp ㉙ 扩回读；ctest 81/81 全绿。
- [x] 群组-2 群记录导出（定案②③④已拍）：导出固定三段＝参与人（角色＋加入时间）＋时间范围（含建群时间）＋聊天记录（区间内）；无 AI＝普通文档、有 AI＝加总结章节（R11 未接线必须自动降级注明）；权限定案②＝全员；格式定案④＝Markdown；记录导出行为落 audit_reads。
  证据：GET /files/group-record-export（keyset 游标分页 has_more/truncated、权限 is_group_member 现查 403）；server/tests/test_files_api.cpp 群组-2 段＋test_group.cpp 库级腿；admin/tests/test_admin_api.cpp ㊮ 扩审计可查；ctest 81/81 全绿。

**已定案表（2026-10-12 用户拍板回执，待拍板清单关闭）**——全文见 docs/design/建群主题与群记录导出.md §四：
① 建群主题可以跳过（弹窗仍默认弹出，主题与目的都可留空直接建群；空主题＝群名下方不显示该行）；
② 导出权限全员（群内任何成员可导该群记录；路由层 az，非群成员一律不可导）；
③ 记录导出行为（谁/何时/哪个群/区间/条数/是否含 AI 总结，admin 后台可查）；
④ 默认格式 Markdown（.md）（docx 须注明转换工具与依赖，无依赖只出 Markdown）。

- [x] 缺陷（反复未修）：启动界面右侧大片空白，不像正常聊天软件（来源：用户 2026-10-14，第三次反馈）
  - 用户参照：**正常聊天软件启动即进入联系人/会话列表那种长方形布局**；**当前启动后右侧是空的**，视觉上像正方形
  - 已排除：窗口尺寸不是主因——`66100a7` 已把默认尺寸改为 `1280x800`（`main_window.cpp:318`），是宽屏比例。所以问题在**启动时的视图状态**，不在窗口几何
  - 强烈怀疑：**启动时没有选中任何会话**，右侧面板因此渲染为空占位 → 右侧整片空白
  - 验收：启动后的默认视图**不允许出现大片空白**。要么默认选中最近会话，要么未选中时右栏改为**列表占满**（如微信/QQ 那种启动即列表）。修复须**真实启动验证并截图对比**，不接受"改完了"
  - 红线：这是第三次反馈。**先真实启动看当前实际渲染**，再动手；改完必须自己截图确认右侧不再空白，**不要只改代码就宣称完成**
  - 证据：根因＝构造 `close_chat(false)` 隐藏会话面板后，中央布局尾部 `addStretch(1)` 吸收全部余量（main_window.cpp:853 旧码）→ 右侧整片空白。修复 `2e56c92`：选「列表占满」口径——移除尾部 stretch，列表列在无选中态 Expanding 占满 rail 之外全宽（微信/QQ 启动即列表；顺带修好最大化列表态死空间），关会话钳 320（2026-10-11 令）不动。Xvfb 真机启动前后截图：/tmp/defect3/shots/before_root.png → after_fresh_root.png（右侧空白→列表占满）；window_layout 新腿⑦＋全量 ctest 83/83 绿

- [x] 缺陷（反复未修·续前条）：启动无会话时窗口仍是宽横窗，未坍缩成列表态窄长方形（来源：用户 2026-10-15，多次反馈「说了无数次」）
  - 根因（2026-10-15 实测定位，两条通道）：`66100a7` 反转布局序（默认 1280×800）后——①构造 `close_chat(false)` 显式跳过启动坍缩；②单键 `window_geometry` 存档恢复把上次会话态宽窗直接盖回无会话启动态（Xvfb 复现：探针落 960×640 可复存档 → 启动即 960×640）。前条 `8a84f85` 只登记 todo 未实测，启动路径没修＝假关闭
  - 修复（重派照抄 Telegram Desktop 机制，不自造）：①最小尺寸随布局态 `apply_state_minimum`——列表态 320×420（rail 60＋列表列 260），会话态 60＋260＋380（＝TG columnMinimalWidthLeft 260／columnMinimalWidthMain 380 之和口径，不用布局 minimumSizeHint：聊天输入行按钮串把自然下限顶到 892，过宽且反锁用户既有窄会话窗）；②几何存档按态分键 `window_geometry_list`／`window_geometry_chat`（TGWindowArchiver 按态归档同构），启动只取列表档，会话档只归 `open_chat` 经 `restore_chat_geometry` 恢复（chat 档→旧单键档→默认 1280 兜底）；③构造即 `close_chat()` 坍缩 320；④旧单键存档迁移归会话态（旧版只存过 ≥480 宽窗，宽档必属会话态）。布局态判据用显式 `chat_open_` 旗（离屏未 show 窗里子件 isVisible 恒 false，按 isVisible 分键会把会话档写进列表键）
  - 证据：Xvfb :99 真机启动前后实测（/tmp/geo_shots/）——修复前：首次启动 1280×800、有 960×640 存档启动直接 960×640；修复后：①首次启动（空配置）320×800，列表占满右缘无空白（1_fresh_startup_root.png）②同平台探针落双档（list 320×600＋chat 960×640）后重启仍 320×600 列表态窄窗、chat 档不盖回（4_archive_startup_root.png）③点开好友会话恢复 960×640 会话档（5_archive_open_chat_root.png；空配置点开默认 1280×800 见 2_open_chat_root.png）④关会话回列表态 320×640（6_archive_close_root.png）。test_window_layout 重写九腿（构造坍缩／开关往返／按态存档往返／会话档隔离回归／旧单键迁移归会话态／列表占满／托盘隐藏态退出）＋QSettings 双格式路径重定向隔离（只重定向 IniFormat 管不住主窗内部 NativeFormat 构造，存档漏 ~/.config 跨跑串味实录）；ctest 84/84 全绿

- [x] 消息已读状态：灰色勾（来源：用户 2026-10-14）
  - 要求：消息被对方读过之后显示**灰色的勾**，即**已读状态**
  - 要定清楚（别只改 UI）：
    1. **状态枚举**：未发送 / 已发送 / 已送达 / 已读 —— 至少区分**已送达与已读**（灰勾 vs 已读的视觉差异）
    2. **已读事件如何回传**：对端何时上报"已读"、上报的是**单条还是会话水位**（逐条回传会放大消息量，水位更好但要能反推每条状态）
    3. **本地存储**：已读水位存哪、跨设备/重连后如何对齐
    4. **视觉**：灰勾样式与现有消息状态图标一致，**不新造一套图标语言**
  - 验收：两端真实会话下能观察到 已发送 → 已送达 → 已读 的状态推进，且重连后状态不回退
  - 证据：修复 `712c2c6`。①状态枚举＝四态无 schema 变更：receipt `''`=已发送（落库在途）/`delivered` 已送达（灰 ✓）/`read` 已读终态（灰 ✓✓），未发送＝受理失败无行＋状态条标注；气泡沿用 text_muted 元信息灰勾，不新造图标语言。②回传＝逐条上报（拒绝水位：乱序已读无法从水位反推）：直连域 proto `Read.seq`（MsgType::READ=41）点对点直发发送方，按 `(to_id, seq)`＋`source='direct'` 命中；协作域沿用 msg_id＋服务端 READ_NOTICE。③上报时机＝对端会话在前台收到即报＋打开会话/日期窗时补报存量，引擎 `reported_reads_` 去重（重启幂等重报一次）。④存储对齐＝messages.receipt 列＋`receipt != 'read'` 只升不降守卫（两域同口径），重启重开库历史带 ✓✓ 不回退。真实启动验证（Xvfb 双实例双库）：/tmp/rc_shots/13_A_delivered.png（灰 ✓＋已送达 seq 1）→ 22_A_read.png（灰 ✓✓＋对方已读 2010ba08…）→ 23_A2_reconnect.png（A 重启后历史仍 ✓✓ 不回退）；全量 ctest 83/83 绿（test_direct_chat 四态推进/去重/只升不降/键防串扰＋test_chat_actions 直连缝）。已知约束：直连无服务端账本，对端离线期间的已读无法补传
- [x] 「盯」类提醒功能（来源：用户 2026-10-14，对标钉钉的"盯"）
  - 要求：参考钉钉的**「盯」**，对某条消息/某人做持续跟进提醒
  - 待定义（先出设计不写码）：盯的对象（消息/会话/人）、提醒触发时机、提醒只给盯的人还是给对方、盯的解除条件、与已读状态的关系
  - 验收：先出设计与交互说明，**经用户拍板后再实现**
  - 设计（2026-10-14 出，待拍板）：
    1. **对象＝单条消息**（我发出的行）：对标钉钉口径，直接复用刚落地的已读回执链（直连逐条 READ／协作 READ_NOTICE），盯＝"对这条消息的已读事件订阅提醒"。入口＝气泡右键/长按菜单「盯住本消息」；对端已读（receipt=read）的消息入口置灰——已读无需盯【已代决】
    2. **触发时机＝已读即提醒**：READ 到达落 `receipt=read` 的同时，若该消息在本端盯住集合 → 系统托盘通知＋状态条一行"对方已读你盯住的消息（预览文本）"＋会话列表该会话高亮。加一条**未读超时提醒**：盯后 30 分钟仍未读 → 托盘提醒一次"盯住的消息仍未读"（只一次，不循环轰炸）【已代决，间隔可调】
    3. **通知范围＝只给盯的人，对方无感**：轻量自用跟进，不向对端发送任何"被盯"信号，不升级成催促工具【已代决；若想要钉钉 DING 式对方侧强提醒（对端弹窗/角标），**待用户拍板**——那是对端新帧型＋开关，另立一单】
    4. **解除条件三通道**：①已读自动解（触发提醒即解，盯与已读天然同生命周期）；②手动解（菜单「取消盯」）；③超时解（默认 24h，防僵尸盯，解除不通知）【已代决，时长可调】
    5. **与已读状态的关系＝严格寄生**：盯不新增回传路径、不改 receipt 语义；READ 已到达的迟到盯创建时立即检查 receipt，已 read 则提示"已读"并拒绝创建。乱序鲁棒性继承已读链的只升不降【已代决】
    6. **存储＝本地库持久化**：新表 `watched_messages`（键与 receipt 同口径：直连 `(to_id, seq)`／协作 `msg_id`，加创建时间做超时判定），重启后盯仍有效，READ 到达时照常触发【已代决】
    7. **视觉＝沿用现有元信息语言**：被盯气泡时间戳旁一个小眼睛字符图标（text_muted 灰，与 ✓/✓✓ 同行同色），不新造图标体系；会话列表高亮复用未读高亮样式【已代决】
    8. 范围外：盯"人"（对方上线即提醒）依赖在线状态机制，本单不做【待用户拍板要不要，要则另立单】
  - **修订（实现单·用户 2026-10-14 定稿，按此实现）**：②触发＝**未读超阈值（默认 10 分钟，QSettings `watch/remind_minutes` 可配、sweep 现读即时生效）才提醒**——对方一打开就提醒是"已读通知"不是"盯"，已读改为**静默自动解除**（摘 ◉、无提醒文案、无高亮）；④解除三通道口径不变（已读静默解复用已读水位/逐条 READ 判定，不新造状态源）＋手动解＋超时解（`watch/expire_hours` 可配）；⑥存储仍走 `watched_messages` 独立小表（盯行有独立生命周期字段 created_ms/reminded 与三通道解除删除，不塞 messages 台账；键与回执同口径）；⑦入口＝气泡右键（长按后续）＋已盯 ◉ 视觉＋**任务菜单「盯列表…」**看到"我在盯哪些"并逐条取消
  - 证据：修复＋功能 `2c1cd18`（另 `a8c1471` 顺带修重启后直连 seq 归零重发、双端 UNIQUE 撞行静默丢消息）。①盯创建/已读拒绝：Xvfb 双实例双库实测 /tmp/wt_shots/69_item_zoom.png（已读行菜单「盯住本消息」置灰、点击无效盯表仍 0）＋ 84_menu6.png/85_status（未读行右键点「盯住本消息」→ 盯行落库＋meta 行长 ◉）——右键取键改走 cursorForPosition.charFormat（Qt anchorAt 对嵌套表气泡锚命中区空间错位，探针实录字形区全空）。②已读静默解：87_watch.png（B 打开会话即读 → ✓✓ 出现、◉ 消失、盯表清零，状态条仅既有"对方已读 ✓✓"回执文案、无盯提醒）。③未读超阈值提醒：A 配 remind_minutes=1 实测 93_status.png（"盯住的消息仍未读：盯未读提醒腿"）＋ reminded=1 只一次（后续 sweep 不复燃）；96_watchlist.png（任务→盯列表…：条目"直连 · coding · 盯未读提醒腿 · 已盯 N 分钟"带提醒高亮底色＋取消盯按钮）。④重启持久：98_A_reopen.png（重启后历史重载 ◉ 重挂、reminded 不重置、sweep 不误报）。⑤全量 ctest 84/84 绿（test_watch_reminder 12 腿：创建/UPSERT/列表快照取消/已读拒绝/静默解幂等/协作 c: 键/手动解/阈值边界 9↔11 分钟/阈值可配/24h 过期只清目标行/重启装载不误报）

## 待办

- [x] docs 根目录双语配对（2026-10-16 完成）：`docs/审计-文档一致性.md` 已补英文对应件 `docs/audit-doc-consistency.md`（183 行逐条真译，表格/代码引用/专有名词原样保留），双方首行切换器互链；`docs/建设方案评审报告-V10.0.md`（1204 行 >1000 阈值）按红线只做摘要级英文版 `docs/plan-review-report-v10.0.md`（保留全部标题层级/R1–R16 需求编号/26 张表/决策项等事实，段落压缩不逐段硬译，顶部如实标注摘要级＋中文原文为准），双方首行切换器互链。
- [x] 文档英文化缺口（2026-10-16 登记，同日完成）：下列中文文档全部补上英文版＋双向语言切换器——
  - 设计稿 `docs/design/` 23 篇：`IP与办公位置显示.md`、`OIDC身份认证接入设计.md`、`一级菜单管理.md`、`个人任务清单与外部工具接入.md`、`信箱.md`、`办公室位置图.md`、`品牌物料.md`、`基础平台化改造计划（身份-组织-授权-归档）.md`、`审批与日报周报.md`、`建群主题与群记录导出.md`、`文件存储与外网单向传输.md`、`机器人平台.md`、`权限模型.md`、`模型网关.md`、`消息标识与归档去重.md`、`群内智能助手.md`、`群工具三件.md`、`群工具框架.md`、`群服务器工具.md`、`群组知识共享.md`、`远程协助.md`、`group-remote-access.md`（英文文件名但正文为中文）、`settings-consolidation.md`（英文文件名但正文为中文）
  - 文档站 `docs/src/`：首页 `index.md` ＋ 指南 19 篇（`product`／`prototypes`／`requirements`／`roadmap`／`decisions`／`report`／`compliance`／`authorization`／`feiq-compat`／`file-storage`／`backup-restore`／`wan-drill`／`notify`／`settings`／`crash-reporting`／`platform-adapter`／`delivery-matrix`／`av-collab`／`development`）；`.vitepress/config.mts` 仅 `lang: 'zh-CN'`，无 `locales` 配置
  - 完成说明：设计稿 23 篇各补英文对应件（21 个新英文文件名；`group-remote-access.en.md`／`settings-consolidation.en.md` 按 `.en.md` 约定），指南 19 篇各补 `.en.md`，首页补 `index.en.md`；43 对文件首行切换器互链（脚本校验零断链）；`pnpm docs:build`（pnpm 10，CI 钉版）通过。登记下轮的两项缺口（`locales` 双语路由配置、英文版内部链接指向中文原文件）已于下条条目完成。
- [x] 文档站双语路由 `locales`（2026-10-16 登记下轮，同日完成）：`docs/src/.vitepress/config.mts` 加 `locales`（root=简体中文 `zh-CN` ／ en=English `link:'/en/'`），英文页由 `*.en.md` 平移进平行 `src/en/` 树（首页＋指南 19 篇，`git mv` 保留改名追踪）；中英各一套 nav/sidebar/主题文案（`outline`/`docFooter`/`lastUpdatedText`/明暗切换/footer 中英分列，侧边栏标签取各页 H1 原文）；40 页首行切换器由相对 `X.en.md` 改写为绝对 `/en/...`↔`/guide/...`（顺带修掉首页切换器重复两行的旧 bug），英文正文内链一律指向英文对应页（`/guide/x`→`/en/guide/x`，`/screenshots/*` 属公共资源不加前缀）。
  - 证据：`vitepress build src`（vitepress 1.6.4，`docs/package.json` 钉版，等价 CI `pnpm docs:build`；本机无 pnpm 故直接调 `docs/node_modules/.bin/vitepress`）build complete in 15.73s，产出英文页 20 张于 `/memex/en/**` ＋中文页 20 张＋`404.html`，旧 `*.en.html` 残留 0；**零死链**（`ignoreDeadLinks` 默认开启，断链即 build 失败）＝内链改写全量校验通过；`<html lang>` 分别为 `zh-CN`／`en`，translations 切换菜单已渲染；20 对双语互链脚本校验 20/20 一致且目标文件全部命中。

## 2026-10-19 深度源码审核第一批（A0）

- 记录：[docs/review/audit-batch-a0-2026-10-19.md](docs/review/audit-batch-a0-2026-10-19.md)。已同步 `origin/main`；清点 `git ls-files` 共 654 个 tracked 文件并建立模块审计矩阵。A0.4–A0.8 的执行规则已按本批证据收口；A0.1–A0.3 保持进行中，因尚未逐文件阅读、逐条核对 45 个未勾选框及完成所有业务链路的失败/重试/重启检查。
- 当前源码证据仍支持队列中的 P0-1/2/3/4 与 P1-1/4/6 风险，分别涉及协作态 TLS、落库后 ACK、归档/离线队列失败语义、文件覆盖、TOFU 首信任、帧累计缓冲和 Admin CSRF/session；需按原条目实施及测试，不重复登记、不在本批宣称修复。未在本批验证 Windows/macOS/UOS/麒麟/OHOS 或真实公网部署。

## 原型符合度（来源=用户 2026-10-24 令；验收＝与 prototypes/xxx.html 逐屏比对一致(控件/布局/文案/间距)）

台账：`docs/design/prototype-conformance.md`（36 张原型 → C++/Kotlin 界面，含留痕铁律与未入型清单）。

### 桌面 · 主窗口与列表面

- [x] 与 prototypes/desktop-chat.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/main_window.cpp` 群聊页）（✅ 2026-10-29 afa99fd：头像列/胶囊系统行/文件卡片/日期分割线/输入区说明条/未归档徽标全量补齐＋warning_wash 令牌；门禁 84/84 绿）
- [x] 与 prototypes/desktop-collab.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/main_window.cpp` 协作态会话列表）（✅ 2026-10-30 cceec84：会话行工厂（40px 头像+在线点/姓名 600/时间 11px/预览 12px/未读胶囊 min-17）＋置顶/最近联系分区（群聊并入，头部「群聊·服务端归档」块撤）＋side-head「+」发起单聊钮（账号菜单=组织架构成员）＋主题切换行控件重建；门禁 84/84 绿。近似项见台账）
- [x] 与 prototypes/desktop-direct.html 逐屏比对一致(控件/布局/文案/间距)（未登录直连列表态）（✅ 2026-10-30 d52bfc2：直连行控件化（头像/在线点/「账号·设备」名/时间/末条预览/未读胶囊）＋「在线设备」分区＋侧栏头「自动发现 x.x.x.0/24 · UDP 2425」注脚＋计数绿点＋搜索占位改「搜索设备 / IP」（检索字段不变）；门禁 84/84 绿。近似项见台账）
- [x] 与 prototypes/desktop-direct-chat.html 逐屏比对一致(控件/布局/文案/间距)（直连会话展开态）（✅ 2026-10-30 128c3ec：会话头 28px 头像＋「账号·设备名」标题＋icon-btn 化动作位（文件/更多菜单＝既有真实操作）＋系统胶囊行对齐「已通过 UDP 广播发现对方 · 本对话不归档」＋输入区「直连 · 无归档」描边胶囊（随形态换字）；门禁 84/84 绿。近似项见台账）
- [x] 与 prototypes/desktop-friends.html 逐屏比对一致(控件/布局/文案/间距)（好友列表）（✅ 2026-10-30 d94d616：rail 增「好友」钮切真·好友视图（协作态侧栏只摆通讯录：星标好友＋部门分组头「路径 · N 人」）＋行控件复用会话行工厂（40px 头像/在线点/「人名 · 部门末级」名/时间/末条预览/未读胶囊）＋side-head「好友」/「搜索好友 / 工号」/「来自组织架构通讯录」注脚/「N / M 在线」绿点计数＋tooltip 账号·部门；`layout-friends[-light|-dark]` 改存好友视图实况、原列表态实况迁 `layout-messages-*`；门禁 84/84 绿。近似项见台账）
- [x] 与 prototypes/desktop-group.html 逐屏比对一致(控件/布局/文案/间距)（群聊基础面）（✅ 2026-10-30 0148ddc：群右栏 infopanel（群成员·N＋28px 头像行＋群主胶囊＋「查看全部 N 人」开群资料＋群文件≤3＋归档说明虚线卡）＋会话头 meta「N 人 · 群主：X · 全量归档」（群号挪标题悬浮）＋头部 📢群公告/🗂群文件(切右栏)/👥成员(brand)钮＋横幅公告口径（群公告(群主：X)：…，brand 底）＋气泡 meta 群主标签（10px 品牌色）＋「@ 唤起成员选择」提示与占位「输入消息，@ 成员可加提醒…」＋「群消息全量归档」描边胶囊＋群行副行「N 人 · 群公告已更新」＋临群行控件化（opacity .7＋「免服务端 · 仅本网段 · 不归档」＋「未归档」徽标）；live-group 实况重摄含右栏（像素核验），门禁 84/84 绿。近似项见台账）
- [x] 与 prototypes/desktop-files.html 逐屏比对一致(控件/布局/文案/间距)（文件传输面）（✅ 2026-10-30 937ba5d：rail「文件传输」钮切真·传输管理面——220px 传输通道侧栏（点对点直传 active 行/共享空间置灰/旁路设计虚线卡/已发现伙伴品牌卡）＋头部 meta「正在传输 N · 今日完成 N · 累计 X」（真实台账聚合）＋↑发送文件钮＋全部/传输中/已完成/已暂停标签行＋传输卡（36px 图标/「目录名/（文件夹 · N 个文件）」名/副行/状态胶囊/6px 进度条/「✓ 接收方已确认·SHA-256 已校验」行/取消/打开目录/所在会话钮）＋页脚授权留痕说明卡；台账会话内存级（同 id 覆盖＋进度差分速度）；live-files 实况重摄为管理面（像素核验），门禁 84/84 绿。近似项见台账）
- [x] 与 prototypes/desktop-org.html 逐屏比对一致(控件/布局/文案/间距)（组织架构与汇报关系）（✅ 2026-10-30 6ef8efb：rail「组织架构」钮切三栏整屏——240px 左栏（「搜索部门 / 成员」过滤＋QTreeWidget 部门树带右对齐子树计数列＋「全部成员」根＋通讯录可见性虚线卡）＋中栏（「部门 · N 人」头＋部门负责人胶囊（结构推导）＋成员卡网格：44px 头像/姓名/部门·职务副行/在线点、「（我）」卡品牌底）＋右栏档案面板（64px 头像/在线徽标/汇报链路沿 manager 上溯（环防御）/kv 直属上级·部门·职务·在线时长/「发消息」「设为常用联系人」钮/数据口径注脚）；rebuild 只在进图/ORG_DATA/主题三入口（apply_view_mode 只管显隐——show_org_profile 尾部回调成无限递归实录 SEGFAULT 已防）；test_mode_switch org 腿：CLI 造部门树＋汇报链→树计数/卡片/档案链路断言→live-org 实况重摄（浅＋深，像素核验）→还原空档案（后续文件腿授权裁决依赖空部门路径相等）；门禁 84/84 绿。近似项见台账）
- [x] 与 prototypes/desktop-search.html 逐屏比对一致(控件/布局/文案/间距)（归档检索与导出）（✅ 2026-10-30：管理台菜单「归档检索台…」整窗 ArchiveConsole（auditor 专属，判权全在服务端）——条件行（人员账号/时间窗勾选＋起止/关键词/「精确匹配」语义说明胶囊/检索 brand 钮）＋五列命中表（关键词品牌色高亮（服务端同款分词）/会话对端「甲 ⇄ 乙（单聊）· →（群）」/归档时间/「已撤回 · 原文留存」warning 胶囊/查看上下文·撤回事件平钮）＋页脚命中计数＋「导出留证」证据包回显＋查阅日志台账（检索/上下文/撤回事件/导出各落一条 audit_reads 行）；新面全真实后端：`/files/audit/` 三路（context span5/recall-event/export）＋store 两法（context_around/last_recall_event），撤回原文保留可检可导；theme_changed 重染＋命中行随缓存回包重建；test_archive_console 冒烟（无角色 403→授 auditor→命中两行→上下文窗口前后行在→撤回人 member1→未撤回如实呈现→证据文件头注/行/计数→四类留痕行）；live-search[-light|-dark] 由 CLI 终端输出改摄控制台实况（像素核验）；门禁 85/85 绿（bot/ldap 各一次满载假红、隔离复跑绿后全量再跑收口）。近似项见台账）
- [x] 与 prototypes/main-empty.html 逐屏比对一致(控件/布局/文案/间距)（联系人空态）（✅ 2026-10-30 c738619：登录后零会话列表态——侧栏两行引导「还没有会话／从通讯录选择联系人，开始聊天」（muted 12px 与原型 .empty-list 逐字一致；QSS item padding 8px 会把行内控件矩形压成 15px 半行，sizeHint 补 16px 修复）；主区欢迎页（.first-guide：标记＋「欢迎使用 Memex」＋副题＋四链接卡文案）按布局令（列表态坍缩 320px 面板隐藏）潜伏渲染就位，close_chat/refresh_devices 共用 render_empty_state 出口（替换对已登录用户误导的「请登录协作态」发现引导；rerender_chat 随登录态重推导）；test_mode_switch 空态腿：独立 app 名 w0 隔离 QSettings/本地库，登录→hint 在场＋欢迎文案入主区→layout-empty[-light|-dark] 实况（像素核验两行文本在场）→开会话行即撤→登出即撤；deferred delete 排干后再查树（死指针假在场坑）；mode_switch ctest 超时 90→240s（高载 load30+ 顶死假红）。门禁 85/85 绿（task_dialog/collab_resend 满载假红、隔离复跑绿、全量第三跑收口）。近似项：✦→●、动作钮/链接卡以说明文案呈现不造假控件、hint 落首行非整栏居中、side-nav 页签由协作态分区取代——见台账）

### 桌面 · 对话框（已实现面，按原型收口偏差）

- [x] 与 prototypes/desktop-tasks.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/task_dialog.cpp`）✅ 2026-10-30 e0b397e（双栏 body-split 重排：298px 录入列月历/录入区/外部登记/脚注＋视图列 sec-head 计数与「只看未完成」/列表↔看板互斥/领导视角/状态行；连接区已连接徽章、拉取行并入工具行右侧；近似项见 prototype-conformance 台账。门禁 85/85 绿（task_dialog 连接握手 wait 8000→15000 放宽，套件高载 load37 实测超 8s＝环境载压非回归）；实况浅深重摄像素核验）
- [x] 与 prototypes/desktop-approval.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/approval_dialog.cpp`）✅ 2026-10-29 ea50732（含走查步骤见 prototype-conformance 台账）
- [x] 与 prototypes/desktop-report.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/report_dialog.cpp`）✅ 2026-10-29 ea50732（含走查步骤见 prototype-conformance 台账）
- [x] 与 prototypes/desktop-group-tools.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/group_tools_dialog.cpp`）✅ 2026-10-30 c2825a4（三页 Tab 每页 ts-head 规则副题；发起区并单行；连接区「已连接」徽章；脚行判权/结果卡说明；窗题带群号、1080×720。近似项见 prototype-conformance 台账。门禁 85/85 绿一次过；实况浅深重摄像素核验）
- [x] 与 prototypes/desktop-group-memo.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/group_memo_dialog.cpp`）✅ 2026-10-29 ea50732（含走查步骤见 prototype-conformance 台账）
- [x] 与 prototypes/desktop-group-vault.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/group_vault_dialog.cpp`）✅ 2026-10-30 280c27b（双栏 split 重排：400px 条目列解锁行/掩码列表/条目动作行/箱管理区「群主/管理员：」＋四钮＋右注独占一行（≈原型 flex-wrap）＋详情列「条目详情 · 查看/复制均为显式动作，服务端全留痕」卡与 ⚠ 硬性分离告警；连接区已连接徽章；窗 1000×660；近似项见 prototype-conformance 台账。门禁 85/85 绿两轮（重排轮＋抓图腿轮）；`test_group_vault_dialog` 新增抓图腿，实况浅深首摄像素核验）
- [ ] 与 prototypes/desktop-group-ci.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/group_ci_dialog.cpp`）
- [ ] 与 prototypes/desktop-group-pack.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/group_pack_dialog.cpp`）
- [ ] 与 prototypes/desktop-group-servers.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/group_server_dialog.cpp`）
- [ ] 与 prototypes/desktop-group-record.html 逐屏比对一致(控件/布局/文案/间距)（`MainWindow::group_record_export_dialog`）
- [ ] 与 prototypes/desktop-assist.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/assist_dialog.cpp`）
- [ ] 与 prototypes/desktop-office-map.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/office_map_dialog.cpp`）
- [x] 与 prototypes/desktop-audit.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/audit_dialog.cpp`）✅ 2026-10-29 ea50732（含走查步骤见 prototype-conformance 台账）
- [x] 与 prototypes/desktop-file-assistant.html 逐屏比对一致(控件/布局/文案/间距)（`client/app/file_assistant.cpp`）✅ 2026-10-29 ea50732（含走查步骤见 prototype-conformance 台账）

### 桌面 · 设置页

- [ ] 与 prototypes/settings-general.html 逐屏比对一致(控件/布局/文案/间距)（主题/主题包/品牌）
- [ ] 与 prototypes/settings-account.html 逐屏比对一致(控件/布局/文案/间距)（账号与设备）
- [ ] 与 prototypes/settings-message.html 逐屏比对一致(控件/布局/文案/间距)（表情/截图偏好）
- [ ] 与 prototypes/settings-privacy.html 逐屏比对一致(控件/布局/文案/间距)（离开/锁屏/通知偏好）
- [ ] 与 prototypes/settings-advanced.html 逐屏比对一致(控件/布局/文案/间距)（网络与远程/快捷键/连接）

### 邮箱（未实现面，先建后比）

- [ ] 与 prototypes/desktop-mailbox.html 逐屏比对一致(控件/布局/文案/间距)（邮箱＝统一收件箱：告警/通知/公告；桌面侧尚无实现）
- [ ] 与 prototypes/mobile-mailbox.html 逐屏比对一致(控件/布局/文案/间距)（移动端邮箱；尚无 MailboxActivity）

### 移动端

- [ ] 与 prototypes/mobile-chat.html 逐屏比对一致(控件/布局/文案/间距)（`ChatActivity.kt`）
- [ ] 与 prototypes/mobile-init.html 逐屏比对一致(控件/布局/文案/间距)（`InitActivity.kt` / `LoginActivity.kt`）
- [ ] 与 prototypes/mobile-me.html 逐屏比对一致(控件/布局/文案/间距)（`MeActivity.kt`）
- [ ] 与 prototypes/mobile-sessions.html 逐屏比对一致(控件/布局/文案/间距)（`MainActivity.kt`）
- [ ] 与 prototypes/mobile-tasks.html 逐屏比对一致(控件/布局/文案/间距)（`TasksActivity.kt`，看板 <600dp 自动降级回列表）
