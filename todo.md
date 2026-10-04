# 需求与任务清单（todo）

> 依据《建设方案评审报告 V10.0》拆解：R 条目＝需求与验收口径，A 条目＝第一期验收标准，任务按四期拆到可验收粒度。
> 追加需求（评审报告之后由建设方下达）以「R17／R18」编号进入本清单。状态：`[ ]` 未开始 · `[~]` 进行中 · `[x]` 完成。

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
| R10 | 功能对标企业微信 | 重要 | 按二期分期清单逐项验收，不要求一次性达成 | 二起 |
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

## 二、第一期任务拆解（可验收粒度）

### 阶段 0：工程骨架

- [x] T0.1 monorepo 聚合 CMake：client／server／common／admin 目录与开关（`MEMEX_BUILD_CLIENT`），顶层构建、ctest 通过。
  验收：空工程 `cmake --build build && ctest` 全绿。
- [x] T0.2 协议骨架（common）：帧格式（长度前缀 + JSON 载荷）、消息类型枚举、序列化往返单测。
  验收：协议单测覆盖编解码往返与畸形输入不崩溃。
- [x] T0.3 服务端骨架：asio TCP 监听、连接接入、握手与心跳、优雅退出、日志（前缀 `[MEMEX]`）。
  验收：集成测试起服务端、客户端连接完成握手、ctest 绿。
- [x] T0.4 客户端骨架：Qt6 Widgets 空主窗口可启动，双引擎类骨架（DirectEngine／CollabEngine）注册进主窗口，offscreen 冒烟测试。
  验收：offscreen 启动主窗口，引擎状态机单测绿。

### 阶段 1：直连态（对应验收 A1、A2、A11、A13 的直连半边）

- [x] T1.1 UDP 广播发现：绑定 UDP 2425，周期宣告（设备名、账号占位、直连协议版本），同网段在线设备列表（加入／超时离开）。
  验收：同机双实例互现；停掉一端后另一端在超时时间内将其置为离线（A1 半边）。
- [x] T1.2 直连单聊文本消息：TCP 点对点（2426 起）送达确认、本地 SQLite 落库（来源＝直连）。
  验收：双实例互发文本，重开后历史仍在本机；全程无服务端进程参与（A1、A2）。
- [x] T1.3 直连文件传输：文件与目录传输、进度、断点续传（分块 + 偏移协商）。
  验收：双实例传输 ≥100MB 文件与多层目录，中断后续传完成且哈希一致（A10 直连半边）。
- [x] T1.4 直连态界面：设备列表（替代联系人）、会话列表、聊天窗、「未归档」语义的本地态标记、未发现设备时的引导提示（改用协作态）。
  验收：零配置进入主界面即可完成发现与聊天（A1）。

### 阶段 2：服务端与协作态（A3–A6、A8、A11、A16）

- [x] T2.1 账号与设备：账号表＋密码摘要登录、设备指纹（SHA-256 哈希，主辅分级）、登录记录（设备类型／名称／时间／来源／版本）、桌面端单点在线互踢（原子化踢出＋客户端提示）。
  验收：同账号第二台桌面登录后第一台收到下线提示；登录记录全量可查（A4、A5）。
- [x] T2.2 接入网关与消息通道：长连接、心跳、断线重连、离线消息补投、消息标识去重、同类型互踢＋跨类型并存。
  验收：协作态双端经服务端互发文本、送达回执、msg_id 去重落库；接收方离线再上线补投且不重复；服务端重启后自动重连续达；桌面与手机各留一台在线（A3）。
- [x] T2.3 消息归档：协作态消息全量落库（messages 表：撤回只置标记不清正文）；撤回事件独立记录；本地缓存与服务端归档分离展示。
  验收：撤回后管理员仍可检索原文与撤回记录；终端删本地记录不影响归档（A6）。
- [x] T2.4 模式切换与降级：登录／登出不重启切换形态，本地历史合并展示（来源字段）；服务端不可达回落直连态并明确提示「消息不进归档」。
  验收：切换不丢历史（A3）；停服务端后直连可用且有提示（A11）。
- [x] T2.5 断线补传归档（决策事项 8 建议方案）：协作态双方均为已登录时，中断期消息本地暂存，恢复后按消息标识去重补传。
  验收：中断期消息恢复后归档无重复无缺失（A16）。
- [x] T2.6 组织架构：部门树、成员资料、直属上级独立字段（每人至多一名）、上级链路逐级上溯、批量导入（CSV）与错误项校验。
  验收：成员详情可见直属上级并可逐级上溯；导入错误行被校验拒绝（A19）。

### 阶段 3：管理后台与策略（A5、A6、A9、A20）

- [x] T3.1 管理后台骨架与成员部门管理：增删改查、批量导入、角色与权限分级。
  验收：管理员可维护部门与成员并生效到客户端。
