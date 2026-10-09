# 界面原型与预览

原型设计稿是交付物，覆盖桌面端八屏与手机端四屏。风格与产品气质一致：品牌橙 `#e16531`（取自 logo）主色、暖中性底色、亮暗双主题。源文件（HTML＋共享设计系统 `prototypes.css`）在仓库 `docs/design/prototypes/`，截图在 `docs/src/public/screenshots/`。

每屏均可加 `?theme=dark` 参数查看深色版；首页轮播的右上角按钮可切换截图主题。

## 桌面端

| 屏幕 | 说明 |
| --- | --- |
| 未登录直连态 | 零配置：UDP 广播自动发现同网段终端；会话标注「仅存本机 · 不入归档」；无对话＝列表态长方形面板（不摆聊天面板），点好友展开对话 |
| 好友列表 | 协作态下的纯好友列表（布局令列表态）：组织架构通讯录分组、搜索好友／工号、在线数统计；点好友展开对话，再点当前好友或 ✕ 关回列表态 |
| 登录协作态 | 长连接在线、全量归档；服务端不可达时状态徽标显式回落提示 |
| 单聊 | 归档起点「归档自 X 时刻」、跨态会话「未归档」不可关闭、撤回仅显示层（原文留存） |
| 群聊 | 公告、@提醒、成员管理；群消息同样全量归档 |
| 组织架构 | 部门树、成员卡片、汇报链路、直属上级、可见性策略提示 |
| 文件传输 | 点对点直传不经服务端、断点续传、元数据入归档 |
| 归档检索 | 管理台：人员＋时间＋关键词检索、撤回原文留存标记、查阅日志、导出留证 |

### 未登录直连态

实况截图：双实例同机互发现（UDP 2425）。**布局令（2026-10-08）**：无对话＝
「左菜单＋好友列表」长方形列表态，**不摆聊天面板**——启动/关会话即此态；
点好友后右侧展开对话（点好友或点标题栏 ✕ 回列表态），展开态实况见下方
第三、四张（真实客户端窗口，`test_mode_switch` 布局腿程序化抓取，深色随
2026-10-09 补摄）。

![桌面端未登录直连态·列表态（浅色·实况）](/screenshots/live-direct-light.png)
![桌面端未登录直连态·列表态（深色·实况）](/screenshots/live-direct-dark.png)
![桌面端直连会话展开态·点好友后（浅色·实况）](/screenshots/layout-chat.png)
![桌面端直连会话展开态·点好友后（深色·实况）](/screenshots/layout-chat-dark.png)

### 好友列表

实况截图（协作态列表态，2026-10-09 补摄、2026-10-10 浅色重摄＋深色补齐）：登录协作态、未开会话＝「左菜单＋
好友列表」长方形列表态（不摆聊天面板）——局域网设备分组（在线数统计）＋
「协作会话 · 服务端归档」分组（未读／在线标识、星标置顶随常用联系人）；
搜索栏按昵称／账号／IP／群名过滤；点好友展开对话、再点当前好友或 ✕ 关回
列表态（真实客户端窗口，`test_mode_switch` 布局腿程序化抓取）。**如实更正**：
2026-10-09 首轮入库的「浅色」实为深色抓图——测试切暗腿的还原缺陷（system
档判暗兜底回读已被染暗的调色板，还原又落回深色），浅图带暗入库；本轮修
抓取机制（还原改显式回亮色）后浅色重摄、深色一并补齐。

![桌面端好友列表（浅色·实况）](/screenshots/layout-friends.png)
![桌面端好友列表（深色·实况）](/screenshots/layout-friends-dark.png)

原型稿：浅色与深色各一（协作态好友列表设计稿，保留备查）。

![桌面端好友列表（浅色·原型稿）](/screenshots/desktop-friends-light.png)
![桌面端好友列表（深色·原型稿）](/screenshots/desktop-friends-dark.png)

### 登录协作态

