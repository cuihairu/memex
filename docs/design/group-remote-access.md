# 群远程访问授权：memex 只做授权关系，通道归 cockpit（设计 2026-10-15）

> 状态：**设计稿**（2026-10-15 用户设计单，本轮只落档不写码）。分工与红线
> 由用户单拍死；接口契约是**草案**，标注「待 cockpit 确认」后方可进入实现。
> 相关：权限模型（docs/design/权限模型.md，群角色/档序引擎/能力门/台账）、
> R16 远程协助（已实现，点对点 consent，见 todo 缺口盘点§五——与本设计是
> 两个功能：R16 管「帮我看屏幕」，本设计管「群里授权谁能连哪台机器」）。

## 0. 分工与红线（用户单核心，不得打破）

| 层 | 归属 | memex 是否实现 |
| --- | --- | --- |
| 远控通道 SSH/RDP/VNC（Guacamole 栈） | **cockpit**（guacd 字节管道＋票据＋出口策略＋录制） | ❌ 绝不实现 |
| 凭据保管、策略确认、host 建议 | **persona**（vault＋agent 策略引擎） | ❌ 绝不实现 |
| **群内授权关系**（谁、被授权、哪台、什么权限） | **memex** | ✅ 一期唯一要做的 |
| 聊天窗口内嵌会话 UI（仅呈现，通道来自 cockpit） | memex | 二期 |

**现状依据（2026-10-15 核实，非猜测）**：cockpit 三协议已全部走
Guacamole（`/api/remote/guacamole` → guacd；Telnet 走 agent raw TCP 兜底），
设计＋真机实测见 cockpit `docs/remote-access-integration-design.md`；
persona 已有 SSH host 管理与策略引擎（`PERSONA_AGENT_REQUIRE_CONFIRM`/
`ENFORCE_KNOWN_HOSTS`/`MIN_INTERVAL_MS`/`CONFIRM_ON_*`，persona/TODO.md
SSH Agent 节）与外部 token/listener 接入面（persona/docs/
EXTERNAL_INTEGRATION.md）；memex 侧语音通话/屏幕共享帧流地基没有
（todo 缺口盘点§五：视频会议/语音通话未实现，屏幕共享仅 R16 的 JPEG 轮询）。

### 红线：memex 全链路不接触凭据

- memex 的授权记录**只承载**「谁、被授权、哪台、什么权限」；**不存储、
  不转发、不中转、不在日志/审计里出现**任何密码/私钥/token。
- 主机登记只存地址/端口/名称/协议可用性，**永不存连接凭据**——凭据由
  persona 保管、cockpit 建会话时自行注入（cockpit 票据的
  `username/password/private_key` 字段对 memex 是黑盒）。
- 本设计的接口契约（§5）**任何方向都不出现凭据字段**；实现时的 schema
  review 把「新增字段是否可能携带凭据」当检查项。
- 依据：权限模型§4 已有同款边界（「凭据永不因入群而明文可见——权限=
  可行使动作，凭据仍走加密面」），本设计把该边界从「群密码箱」推广到
  「远程访问授权」。

## 1. 授权模型

### 1.1 实体与关系

```
群(Group) ──登记──► 主机(RemoteHost，组内资源)      群(Group) ──成员──► 账号
   └──────────── remote_grants（授权行）───────────────┘
              (group_id, account, host_id, level)
```

- **RemoteHost**（组内主机登记）：`host_id`、`group_id`、名称、地址、端口、
  协议可用性（ssh/rdp/vnc 布尔，供 UI 只放行该主机真开的协议）。登记人
  群主/管理员；**不含任何凭据字段**（红线）。
- **remote_grants**（授权行）：`group_id` + `account`（被授权成员，必须在
  群内）+ `host_id` + `level`（权限级别）+ `expires_at`（可空＝永久）。

### 1.2 权限级别（四值，同主机多行叠加）