- [x] T3.2 消息检索与导出：按人／时间／关键词检索、导出留证、查阅行为记日志。
  验收：检索命中归档消息且撤回原文可查；导出文件完整；查阅日志有记录（A6）。
- [x] T3.3 设备台账与登录记录：设备列表、责任人登记、启停解绑、登录记录查询。
  验收：台账对应关系与登录记录完整可查（A5）。
- [x] T3.4 策略开关下发（按部门）：允许免登录使用、允许与未登录设备通信、新设备登录需审批。
  验收：关闭跨态通信后跨态会话无法建立；关闭免登录后未登录端无法进主界面（A9）。

### 阶段 4：群聊、跨态与体验（A7、A13、A17、A18、A20）

- [x] T4.1 群聊：建群、拉人、群公告、@ 成员、群消息归档；免服务端临时群（直连态）单列。
  验收：协作态群消息全量归档可检索；直连临时群不经服务端。
- [x] T4.2 跨态互通：已登录端发现未登录端（广播携带账号标识仅作显示）、跨态会话「未归档」固定标识、跨态建立日志上报（时间／双方／时长，不含内容）、归档起点「归档自 X 时刻」。
  验收：跨态会话标识不可关闭；转协作态后归档起点与实际登录时间一致（A7、A8）。
- [x] T4.3 消息状态与多端：发送状态、已读回执、在线状态；桌面单点在线提示（与 T2.1 合并验收）。
- [x] T4.4 截图与标注：截屏、涂鸦／箭头／马赛克／文字、确认即发送；多显示器与高 DPI 适配。
  验收：多显示器＋高 DPI 下区域正确，标注后直达会话（A17）。
- [x] T4.5 表情与常用联系人：内置表情、自定义表情包导入、常用表情按频次；常用联系人置顶／星标／最近排序，数据落服务端（换机保留）。
  验收：表情包可导入并发送；常用联系人换机登录后完整（A18）。
- [x] T4.6 通讯录可见性：隐藏部门／成员（可白名单例外）、部门限看本部门、敏感人员字段隐藏。
  验收：被隐藏者不可见不可搜；受限部门只见本部门（A20）。
- [x] T4.7 系统集成：托盘、系统通知、开机启动、互踢提示；安装包命名 `MemexClient-x.y.z-*`（A23）。
- [x] T4.8 手机端初始化向导（R17／R18）：首次启动强制服务器地址设置页（含连通性校验），通过后进登录；移动端无任何免登录入口。
  验收：未完成初始化无法进入主界面；全部界面无匿名入口（R17、R18）。
  依赖（2026-10-04 用户令更新）：R17／R18 均为手机端专属规则（直连态／免登录仅桌面端）；初始化向导随 Android 端（T6.3，排前）落地启动，iOS 端（T6.4）代码面并行，不再等鸿蒙 T6.2（T6.2 属桌面矩阵项）。
  受阻（2026-10-04 探测）：构建链 hvigor／hdc／ohpm 与 OHOS SDK 均不在环境（见 T6.2），当时判「无鸿蒙客户端可承载初始化向导面——随 T6.2 解锁后启动」；该交付口径后经 2026-10-04 用户令改为随 Android／iOS（T6.3／T6.4）落地，探测结果本身留档不重探。
  进展（2026-10-04 Android 承载面收口）：`apps/android` 落地（见 T6.3 骨架笔）——InitActivity（地址输入＋PING→PONG 连通校验，校验不通过停留向导、无跳过路径）、LoginActivity（真 LOGIN 线格式，device_kind=mobile）、MainActivity 唯一 launcher＋RouteGuard 守卫（未初始化改道向导／未登录改道登录页）。单测 39/39（地址解析、帧编解码对齐 common/frame.cpp、RouteGuard 全组合、假服务端探测/登录全链路、R18 结构面：manifest 唯一 launcher＋res 无免登录类文案＋无硬编码文案）。真机走查（模拟器 android-34＋本机 memex_server）：清数据冷启→向导；坏地址→报错停留；10.0.2.2 校验→服务端日志「收到 ping」→登录页；zhangsan 登录→服务端「登录成功（zhangsan，mobile）」→主界面；重启→直接登录页（初始化持久化、未登录不进主界面）；清数据→回向导；登录页「修改服务器地址」→重设表单仍须过校验。截图七张入 `apps/android/docs/walkthrough-0*.png`。iOS 面随 T6.4（代码-only＋CI）。