![桌面端登录协作态（浅色·实况）](/screenshots/live-collab-light.png)
![桌面端登录协作态（深色·实况）](/screenshots/live-collab-dark.png)

### 单聊

实况截图：直连单聊互发文本——外出气泡品牌橙靠右、来访气泡靠左，行首标注「直连·仅本机」，顶部常驻「消息不进归档」横幅。

![桌面端单聊（浅色·实况）](/screenshots/live-chat-light.png)
![桌面端单聊（深色·实况）](/screenshots/live-chat-dark.png)

### 群聊

![桌面端群聊（浅色·实况）](/screenshots/live-group-light.png)
![桌面端群聊（深色·实况）](/screenshots/live-group-dark.png)

### 组织架构

![桌面端组织架构（浅色·实况）](/screenshots/live-org-light.png)
![桌面端组织架构（深色·实况）](/screenshots/live-org-dark.png)

### 文件传输

![桌面端文件传输（浅色·实况）](/screenshots/live-files-light.png)
![桌面端文件传输（深色·实况）](/screenshots/live-files-dark.png)

### 归档检索

![桌面端归档检索（浅色·实况）](/screenshots/live-search-light.png)
![桌面端归档检索（深色·实况）](/screenshots/live-search-dark.png)

## 手机端

| 屏幕 | 说明 |
| --- | --- |
| 初始化向导 | 第一步即域名／服务器地址（必填＋连通性校验），未完成不能进入主界面（R17） |
| 会话列表 | 移动端全部为协作态，不出现任何免登录入口（R18） |
| 聊天 | 已读回执、文件卡片、归档状态随手可见 |
| 我 | 设备管理、桌面端单点在线说明、留存策略展示 |

### 初始化向导

实况截图（Android，T6.3）：首次启动强制进入服务器地址设置页，填写域名／IP（可带端口，缺省 24360）并连通校验（PING→PONG）通过后才放行登录——未完成初始化不出现任何可聊天界面（R17）。深色补摄（2026-10-10 深色主题走查，`walkthrough-18` 同源）。

![手机端初始化向导（Android 实况·浅色）](/screenshots/mobile-live-init.png)
![手机端初始化向导（Android 实况·深色）](/screenshots/mobile-live-init-dark.png)

原型稿：浅色与深色各一。

![手机端初始化向导（浅色·原型稿）](/screenshots/mobile-init-light.png)
![手机端初始化向导（深色·原型稿）](/screenshots/mobile-init-dark.png)

### 会话列表

实况截图（Android，T6.3）：登录成功后的主界面——协作态唯一入口，无任何免登录形态（R18）。深色补摄（2026-10-10 深色主题走查，`walkthrough-19` 同源）。

![手机端登录成功主界面（Android 实况·浅色）](/screenshots/mobile-live-main.png)
![手机端登录成功主界面（Android 实况·深色）](/screenshots/mobile-live-main-dark.png)

会话列表屏实况（Android，2026-10-09 走查补摄）：会话列表带未读角标、发起会话
输入框（对端账号）、文件助手／退出登录入口；真实服务端下发（webhook 通知镜像）
落会话并计未读——浅色（上）／深色（下）各一（`walkthrough-08/11` 同源）。

![手机端会话列表（Android 实况·浅色）](/screenshots/mobile-live-sessions.png)
![手机端会话列表（Android 实况·深色）](/screenshots/mobile-live-sessions-dark.png)

原型稿：浅色与深色各一（保留备查）。

![手机端会话列表（浅色·原型稿）](/screenshots/mobile-sessions-light.png)
![手机端会话列表（深色·原型稿）](/screenshots/mobile-sessions-dark.png)

### 聊天