| 级别 | 含义 | 映射到 cockpit 通道（待 cockpit 确认） |
| --- | --- | --- |
| `ssh` | 可发起 SSH **交互终端** | guacd ssh（protocol=ssh） |
| `exec` | 可**单命令执行**（非交互） | cockpit agent exec 通道 |
| `view` | **只读桌面**（看屏，无键鼠输入） | guacd rdp/vnc ＋ read-only |
| `desktop` | 可发起 RDP/VNC **交互桌面**（含键鼠） | guacd rdp/vnc |

**粒度理由**：
1. **通道是正交能力，不能做单一有序级别**。「可以跑命令」与「可以看桌面」
   互不蕴含——若用级别 1<2<3 排序，授权「只许 SSH」必然捎带桌面或反之，
   违反最小权限。四值各管一个通道动作，群主按需点选。
2. **view/desktop 的分界是输入不是画面**。远控最高危动作是键鼠输入（可
   敲命令、可关机），看屏本身风险低一档；排查类场景（「帮我看下屏幕上
   报什么」）只需要 view。
3. **exec/ssh 的分界是审计口径**。exec 是一次性已知命令体（cockpit 记录
   命令原文，事后可对账）；交互 shell 是任意操作（只能靠录制回放追）。
   自动化巡检类授权给 exec 即可，不必给人一个完整 shell。
4. **一行一级别，零迁移扩通道**。将来加新通道（如 Telnet、数据库网关）
   ＝新增枚举值＋新授权行，不动存量行；比位掩码省一次全量迁移。

### 1.3 与群能力门的关系

群能力清单（`group_capabilities` 现七项）**新增第八项 `remote`**：整个远程
授权工具（主机登记＋授权管理＋会话入口）按群开关。与既有七项不同，本项
**登记即默认拒**（remote 是实打实的控制权入口，安全敏感，默认姿态关闭；
与既有「未配置=现行允许」的首段步进口度有意偏离，理由留痕于此）。

## 2. 授权生命周期

| 决策点 | 拍板 | 理由 |
| --- | --- | --- |
| 谁可授权 | **群主＋群管理员**（组内）；org-admin 经 az 新规则 `remote-authz-manage`（跨群兜底，Explicit Allow 档） | 对齐既有先例（群公告/群资料＝群主/管理员可设）；跨群管理与 `branding-manage` 同构 |
| 成员可否授权 | 否（Default Deny） | 权限模型§2：成员＝使用能力，不配置 |
| 可否转授 | **一期不支持**（无委托链） | 委托链稀释问责（谁授的说不清）；需要时后续按 scope 规则扩展，不占本期 |
| 撤销后已建会话 | **memex 不杀会话**——通道归 cockpit（红线§0 分工）。memex 撤销即时生效于**下一次建连校验**（pull 模式，§5），并向 cockpit 发撤销通知，由 cockpit 决定是否终止在线会话（契约内 SHOULD 语义，待 cockpit 确认）；memex 侧会话台账标记 revoked | memex 手伸进通道＝打破分工；「新会话立即拒＋在线会话 cockpit 裁决」是边界内能做到的最强语义 |
| 退群/移出群 | 该群全部授权行级联失效（查询时过滤＋物理删除二选一：**拍板查询时过滤**，行保留供审计追溯） | 权限模型「退群即失」；留行不留权 |
| 主机删除 | 该主机全部授权行同上失效（查询过滤） | 同上 |
| 过期 | `expires_at` 到点自动失效（sweep 扫描，非查询时懒判——审计要记 expire 事件） | 临时授权（值班窗口）常见；事件要留痕 |

## 3. 存储与审计

memex 服务端库四张新表（文件面 24561，随既有 schema 演进）：

```sql
remote_hosts   (host_id PK, group_id, name, address, port,
                ssh_ok, rdp_ok, vnc_ok, created_by, created_ms)
remote_grants  (grant_id PK, group_id, account, host_id, level,
                granted_by, granted_ms, expires_at NULL,
                revoked_ms NULL, revoked_by NULL)
remote_authz_audit (id PK, ts_ms, actor, action, group_id, host_id,
                target_account, level, detail)
remote_sessions    (session_id PK, grant_id, group_id, account, host_id,
                protocol, mode, started_ms, ended_ms NULL,
                end_reason NULL, revoked INT DEFAULT 0)
```