- [x] T4.9 主题切换（R19，第一期主线后启动）：亮／暗主题跟随系统＋手动切换；界面颜色全部走主题令牌，预留多主题扩展位。
  验收：切换即时生效且重启后保持；系统亮暗变化时跟随；新增主题仅需新增一套令牌（R19）。
  进展（内核已落地）：`theme.{hpp,cpp}` 令牌层（23 令牌 × 亮暗两套；品牌橙 #e16531 两主题恒同，扩展主题也不许漂移主色）＋ ThemeManager（跟随系统按 QStyleHints::colorScheme 解析、手动选择优先且不被系统变化覆盖、QSettings 落盘 `appearance/theme_mode`）＋ 令牌化全局 QSS（`%令牌名%` 模板统一替换，无占位符残留）＋ 设置页 `theme_settings_page`（点选即时生效并回灌选中态）＋ main.cpp 起窗前 apply。验收用例 `test_theme`（令牌完备性与对比度门槛、跟随解析、落盘往返、QSS 覆盖、自定义主题扩展位、应用级即时生效、设置页交互），全量 21/21 两轮绿；xvfb 实截亮/暗两态像素差成立。
  接线收尾（T4.9 清零）：主窗各控件硬编码色全部改走令牌（apply_theme_styles 统一出口：侧栏/设备列表/会话头/气泡区/输入行/描边按钮/发送按钮/归档横幅，theme_changed 即时重刷＋setWindowIcon 同刷）；聊天区富文本内联色按 ChatRow 记录重放（rerender_chat，不重查库防丢即时提示）；「设置 → 主题…」入口挂进主窗菜单（macOS PreferencesRole）；令牌扩 4 枚（brand_text 浅底品牌色文本 5.3:1／success_wash+success_text 协作态横幅／disabled_bg 禁用底，亮暗各一套、对比度门槛入 test）；test_theme 补第⑧节主窗接线验收（生产路径 ThemeManager::instance() 驱动；「颜色全部来自令牌」运行时口径＝样式里每个 hex ∈ 当前主题令牌值，与④「QSS 必含品牌橙」自洽）。截图标注工具条为截图功能自身对比面（白色工具条压暗背景），不属主窗 UI 未令牌化。全量 25/25 绿。
- [x] T4.10 通知子系统：webhook 接入（token 鉴权、按群／个人独立 webhook，JSON payload＝目标／标题／内容／紧急程度／可选跳转，调用即向目标发消息，文档带 curl 示例）；紧急程度三级分级推送（普通＝站内会话消息、重要＝桌面通知强提醒、紧急＝置顶弹窗需确认收悉，全屏／演示模式策略可配）；个人通知偏好（每级是否弹窗、免打扰时段）可配；群公告式通知同级；手机端规则照旧（初始化设域名、无匿名聊天，紧急＝横幅＋震动）。
  验收：真实走查——curl 打 webhook → 群内收到消息 → 改紧急级别 → 弹窗真实弹出（截图）、前后对账。
  进展（已落地，四笔）：2b8782b webhook 面（协议 NOTICE=48＋Notice{title,content,urgency,jump_url}、kNoticeSender「通知」、compose_notice_text 同源正文、webhooks 台账 sha256 摘要、HTTP 接收器 POST /hook/<token>、投递镜像 TEXT＝离线入队＋归档＋在线扇出＋常用联系人，test_webhook 鉴权/校验/投递全绿）；3ee6309 CLI 面（serve --webhook-port 默认 24361 独立端口绑定失败只降级、webhook create|list|revoke、建即校验目标、token 仅一次显示＋自带 curl 示例）；8484905 客户端面（引擎 handle_notice＋notice_received、本地库 next_local_seq 解服务端起源消息 UNIQUE(from_id,seq) 冲突、notify_prefs 三级开关/免打扰跨零点/全屏策略、notify_center 重要托盘强提醒＋紧急置顶确认弹窗排队＋全屏递延退出补弹＋偏好设置页、主窗设置/托盘入口与 T4.7 气泡 kNoticeSender 守卫，test_notify 六节含真实服务端全链路与弹窗抓图）；文档笔 notify.md（payload 表＋curl 示例＋状态码＋截图）＋文档站导航。走查：curl 三级 200 带 msg_id、错 token 401、坏 urgency 400、messages 前后 0→3 条类型 notice 正文同源；紧急弹窗真实弹出截图入库。手机端规则（横幅＋震动）随 Android／iOS 手机端（T6.3／T6.4，2026-10-04 用户令）。

### 阶段 5：平台适配与验收（A12–A15、A21–A23）

- [x] T5.1 UOS／麒麟适配验证：消息、文件、界面、截图取屏（X11／Wayland 各自验证，不支持环境明确降级提示）（A12、A21）。
- [x] T5.2 备份恢复演练与预案文档（A14）。
- [x] T5.3 组件清单与许可核验（A22）、命名一致性检查（A23）。
- [x] T5.4 跨地域接入联调与断线补传场景验证（A13、A15、A16）。

