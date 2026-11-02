# 配置效果（可视化）

> 本页记录**各配置开关的可见效果**：每开关一张关/开对比图＋切换动态 GIF，
> 由 `scripts/docs_gif.sh` 重跑生成。与[客户端设置](/zh/guide/settings)互补，
> 以「改前/改后」证据呈现；兼作配置效果回归门禁（off/on 无像素差异即失败）。

下列每一项都是翻转后有可见效果的开关。**关**图为翻转前状态，**开**图为
翻转后状态，GIF 为两者之间的过渡（交叉淡入）。

## 主题（亮 / 暗）

**位置**：设置 → 通用 → 主题下拉。亮→暗切换重绘整个客户端（聊天窗、列表、
对话框、设置面板自身）。关=亮色，开=暗色。

| 关（亮） | 开（暗） |
|---|---|
| ![主题 关](/screenshots/layout-chat.png) | ![主题 开](/screenshots/layout-chat-dark.png) |

![主题 过渡](/screenshots/gifs/theme.gif)

## 新消息通知

**位置**：设置 → 消息 → 新消息通知（`popup_message`）。关=来消息只进会话
（不弹窗）；开=桌面通知弹窗显示发送者与摘要。

| 关 | 开 |
|---|---|
| ![通知 关](/screenshots/config-effect-notify-off.png) | ![通知 开](/screenshots/config-effect-notify-on.png) |

![通知 过渡](/screenshots/gifs/notify.gif)

## 提示音

**位置**：设置 → 消息 → 提示音（`sound_mode`，三档）。关=全部静音（0 档）；
开=按事件类型播报（2 档）。

| 关 | 开 |
|---|---|
| ![提示音 关](/screenshots/config-effect-sound-off.png) | ![提示音 开](/screenshots/config-effect-sound-on.png) |

![提示音 过渡](/screenshots/gifs/sound.gif)

## 免打扰

**位置**：设置 → 消息 → 免打扰（`dnd`）。关=通知按各级既有策略；开=免打扰
时段内普通/重要通知静默（紧急仍须确认收悉）。

| 关 | 开 |
|---|---|
| ![免打扰 关](/screenshots/config-effect-dnd-off.png) | ![免打扰 开](/screenshots/config-effect-dnd-on.png) |

![免打扰 过渡](/screenshots/gifs/dnd.gif)

## 网段黑名单

**位置**：设置 → 高级 → 网段黑名单（`net_blacklist`）。关=放行所有网段；
开=命中段表则发现包不回应、新入站连接即断。

| 关 | 开 |
|---|---|
| ![黑名单 关](/screenshots/config-effect-blacklist-off.png) | ![黑名单 开](/screenshots/config-effect-blacklist-on.png) |

![黑名单 过渡](/screenshots/gifs/blacklist.gif)

## 外网入口端口（服务端 CLI）

**位置**：`memex_server serve --uplink-port <端口>`（R23-4）。关=无外网入口面
（服务端只监听内网）；开=另挂单向上传面（`/uplink/*`，无任何下载/读取内网
数据），启动即打印外网入口开启横幅。

| 关 | 开 |
|---|---|
| ![外网入口 关](/screenshots/gifs/uplink-off.png) | ![外网入口 开](/screenshots/gifs/uplink-on.png) |

![外网入口 过渡](/screenshots/gifs/uplink.gif)

## 流水线

`scripts/docs_gif.sh` 可重跑，全量再生成：

1. 重建 `test_settings_window` 与 `test_mode_switch`。
2. 重跑两者（设置窗测试 ⑨ 块产出 `config-effect-*` 关/开 PNG；模式切换测试
   产出 `layout-chat-*` 主题对）。
3. 断言每对关/开**像素确有差异**（`compare -metric AE > 0`）——开关无可见
   效果即流水线失败（回归门禁）。
4. `ffmpeg xfade` 将每对合成为 GIF（交叉淡入、调色板优化）。
5. CLI 开关捕获真实 `serve --uplink-port` 输出渲染为终端风格帧（本环境无
   asciinema，改用真实命令输出渲染）。

产物落在 `docs/src/public/screenshots/` 与 `docs/src/public/screenshots/gifs/`。