**审计字段口径（授权变更全留痕）**——`remote_authz_audit` 一行一事件，
`action ∈ {host_add, host_update, host_remove, grant, revoke, grant_expire,
capability}`，`detail` 记变更前后差（如 level 变更的前后值）；**永不记
凭据**（红线）。`remote_sessions` 由 cockpit 回调写入（§5），与 R16 的
assist 审计同构（`files_client.cpp:1000-1124` 先例）。留痕铁律对齐
todo§零：审计只增不改不删，群主/管理员可查本群、org-admin 可查全部。

## 4. 外部边界接口契约（草案，待 cockpit 确认）

形态：memex 文件面 HTTP JSON（24561），前缀 `/files/remote/`，与
`/files/assist/*` 同构。**所有请求/响应无凭据字段**（红线）。鉴权方式
（cockpit 用服务账号走 FilesClient 登录，还是 org-admin 发长效 token）
**待 cockpit 确认**。

### 4.1 memex → cockpit 的建连入口（二期用，一期只定形）

memex 客户端拿到授权后向 cockpit 要会话，memex 传「人＋机＋权限」，
cockpit 自行向 persona 取凭据：

```
cockpit: POST /api/remote/sessions/memex        （待 cockpit 确认）
  { grant_id, account, host_id, protocol, mode }   ← 无任何凭据字段
→ { session_url, ticket, expires_in }
  ticket 一次性短时效（cockpit 既有 5 分钟口径不变）
```

### 4.2 cockpit → memex 的校验与回调（一期实装，契约先行）

```
memex: POST /files/remote/authz/check
  { group_id, account, host_id, protocol, mode }
→ { allowed: bool, grant_id?, level?, expires_at?,
    reason: "allow:<level>" | "deny:<reason>" }     ← 档序词汇同权限模型§5

memex: POST /files/remote/authz/audit              ← cockpit 会话回调
  { event: "session_started" | "session_ended",
    session_id, grant_id, end_reason? }
```

- **pull 校验在建连时点执行**＝撤销对「下一次建连」即时生效（§2）。
- `mode ∈ {interactive, view, exec}` 与级别映射：interactive→ssh/desktop、
  view→view、exec→exec；level 不含对应动作即 deny。
- 撤销通知：memex 在 revoke 时**反向推送**一条
  `POST {cockpit}/api/remote/authz/revoked`（body: grant_id；地址与端点
  **待 cockpit 确认**），cockpit 按需终止在线会话并回 session_ended。

### 4.3 明确不进契约的东西

凭据（任何形态）、persona token、cockpit 内部 RBAC 细节、录制/出口策略
——全在各自仓内闭环，memex 只认 grant_id 语义。

## 5. 分期与不做

- **一期（本设计范围）**：授权模型＋存储＋审计＋管理/校验 HTTP 面
  （`/files/remote/` 的 grant/revoke/list/check）；客户端管理 UI 不做
  （curl/CLI 可操作，契约可验证）。
- **二期**：聊天窗内嵌会话 UI（iframe/WebView 挂 cockpit session_url，
  仅呈现）；主机登记从 cockpit agent 清单同步（待 cockpit 确认）；授权
  管理对话框；文件传输（走 memex 既有直连文件通道，与远控通道无关）。
- **不排期（地基没有）**：音视频类（语音通话/视频会议/实时屏幕流）——
  依赖采集/编码/帧流地基，待地基立项后另单（todo 缺口盘点§五已登记）。
- **memex 永不做**：SSH/RDP/VNC 协议链路、guacd 集成、凭据保管/注入、
  出口策略/录制（§0 分工表）。

## 6. 对齐检查（实现前必过）

1. 契约草案经 cockpit 确认（端点名/参数/鉴权方式/撤销推送方向）；
2. persona 侧确认「cockpit 建会话时取凭据」的接入形态（persona 已有
   token/listener 面，属 cockpit↔persona 双边，memex 不介入细节）；
3. az 引擎新增 `remote-authz-manage`、`group_capabilities` 新增 `remote`
   项的 schema review（含红线检查项：新表/新接口字段无凭据承载）；
4. 审计台账接留痕铁律复查（只增不改不删）。
