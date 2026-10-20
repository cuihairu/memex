# Notification Subsystem and Webhook Access (T4.10)

> The desktop client has landed; mobile push has landed with the Android client (T6.3: normal = in-app only, important = banner + sound + vibration, urgent = persistent until 「确认收悉」, aligning with the desktop's three-tier semantics) — the iOS client implements the same three-tier semantics via APNs / local notifications (T6.4, code-only + CI; real-device APNs verification awaits a real device); the HarmonyOS mobile client is code-only + CI (T6.6; the compile leg awaits the OHOS toolchain).

## Webhook Access

External systems (OA, monitoring, on-call platforms) send in-app notifications to Memex over HTTP. By default the server listens for webhooks on a dedicated **24361** port, separate from the message port (24360) (`serve --webhook-port N` to change, `0` = disabled; if the port is occupied it only degrades to "access not enabled", and the main message channel is unaffected).

### Creating a Webhook (Independent Token per Group or Individual)

```bash
# 个人：目标＝账号；群：目标＝group:<群号>（建即校验目标存在）
memex_server webhook create --target wanghao --name 值班平台 --db memex-server.db
memex_server webhook create --target group:3 --name 公告广播 --db memex-server.db
# token＝whk_<随机>，仅创建时显示一次；台账只存 sha256 摘要
memex_server webhook list    --db memex-server.db   # id/目标/备注/摘要/创建时间/状态
memex_server webhook revoke <id> --db memex-server.db   # 吊销后同 token 一律 401
```

### JSON Payload and curl Example

`POST /hook/<token>`, `Content-Type: application/json`:

| Field | Required | Description |
| --- | --- | --- |
| `title` | Yes | Title (non-empty) |
| `content` | Yes | Body (non-empty) |
| `urgency` | No | `normal` (default) / `important` / `urgent` |
| `jump_url` | No | Jump link, archived with the body and shown in the popup |
| `target` | No | Explicit target; if given it must match the binding, otherwise 403 |

```bash
curl -X POST http://服务器:24361/hook/whk_xxxxxxxx \
  -H 'Content-Type: application/json' \
  -d '{"title":"紧急通知","content":"机房割接，请立即确认","urgency":"urgent","jump_url":"https://oa.local/change/123"}'
# 200：{"msg_id":"…","ok":true,"recipients":2}
# 400 缺字段／坏 urgency｜401 错或已吊销 token｜403 target 不一致｜404 目标不存在｜405 非 POST
```

Call = deliver: an online target receives a NOTICE frame immediately, an offline target is queued for redelivery at login; everything is archived (type `notice`, body form = `标题：正文[ 跳转]`, generated from the same source as the client's local cache, reconcilable before and after via the `messages` CLI).
A group target fans out to all members (including the group owner), at the same level as a group announcement.

## Three-Tier Urgency Push (Desktop)

| Tier | Behavior | Default |
| --- | --- | --- |
| Normal | In-app conversation message only (visible in the 「通知」 conversation, no popup of any kind) | Popup off |
| Important | Strong desktop notification (tray bubble, regardless of window activation state) | On |
| Urgent | Pinned popup that only closes on 「确认收悉」; multiple messages queue and are confirmed one by one | On |

![紧急通知置顶弹窗（需确认收悉）](/screenshots/notify-urgent-dialog.png)

## Personal Notification Preferences

Settings menu / tray menu → "Notification Preferences":

- Three-tier popup switches (the defaults in the table above can be changed; off = in-app messages only);
- **Do-not-disturb hours**: the range can cross midnight (e.g. 22:00–08:00); during the period normal/important are silenced, but **urgent is never silenced** — reminders requiring acknowledgment are not lost to do-not-disturb;
- **Fullscreen / presentation mode policy**: default "deferred popup" (queued during fullscreen, replayed per the then-current preferences after exiting fullscreen), changeable to "popup as usual".

## Event Notification Switches and Alert Sounds (2026-10-08 User Directive)

The lower half of the "Notification Preferences" settings page has two sections:

**Event notifications** (each an independent switch; the notification window shows the conversation name + summary):

| Event | Default | Description |
|---|---|---|
| Contact comes online → popup notification window | On | Compares presence before/after the push and pops for each newly appeared account; "I come online → notify other contacts" has no local switch — my coming online reaches the other side via the existing presence broadcast, and whether to pop is decided by the **other side's** switch (popup adjudication is physically on the receiving side; sender-granular control would require a protocol extension carrying per-user preferences upstream — not done) |
| Message received → popup notification window | On | Direct / collaboration / group messages share this switch; only when the window is inactive (no interruption while active) |
| File received → popup notification window | On | Split into a separate switch from messages (different event sources are adjudicated independently); same activation gate |
| File transfer succeeded → popup notification window | Off | Success is the norm and is not surfaced per occurrence; failures go to the always-visible status bar |

**Alert sounds** (a separate settings item, independent of the popup switches — with popups off, sounds can still play per tier):

- Three tiers: **all off (default) / play for every one / per event type**;
- "Per event type" expands into message / file / online, each adjudicated independently;
- Sound output currently uses the `QApplication::beep()` system beep (zero Qt Multimedia dependency);
  real audio files and replaceable alert sounds go through Qt Multimedia, approved separately.

## Acceptance Record (2026-10-03)

- `webhook` test (direct server-core link, real TCP HTTP round trip): authentication / payload / target — 15 kinds of bad parameters plus positive cases, online immediate delivery + ACK queue clearing, offline redelivery, group fan-out, archive reconciliation — all green;
- `notify` test (client, process-level real server): adjudication matrix, preference persistence round trip, executors (important tray signal / urgent real popup screenshot + acknowledgment close), fullscreen deferred replay, settings page interaction, full chain (CLI creates webhook → HTTP POST three tiers → engine → popup, group fan-out with dual engines, local database same-source reconciliation) — all green;
- Manual walkthrough: `webhook create` → `serve --webhook-port` → curl each of the three tiers returns 200 with `msg_id`, wrong token 401 / bad urgency 400, `messages` before/after 0→3 entries, type `notice`, same-source body — passed;
  the real popup screenshot is above.