### 阶段 6：平台矩阵扩展（手机端优先序 2026-10-04 用户令：Android 排前 → iOS 进行中·代码-only＋CI 验证 → 其他手机端排后·代码-only＋CI；桌面 macOS 已落地、鸿蒙 NEXT 照原计划不受手机端令影响）

- [x] T6.1 macOS 产物与每日构建三平台安装包：客户端 dmg（Memex.app）＋服务端 dmg（内含 pkg 引导安装），arm64（官方 Qt 6.10.3＋vcpkg 静态非 Qt 依赖）；Linux 补 deb/rpm、Windows 补 Inno setup.exe；install.sh／install.ps1 一键安装（OS/架构检测、装后 --version 真验证、幂等）（A23）。
- [ ] T6.2 鸿蒙（HarmonyOS NEXT）客户端（**桌面矩阵项**——2026-10-04 用户令：鸿蒙 NEXT 属 Qt 桌面矩阵，不受手机端口径令影响，照原计划）：OHOS SDK 交叉编译链（arm64-v8a）、HAP 打包、真机验收（Qt 官方支持口径：HarmonyOS 6.1 起、API 23+）。依赖：DevEco Studio 环境、应用签名证书、真机各一。手机端规则与桌面一致——初始化设服务端域名、无匿名聊天。前置：一期桌面主线清零后再启动。
  受阻（2026-10-04 环境探测）：`hvigor`／`hdc`／`ohpm` 均 NOT FOUND，未检出 OHOS SDK 与 DevEco Studio 目录；签名证书、真机亦无。三项依赖（工具链＋证书＋真机）齐备前无法开工，解锁后即启动。（原随本项落地的 R17／R18、T4.8、手机端通知规则已按 2026-10-04 用户令改由 Android／iOS 承载，见 T6.3／T6.4。）
- [x] T6.3 **Android 手机端（已落地）**：`apps/android` 从零（Kotlin），对齐桌面端功能面——登录（R17 初始化向导设服务器地址＋连通校验、R18 无匿名入口，随本块落地）、会话列表、单聊／群聊收发、消息推送横幅＋震动（对齐桌面端三级通知语义：普通站内／重要横幅强提醒／紧急横幅需确认）、移动端适配。
  验收：功能面＝登录／会话／收发／消息推送横幅震动对齐桌面端；**做完一块提交推送一块**（path-scoped，测试全绿后推）。
  进展（2026-10-04 第一块·工程骨架＋登录块）：Gradle 8.14.3＋AGP 8.13.1＋Kotlin 2.2.21，`com.memex.im`；协议单一事实源跨目录引 `common/proto/memex.proto`（protoc 4.31.1 lite 代码生成，与 C++ 线格式一致）；core 纯 Kotlin（ServerAddress 解析、FrameCodec 对齐 frame.cpp、RouteGuard/InitGate 守卫、MemexClient 探测/登录）；UI 三面（Init/Login/Main，唯一 launcher 经守卫）；单测 39/39 绿＋debug/release 双 APK 产出。走查与 R17/R18 收口详见 T4.8 进展笔；下一块：会话列表与收发。
  进展（2026-10-04 第二块·会话列表与收发）：core 长连接四件套——ChatStore（InMemory 会话聚合：历史/会话倒序/未读仅计接收/msg_id 去重/撤回仅展示层标记/本地 seq）、ChatSession（单 TCP 长连接，对齐桌面 collab_engine 语义：LOGIN 后单读线程分帧；发送 TEXT 本地立落库+受理回执 ACK(seq)→onSent；收到 TEXT 按 peer 归会话（群=to 的 group:N）、带 msg_id 回 ACK(msg_id) 清离线队列、重复补投去重；KICK 互踢断开回调；登出 LOGOUT）、ChatManager（holder/attach/监听/主线程投递）、ChatHolder；UI——MainActivity 会话列表（未读角标＋时间、发起会话、退出登录回落登录页）、ChatActivity 聊天页（我/对方气泡、撤回置灰、发送、自动滚底）、登录页改走 ChatManager.attach 一次建立连接（避免双连接互踢）；真 socket 线格式测试 ChatSessionTest 7 个（假服务端对齐 MemexClientTest 模式：登录帧字段/被拒原因、TEXT 线格式、ACK(msg_id)+去重、群按 to 归会话、KICK、本地立落库）。单测 53/53 绿（含既有 39）＋assembleDebug 产出；下一块：消息推送横幅＋震动及剩余移动端适配。
  进展（2026-10-04 第三块·消息推送横幅＋震动＋移动端适配）：core NOTICE 三级推送数据面（对齐桌面 collab_engine.handle_notice：归档态 composeNoticeText「标题：正文[ 跳转]」与服务端同源、peer 规则同 TEXT 群=to 个人=from「通知」、服务端通知无会话 seq（恒 0）则本地单调 seq、回 ACK(msg_id) 清离线队列、msg_id 去重、NoticeGrade 分级映射未识别按普通）＋Listener.onNotice 桥接（通知也触发会话列表刷新）；UI——MemexApp Application（三通知渠道：聊天消息/重要/紧急，均横幅＋声音＋震动；前后台与当前会话跟踪）、NotificationHelper 常驻通知器（ChatHolder 挂载跨重登保持：TEXT 仅后台且非当前会话横幅＋震动（前台站内渲染不重复打扰）；普通＝仅站内不弹（对齐桌面默认偏好）；重要＝横幅＋声音＋震动不看前后台（对齐桌面强提醒）；紧急＝setOngoing 常驻需点「确认收悉」动作（MemexNotifyReceiver）才可滑除，对齐桌面未确认继续排队弹出）、MainActivity Android 13+ POST_NOTIFICATIONS 运行时申请（未授权静默降级站内）、ChatActivity 进会话即清未读＋长按复制消息。单测 57/57 绿（ChatSessionTest 11：个人通知归档+ACK+分级、群通知按 to+跳转随文+未指定按普通、紧急+去重照回 ACK、映射与归档态）＋assembleDebug 产出。遗留：会话本地持久化（现内存实现，SQLite 随需要补）。