聊天页实况（Android，2026-10-09 第二轮补摄）：**真实人对人互发**——zhangsan 离线
发给 lisi、lisi 登录收离线后在线回复，双向气泡（对端行＋「我」行，各带时间）
全链实录：离线投递→登录送达（未读角标）→在线回复（服务端受理并归档，SQLite
`messages` 表核实）；浅色（上）／深色（下）各一（`walkthrough-14/16` 同源）。
发送路径（BUG-006 修复后）实测不崩、受理即渲染。富交互实录：长按消息即出系统
选择工具栏＋「已复制」toast（`walkthrough-15` 入档）。**如实注明**：回复／表情／
滚动加载等入口 Android 端尚未实现（T6.3 范围外），无图可摄；另图中对端消息的
服务端归档恰触发新登记的 BUG-007（msg_id 撞车静默吞档，投递不受影响），见
BUGS.md。

![手机端聊天（Android 实况·浅色）](/screenshots/mobile-live-chat.png)
![手机端聊天（Android 实况·深色）](/screenshots/mobile-live-chat-dark.png)

原型稿：浅色与深色各一（保留备查）。

![手机端聊天（浅色·原型稿）](/screenshots/mobile-chat-light.png)
![手机端聊天（深色·原型稿）](/screenshots/mobile-chat-dark.png)

### 我

原型稿（Android 端「我」屏未建，iOS 端代码-only）：

![手机端我（浅色）](/screenshots/mobile-me-light.png)
![手机端我（深色）](/screenshots/mobile-me-dark.png)

## 原型源文件

原型为纯 HTML＋CSS（无框架依赖），浏览器直接打开即可交互查看：

- 桌面端：[desktop-direct](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-direct.html)（列表态：无对话不摆聊天面板）· [desktop-direct-chat](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-direct-chat.html)（点好友后对话展开）· [desktop-friends](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-friends.html)（协作态好友列表·列表态）· [desktop-collab](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-collab.html) · [desktop-chat](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-chat.html) · [desktop-group](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-group.html) · [desktop-org](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-org.html) · [desktop-files](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-files.html) · [desktop-search](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-search.html)
- 手机端：[mobile-init](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-init.html) · [mobile-sessions](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-sessions.html) · [mobile-chat](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-chat.html) · [mobile-me](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-me.html)

功能实现后，本页截图将逐步替换为客户端实况截图。当前进度（2026-10-10 走查轮更新）：桌面端七屏已全部替换为实况截图（浅／深各一，真实客户端在 Xvfb 下拍摄，逐张目检非原型稿）；未登录直连态已随布局令（2026-10-08）重摄为「列表态」实况（浅／深各一，Xvfb 双实例互发现拍摄），「会话展开态」实况浅／深各一补齐（点好友后，`test_mode_switch` 布局腿程序化抓取，深色本轮切暗补摄）；好友列表屏实况浅／深各一（协作态列表态，登录后经生产 ✕ 路径关回列表态抓取——局域网设备＋「协作会话·服务端归档」两分组，`test_mode_switch` 布局腿程序化抓取；浅色 2026-10-10 重摄更正——2026-10-09 首轮入库误为深色抓图，抓取机制还原缺陷已修，深色同轮补齐）；手机端初始化向导与登录成功主界面为 Android 实况（T6.3 模拟器走查，与 `apps/android/docs/walkthrough-0*.png` 同源；深色随 2026-10-10 深色主题走查补齐，`walkthrough-18/19` 同源）；会话列表屏与聊天屏本轮补 Android 实况浅／深各一（2026-10-09 模拟器 android-34＋本机 serve 走查，与 `walkthrough-08/09/10/11` 同源）——聊天屏实况 2026-10-09 第二轮已换真实人对人互发（离线投递→登录送达→在线回复全链，BUG-006 修复后实测不崩；长按复制富交互入档，`walkthrough-14/15/16` 同源，详见上「聊天」小节）；「我」屏 Android 未建、iOS 代码-only——暂仍为原型稿并如实标注。鸿蒙端（T6.2 桌面矩阵项，工具链/证书/真机未备受阻；鸿蒙手机版 T6.6 代码-only＋CI，编译腿待 OHOS 工具链）不为其承诺实况截图。
