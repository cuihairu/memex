# 界面原型与预览

原型设计稿是交付物，覆盖桌面端七屏与手机端四屏。风格与产品气质一致：品牌橙 `#e16531`（取自 logo）主色、暖中性底色、亮暗双主题。源文件（HTML＋共享设计系统 `prototypes.css`）在仓库 `docs/design/prototypes/`，截图在 `docs/src/public/screenshots/`。

每屏均可加 `?theme=dark` 参数查看深色版；首页轮播的右上角按钮可切换截图主题。

## 桌面端

| 屏幕 | 说明 |
| --- | --- |
| 未登录直连态 | 零配置：UDP 广播自动发现同网段终端；会话标注「仅存本机 · 不入归档」 |
| 登录协作态 | 长连接在线、全量归档；服务端不可达时状态徽标显式回落提示 |
| 单聊 | 归档起点「归档自 X 时刻」、跨态会话「未归档」不可关闭、撤回仅显示层（原文留存） |
| 群聊 | 公告、@提醒、成员管理；群消息同样全量归档 |
| 组织架构 | 部门树、成员卡片、汇报链路、直属上级、可见性策略提示 |
| 文件传输 | 点对点直传不经服务端、断点续传、元数据入归档 |
| 归档检索 | 管理台：人员＋时间＋关键词检索、撤回原文留存标记、查阅日志、导出留证 |

### 未登录直连态

实况截图：双实例同机互发现（UDP 2425），未选择会话的零配置引导态。

![桌面端未登录直连态（浅色·实况）](/screenshots/live-direct-light.png)
![桌面端未登录直连态（深色·实况）](/screenshots/live-direct-dark.png)

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

![手机端初始化向导（浅色）](/screenshots/mobile-init-light.png)
![手机端初始化向导（深色）](/screenshots/mobile-init-dark.png)

### 会话列表

![手机端会话列表（浅色）](/screenshots/mobile-sessions-light.png)
![手机端会话列表（深色）](/screenshots/mobile-sessions-dark.png)

### 聊天

![手机端聊天（浅色）](/screenshots/mobile-chat-light.png)
![手机端聊天（深色）](/screenshots/mobile-chat-dark.png)

### 我

![手机端我（浅色）](/screenshots/mobile-me-light.png)
![手机端我（深色）](/screenshots/mobile-me-dark.png)

## 原型源文件

原型为纯 HTML＋CSS（无框架依赖），浏览器直接打开即可交互查看：

- 桌面端：[desktop-direct](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-direct.html) · [desktop-collab](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-collab.html) · [desktop-chat](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-chat.html) · [desktop-group](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-group.html) · [desktop-org](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-org.html) · [desktop-files](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-files.html) · [desktop-search](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/desktop-search.html)
- 手机端：[mobile-init](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-init.html) · [mobile-sessions](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-sessions.html) · [mobile-chat](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-chat.html) · [mobile-me](https://github.com/cuihairu/memex/blob/main/docs/design/prototypes/mobile-me.html)

功能实现后，本页截图将逐步替换为客户端实况截图。当前进度：桌面端七屏已全部替换为实况截图（浅／深各一，真实客户端在 Xvfb 下拍摄，逐张目检非原型稿）；手机端四屏的实况截图待鸿蒙客户端（T6.2）落地后取得。