- [x] T6.4 **iOS 手机端（已落地：代码-only＋CI 验证）**：`apps/ios` 从零（Swift／SwiftUI）——登录、会话列表、单聊／群聊收发、消息推送（APNs 横幅／震动，对齐桌面端三级通知语义；R17／R18 手机端规则同）、移动端适配。
  验收（2026-10-04 用户令）：本机无 iOS 环境，**只写代码不做本地验证**；构建＋测试全走 GitHub Actions macOS runner（swift build／test 腿），红了修到绿。
  进展（2026-10-04 首块·工程骨架＋登录＋会话列表＋收发：全量代码-only）：`apps/ios` 从零——SwiftPM 包 MemexKit（core 面移植 Android 实现：ServerAddress 解析、FrameCodec 对齐 frame.cpp、ChatStore 会话聚合（msg_id 去重/未读仅计接收/撤回仅展示层）、ChatSession 长连接（单 TCP＋后台读线程：LOGIN 帧 device_kind=mobile、TEXT 本地立落库＋ACK(seq) 回执、收到 TEXT 按 peer 归会话群=group:N、ACK(msg_id) 清离线队列、KICK 互踢、LOGOUT）、MemexClient 探测/登录、RouteGuard/LoginState）；SwiftUI App 壳（XcodeGen project.yml 组装：RouteGuard 守卫三态路由、初始化向导（地址校验 PING→PONG 通过才保存）、登录页（attach 一次建连防双连接互踢）、会话列表（未读角标＋时间＋发起会话 sheet）、聊天页（气泡/撤回置灰/自动滚底）、NotificationManager（UNUserNotificationCenter 三级：normal 站内/important 横幅+声音/urgent 横幅+声音+需确认 category）＋APNs 注册）；protocol 生成代码 CI 侧 protoc→swift-protobuf 生成（默认命名前缀 Memex_Protocol_V1_，生成参数仅 Visibility=Public，protoc-gen-swift 不存在 PackageName 参数）；单测 37 用例（ServerAddress 10、FrameCodec 8、ChatStore 7、ChatSession 7 真 socket 假服务端、MemexClient 4、RouteGuard/LoginState 2）；CI 腿 `.github/workflows/ios.yml`（macOS runner：brew protobuf/swift-protobuf/xcodegen → 生成 → `swift test` → xcodegen＋xcodebuild 模拟器编译）。CI 绿后收口：真机 APNs 设备台账上报（T2.1/T3.3）与通知中心块；移动端适配（动态字体/深色模式随 SwiftUI 自适应，真机验证腿末块）。
  收口（2026-10-04 CI 全绿）：协议生成（protoc→swift-protobuf，Memex_Protocol_V1_ 前缀）→ MemexKit core 编译 → `swift test` 37 用例全绿（真机假服务端真 socket 全链路）→ xcodegen＋xcodebuild 模拟器 App 壳编译绿（macos-26 runner，Swift 6.1+；Apple 壳一次踩坑：部署目标 iOS 16 引用了 iOS 17+ 的 Color.tertiary，改 UIColor.tertiaryLabel）。测试竞态收口：ACK 断言改有界轮询快照读（5s bounded、不删断言）。遗留：APNs 真机设备台账上报（T2.1/T3.3）与通知中心块需真机验证；动态字体/深色模式随 SwiftUI 自适应，真机走查腿末块。
