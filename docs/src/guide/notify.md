# 通知子系统与 webhook 接入（T4.10）

> 桌面端已落地；手机端推送已随 Android 端落地（T6.3：普通＝仅站内、重要＝横幅＋声音＋震动、紧急＝常驻需「确认收悉」，对齐桌面三级语义）——iOS 端同一三级语义经 APNs／本地通知实现（T6.4 代码-only＋CI，真机 APNs 验证待真机）；鸿蒙手机版代码-only＋CI（T6.6，编译腿待 OHOS 工具链）。

## webhook 接入

外部系统（OA、监控、值班平台）经 HTTP 向 Memex 发站内通知。服务端默认在消息端口
（24360）之外的 **24361** 独立监听 webhook（`serve --webhook-port N` 可改，`0`＝关闭；
端口被占只降级为「接入未启用」，消息主通道不受影响）。

### 建 webhook（按群或个人独立 token）

```bash
# 个人：目标＝账号；群：目标＝group:<群号>（建即校验目标存在）
memex_server webhook create --target wanghao --name 值班平台 --db memex-server.db
memex_server webhook create --target group:3 --name 公告广播 --db memex-server.db
# token＝whk_<随机>，仅创建时显示一次；台账只存 sha256 摘要
memex_server webhook list    --db memex-server.db   # id/目标/备注/摘要/创建时间/状态
memex_server webhook revoke <id> --db memex-server.db   # 吊销后同 token 一律 401
```

### JSON payload 与 curl 示例

`POST /hook/<token>`，`Content-Type: application/json`：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `title` | 是 | 标题（非空） |
| `content` | 是 | 正文（非空） |
| `urgency` | 否 | `normal`（默认）／`important`／`urgent` |
| `jump_url` | 否 | 跳转链接，随正文归档并进弹窗 |
| `target` | 否 | 显式目标；给了必须与绑定一致，否则 403 |

```bash
curl -X POST http://服务器:24361/hook/whk_xxxxxxxx \
  -H 'Content-Type: application/json' \
  -d '{"title":"紧急通知","content":"机房割接，请立即确认","urgency":"urgent","jump_url":"https://oa.local/change/123"}'
# 200：{"msg_id":"…","ok":true,"recipients":2}
# 400 缺字段／坏 urgency｜401 错或已吊销 token｜403 target 不一致｜404 目标不存在｜405 非 POST
```

调用即投递：目标在线即时收 NOTICE 帧，离线入队登录补投；全量归档
（类型 `notice`，正文形态＝`标题：正文[ 跳转]`，与客户端本地缓存同源生成，`messages` CLI 可前后对账）。
群目标＝全体成员扇出（含群主），与群公告同级。

## 紧急程度三级分级推送（桌面端）

| 级别 | 行为 | 默认 |
| --- | --- | --- |
| 普通 | 仅站内会话消息（「通知」会话内可见，不弹任何窗） | 弹窗关 |
| 重要 | 桌面通知强提醒（托盘气泡，不看窗口激活态） | 开 |
| 紧急 | 置顶弹窗，须点「确认收悉」才关；多条排队逐个确认 | 开 |

![紧急通知置顶弹窗（需确认收悉）](/screenshots/notify-urgent-dialog.png)

## 个人通知偏好

设置菜单／托盘菜单 →「通知偏好」：

- 三级弹窗开关（上表默认值可改，关＝仅站内消息）；
- **免打扰时段**：区间可跨零点（如 22:00–08:00）；期间普通／重要静默，
  **紧急不静默**——需确认收悉的提醒不因免打扰丢失；
- **全屏／演示模式策略**：默认「递延弹窗」（全屏期间进队列，退出全屏后按当时偏好补弹），
  可改「照常弹窗」。

## 验收记录（2026-10-03）

- `webhook` 测试（服务端核心直链，真实 TCP HTTP 往返）：鉴权／payload／目标 15 类坏参
  与正例、在线即投＋ACK 清队、离线补投、群扇出、归档对账——全绿；
- `notify` 测试（客户端，进程级真实服务端）：裁决矩阵、偏好落盘往返、执行体
  （重要托盘信号／紧急真弹窗抓图＋确认关闭）、全屏递延补弹、设置页交互、
  全链路（CLI 建 webhook → HTTP POST 三级 → 引擎 → 弹窗，群扇出双引擎，本地库同源对账）——全绿；
- 人工走查：`webhook create` → `serve --webhook-port` → curl 三级各 200 带 `msg_id`，
  错 token 401／坏 urgency 400，`messages` 前后 0→3 条、类型 `notice`、正文同源——通过；
  弹窗真实弹出截图见上。