- [~] 其他手机端（鸿蒙手机版等，2026-10-04 用户令）：排在 Android／iOS 之后；只写代码不本地验证，验证走 CI 可用平台。注：早前「手机端只做 Android、iOS 不做」口径已被后两道令覆盖——最终口径＝Android 排前、iOS 进行中（代码-only＋CI）、其他排后（代码-only＋CI）。
  进展（2026-10-04 鸿蒙手机版·两块全量代码-only）：`apps/harmony` 从零（ArkTS，HarmonyOS NEXT，stage 模型）——第一块 core 纯 TS 逻辑层（hvigor har @memex/core，Node 可测）：wire.ts 手写 protobuf 编解码（canonical proto3：零值标量/空串不上线、oneof presence 空 message 照写；与 protobufjs 加载 common/proto/memex.proto 单一事实源**双向字节交叉**背书）、frame_codec.ts 镜像 client/core frame.cpp（4 字节大端前缀/ZERO_LENGTH/TOO_LARGE 同口径）、address/route/chat_store/format 移植 Android 同语义、session/client/wire_channel 长连接会话与探测登录（互踢/ACK(seq) 受理/ACK(msg_id) 去重/群=group:N 归会话/KICK 断开/NOTICE 分级）；Node 单测 73 用例（tsc strict＋node:test；wire 双向交叉、假服务端真 socket 全链路：登录/被拒/收发线格式/ACK/去重/群/KICK/三级通知/probe 四态；断言前一律有界轮询消读侧竞态）；CI `.github/workflows/harmony.yml`（ubuntu＋Node 22 npm test）绿。交叉验证揪出并修掉两处真实编码器缺陷（body 零值标量与 login_result ok=false 被无条件上线——被 protobufjs fromObject 非 canonical 行为掩盖成假匹配，canonical 对照后暴露）。第二块 ArkTS 壳（entry HAP，依赖 @memex/core file:../core）：HarmonyTransport（@ohos.net.socket 实现 core Transport）、AppState（preferences 初始化态＋内存登录态＋ChatHolder/ChatManager 合并桥）、五页（Index 守卫/InitPage R17 向导 PING→PONG 才落盘/LoginPage R18 attach 一次建连防互踢/MainPage 会话列表未读角标/ChatPage 气泡自动滚底清未读长按复制）、NoticeManager 三级推送（普通=仅站内；重要=横幅+震动不看前后台；紧急=前台确认弹窗排队/后台 isOngoing 常驻+震动，确认收悉撤销）、INTERNET/VIBRATE 权限。**验证边界（如实）**：本机与 CI 均无 OHOS 工具链（hvigor/ohpm/hdc NOT FOUND），Node 逻辑层 CI 全绿；ArkTS 编译腿（hvigor→HAP）待 OHOS runner＋SDK 可用后补，CI 与 apps/harmony/README.md 已如实注记，不造假绿。遗留：编译腿、真机走查（通知横幅/震动/互踢实景）、后台紧急横幅 wantAgent 动作按钮路径。


## 三、第二期（单独立项后细化）

- [ ] 审批（表单＋流程引擎）· 日程与待办 · 公告与日报周报 · 群工具（机器人／投票／接龙／群任务）
- [ ] 会话审计（查阅权限、查阅日志、合规留痕）
- [ ] 办公室位置图（平面图、拖拽式点位编辑器、搜索定位；默认仅本人楼层可见）
- [ ] 远程协助（屏幕查看＋受控方授权操作；WebRTC 媒体底座；受控方显式确认、过程持续可见、全程留痕、按部门开关；不做无人值守）
- [ ] 机器人平台（对标 Telegram Bot，规划预排）：bot 账号体系（管理台创建、独立 token、加入群／被私聊）；消息收发双模式（webhook 回调开发者服务＋长轮询 SDK——内网场景）；开发体验对标电报「几行代码跑通收发」（官方 HTTP API／SDK＋curl 示例＋本地调试工具：bot 收到什么打印什么）；权限边界（bot 可用范围、能看哪些消息留痕口径——bot 消息同样进留痕归档）；与通知子系统复用同一消息投递面（webhook／机器人共用）。前置：T4.10 通知子系统落地；实现另行排期。

## 四、第三期（依赖 GPU 硬件与一期归档）

- [ ] 模型网关（OpenAI 兼容、端点注册、路由、降级、调用审计；归档数据仅本地模型——写死在代码，不做成配置）
- [ ] 群内智能助手 · 会议纪要生成 · 需求整理 · 智能检索（检索增强）

## 五、第四期（建议不自研，独立立项）

- [ ] 音视频会议（采购或集成 LiveKit／Jitsi）· 协同文档（OnlyOffice／Collabora 独立进程部署，保持进程边界）

## 六、持续事项

- [ ] 决策清单跟踪：docs/决策清单.md 十五项待评审组拍板，工程按「建议」项先行。
- [ ] 文档站持续更新：界面预览随功能实现逐步替换为实况截图。
  进展（2026-10-04）：桌面端七屏已全部替换为客户端实况截图（批次一 `live-direct-*`／`live-chat-*`，批次二 `live-collab-*`／`live-group-*`／`live-org-*`／`live-files-*`／`live-search-*`，各屏浅深各一、Xvfb 下真实客户端双实例逐张目检；文件传输屏两轮均完成真实传输并 sha256 核验一致，归档检索屏含「合同」关键词 7 条真实归档记录，群聊屏为真实注入群会话）；手机端四屏仍为原型稿，待 T6.2。
- [ ] 组件清单（third_party/）：引入任何第三方组件即登记名称、版本、许可证、引入方式。
- [ ] BUGS 跟踪：缺陷登记在仓库根 `BUGS.md`（2026-10-04 起），只登记不修，修一条关一条。

## R23 文件存储与外网单向传输（2026-10-04 用户拍板，设计=docs/design/文件存储与外网单向传输.md）

- [x] R23-1 存储抽象层（S3 兼容）+ RustFS compose 集成：元数据表/群人两级配额/秒传（哈希去重）；客户端永不直连对象存储、权限只在元数据层判。
  - 2026-10-04 收口：核心实现（S3Storage 抽象+AWS SDK 实现、RustFSCompose、store 元数据层 files/group_quota/user_quota/uplink_logs、秒传 UNIQUE(file_hash,owner)、配额 UPSERT/0=不限/负数拒绝）来自并行 lane 已在 main；本轮补验证收口＋修三处实现缺陷（ceca13e）：①create_file_meta 秒传冲突时返回 last_insert_rowid（陈旧/错表 rowid）→改回查已有行 id；②compose 生成 RUSTFS_ADDRESS 裸 ":" 对 rustfs 1.0.1 非法（FATAL 起不来）→":9000"；③healthcheck `rustfs admin info` 子命令不存在→`rustfs info`。抽象层补 create_bucket（桶引导缺口）；CLI 新增 storage compose|up|down|health（数据目录须 chown 10001:10001——容器 uid 10001，真容器实证首写 FATAL Permission denied）；CMake 显式链 aws-cpp-sdk-core。
  - 验证边界（如实）：test_files_meta 纯库级单测（元数据/秒传/配额/留痕/compose 契约）CI 常跑；**真容器 e2e（test_s3_e2e，MEMEX_S3_E2E=1 门控）＝compose 起停→健康检查→S3 全接口往返（桶引导/put/get/head/list/预签名 curl 实传实取/两片分片合流/中止/批量删），本机 docker+RustFS 1.0.1 全绿**；CI 无 docker-compose-RustFS 腿（该测试在 CI 明跳，不造假绿）。本机 ctest 28/28 绿。
  - 决策留档：AuthorizationService 骨架本轮不立——R23-1 无 HTTP 判权消费者（抽象层只被测试直链），空架子违背谨慎原则；第一个消费者是 R23-2 的 upload/download/list 端点，**R23-2 开工时先立骨架再动端点**。server 启动时的 S3Config 加载/存储实例接线同留 R23-2。presign_put/get 与「客户端永不直连」铁律相悖，已在 storage.hpp 注明铁律张力警示（当前无消费者；R23-2/3 内网面不得使用，R23-4 若需直传须过设计评审明示暴露范围）。
- [ ] R23-2 群文件 + 个人文件（内网全功能）：群成员可读、群主/管理员可管（删/置顶/配额）；个人私有。
- [ ] R23-3 文件助手（自己↔自己）：备忘录文本 + 文件传输统一收件箱（含「手机发自己=文件传输」）。
- [ ] R23-4 外网单向 uplink + 外网模式客户端：唯一落点=文件助手、两套路由两套 scope（upload-only，下载端点对外网 token 恒 403）、上传全审计、**默认关闭显式开启开启时明示暴露范围**；内网用户转发进群（安全缓冲）。
- [ ] R23-5 防护：杀毒扫描钩子 + 类型/大小白名单 + 外网登录二次验证。

## R24 群组知识共享：公告 / 备忘录 / 密码箱（2026-10-04，设计=docs/design/群组知识共享.md）

- [ ] R24-1 群公告：发布/置顶全员可见/编辑历史留痕/联动三级推送（公告=重要强提醒）。
- [ ] R24-2 群备忘录：条目化共享知识（标题+正文+代码块）、搜索、修订历史可回滚、权限（管理员维护或开放示编辑留痕）；**禁止放密码**。
- [ ] R24-3 群密码箱：wingman 同款加密（PBKDF2 600k→KEK→AES-256-GCM 包 DEK）、显式解锁、每次查看留痕、掩码展示、授权名单（默认全成员，群主可改）、成员变更重包裹。
- [ ] R24-4 UI 边界：备忘录疑似密码提示、密码箱默认掩码、复制密码=显式动作+留痕、导出默认关闭。

## R25 群工具框架：CI/CD / 打包 / 配置导出（2026-10-04，设计=docs/design/群工具框架.md）

- [ ] R25-1 群工具框架：工具白名单动作配置、**入群即授权/退群即失**、动作留痕（谁/何时/动作/参数/结果）、服务端代理调用骨架。
- [ ] R25-2 CI/CD 工具：pipeline 状态视图（红绿灯+列表）、点击触发构建/打包（谁触发可回溯）、结果卡片回群。
- [ ] R25-3 打包工具 + 配置导出工具（首批动作类示例）。
- [ ] R25-4 凭据面：外部凭据只存服务端（R24 密码箱同款加密）、客户端零凭据、破坏性动作二次确认。

## R26 群服务器工具：agent + 负载状态 + 远程会话（2026-10-04，设计=docs/design/群服务器工具.md）

- [ ] R26-1 memex agent：轻量常驻（Linux 起步），注册/心跳/负载上报（CPU/内存/磁盘/负载）。
- [ ] R26-2 群服务器面板：服务器列表红绿灯+状态详情（R25 状态视图实例化）。
- [ ] R26-3 远程会话：SSH 起步（RDP/VNC 随后），一次性短票+接入留痕（谁/何时/连哪台/时长）。
- [ ] R26-4 凭据面：服务器凭据只存服务端（R24 加密面）、客户端零凭据；memex 只做「看+连」，操作类归 croupier。

## 权限模型（横切，2026-10-04 用户定调「类似 Linux 账号管理」，设计=docs/design/权限模型.md）

- [ ] 权限模型落地：群=组（入群即继承/退群即失/建群需特权/群能力管理员配全/组织架构可见/全程留痕）、群内角色（群主/管理员/成员）——R23-R26 各面统一按此模型实现，不各自造权限。

## R27 个人任务清单与外部工具接入（2026-10-04，调查分析=docs/design/个人任务清单与外部工具接入.md）

- [ ] R27-1 本地 todolist：自建/完成/提醒 + 他人分配（谁能分配按权限模型）。
- [ ] R27-2 Provider SPI + L1 跳转框架：capabilities 声明制（L1 跳转/L2 只读/L3 双向），detailUrl 必带。
- [ ] R27-3 首批 provider：GitHub Issues、飞书任务（Task v2）、钉钉待办——各做到能力声明深度。
- [ ] R27-4 二批 provider：Jira/Linear（按需启动）；禅道只接 L1 跳转；Teambition 不接。

## 平台化改造（2026-10-04 用户计划书，蓝图=docs/design/基础平台化改造计划（身份-组织-授权-归档）.md）

- [ ] 平台-1 授权统一服务 AuthorizationService：subject/action/resource/context→ALLOW/DENY+reason；数据过滤（Server Query→Filter→Allowed Records）；冲突规则（Explicit Deny>Allow>Inherited>Default）写死——一期必做。
- [ ] 平台-2 Identity 模型补全：Credential 独立（Password/Token/Certificate/SSO/Device Credential 预留）、Session 加 device_id/expire/logout_reason、IdentityBinding。
- [ ] 平台-3 Organization 与授权拆开：Account 拆 Membership/RoleAssignment/ReportingLine 三关联（一人多部门/多角色/临时代理）。
- [ ] 平台-4 Archive Event Sourcing：Message immutable + MessageEvent append-only（created/delivered/read/recalled/edited）；重投演示可重建。
- [ ] 平台-5 留存策略：废除「永久留存」口径→Retention Policy（30d/6m/1y/3y/Indefinite，管理员配置）；删除=Retention Purge（who/when/what/why/policy/approval，双人审批高风险）。
- [ ] 平台-6 审计独立角色：SecurityAuditor≠SystemAdmin（SystemAdmin 不自动可读消息）；审计本身留痕（audit.message.search/view/export）。
- [ ] 平台-7 降级显式化：DEGRADED 态「未归档通信」全面标识（顶部状态栏/会话标记/输入框提示/恢复补传规则）。
- [ ] 平台-8 Direct 安全：发现只做发现，TCP 上 Device Identity+握手+X25519+AEAD（消息/文件加密边界）。
- [ ] 平台-9 协议幂等：客户端至少一次+服务端 message_id 去重；本地 Sync State 状态机（LOCAL/PENDING/SENDING/SERVER_ACKED/ARCHIVED/FAILED）。
- [ ] 平台-10 文件授权：FileAuthorization 走统一 AuthorizationService（收发/下载/跨部门/再转发四问）。
- [ ] 平台-11 远程协助 Security Domain 模型（consent/audit/粒度权限 view/keyboard/mouse/clipboard/file_transfer）。
- [ ] 平台-12 P2 起按计划书 P0-P4 分阶段推进（高级权限等二期，Authorization 框架一期做）。
