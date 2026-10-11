# 服务端授权盘点（平台-1）

> 对应 todo「服务端授权专项审查」：逐敏感操作盘点「服务端判权＋明确拒绝路径＋回归测试」
> 三件套，输出物是盘点表与补漏测试，不做引擎重构。

## 引擎口径

判权统一走 `AuthorizationService`（`server/src/authz.cpp`）：`authorize(subject, action,
resource, context)` 单一入口，三档规则按 **ExplicitDeny > ExplicitAllow > Inherited**
求值（冲突档内取最近作用域），无命中落 `default-deny`——白名单，不是黑名单。规则谓词
在裁决时刻现查 `ServerStore`（群成员、汇报链路、角色有效期都是当次查询，不缓存），因此
撤权与转岗即时生效。四个面同引擎同径：协作协议面（`server.cpp`/`session.cpp`）、文件面
（`files_server.cpp`）、后台面（`admin/src/admin_app.cpp`）、检索导出（`messages --as`）。

## 三件套判定

| 面 | 判权点 | 拒绝路径 | 回归测试 |
| --- | --- | --- | --- |
| 协作：群创建／拉人 | `group:create`＋成员关系 | `GROUP_RESULT ok=false`＋原因 | `test_group.cpp` |
| 协作：群消息 | 成员现查（发送者） | 静默丢弃不归档 | `test_group.cpp`（非成员/解散后） |
| 协作：消息发送者身份 | 绑定会话身份（本轮补） | 拒收＋留痕日志 | `test_authz_audit.cpp` |
| 协作：撤回 | `original_from == account_` | 静默丢弃＋日志 | `test_archive.cpp` |
| 协作：送达／已读事件 | 收件人现查（本轮补） | 拒收＋留痕日志 | `test_authz_audit.cpp` |
| 协作：设备闸 | 登录时设备台账 | `LOGIN_RESULT ok=false`＋原因 | `test_accounts.cpp` |
| 协作：直连文件授权 | 统一四问（同部门/跨部门/放行/再转发/收方未知） | `FILE_AUTHZ_RESULT allowed=false` | 引擎腿 `test_authz.cpp` |
| 文件：上传/下载/删除/置顶/配额 | `file:*` 规则＋能力门 | HTTP 403＋reason | `test_files_api.cpp` |
| 文件：群文件退群失权 | 群成员每请求现查 | HTTP 403（本轮补腿） | `test_files_api.cpp` |
| 文件：个人/群备忘录、密码箱、群工具、CI、凭据面 | `memo:*`/`vault:*`/能力门 | HTTP 403 | `test_files_api.cpp` |
| 文件：任务/审批/报表/审计 | `task-assign`/`approval-decide`/`report-read`/`audit-read`（含自决 ExplicitDeny） | HTTP 403（拒前于存在性检查） | `test_files_api.cpp` |
| 文件：令牌作用域（内网/上链双闸） | token 作用域比对 | HTTP 401/403/404 | `test_uplink.cpp` |
| 文件：群助手 / webhook 接入 | token＋群组白名单＋成员关系 | 401/403/404 | `test_bot.cpp` / `test_webhook.cpp` |
| 后台：账号/组织/角色/策略/留存/设备 | `admin:manage`→org-admin（每请求重算 effective_roles） | 403＋拒绝日志 | `admin/tests/test_admin_api.cpp` |
| 后台：归档检索导出 | `audit:read`→auditor（org-admin 不自动可读，平台-6 红线） | 403＋`audit.denied` 落库 | `admin/tests/test_admin_api.cpp` |
| 检索 CLI：`messages --as` | auditor 凭据校验 | 退出码＋`audit_reads` 留痕 | `test_visibility.cpp` |
| 引擎自身 | 三档优先级／最近作用域／default-deny | 无命中即拒 | `test_authz.cpp` |

## 本轮补漏（四处代码＋八条测试）

| 洞 | 修法 |
| --- | --- |
| 协作面 TEXT/NUDGE 的归档与投递发送者取客户端自填 `from`，从不比对会话身份（撤回、文件授权都用 `account_`，只此两口） | `session.cpp` 两入口加绑定闸：`msg.from() != account_` 拒收并留痕（口径同「越权撤回被拒」）。去重键 `sha256(from:seq)` 随之绑定会话身份 |
| 非收件人可对他人消息记 `delivered`/`read` 事件并给原发送方推假 `DELIVER_NOTICE`/`READ_NOTICE`（队列清退本就被 `to_account` 限定，只有事件台账与通知面漏检） | `session.cpp` ACK/READ 走 `message_by_id` 取 `to_account`，`is_recipient()` 现查（单聊＝本人；群消息＝群成员），非收件人拒不留痕不通知 |
| 退群后群文件访问无回归腿（群成员判定实时，但「退群即失」从未被测试覆盖） | `test_files_api.cpp` 补腿：`group_leave` 后下载 403、恢复成员后 200 |
| 设备停用后行为无任何测试钉住 | `test_authz_audit.cpp` 钉现状：存量会话不受影响，同指纹新登录被拒 |

## 已知口径与待拍板（盘点如实记录，非缺陷清单）

1. **账号没有「禁用」控制**：`accounts` 表无 `enabled` 列，`credentials.disabled` 存在但
   全仓无任何写入路径。当前唯一的撤销杠杆是设备台账 `devices.enabled`。要「禁用账号」
   需加列＋写入面＋登录/会话校验面，属产品决策面。
2. **撤销只挡新登录**：设备停用不踢存量协作会话（会话状态 `logged_in_` 登录后一次置位），
   也不失效已签发的文件面令牌（进程内存表＋12h TTL，重启即清）与后台会话。后台面例外：
   角色每请求重算，撤权即时生效。是否让「停用设备＝踢存量会话」需服务端配合（后台是独立
   进程，kick 只能由服务端感知），属决策面。
3. **文件面令牌签发不查设备**：`/files/session` 只验凭据，不设 `new_device_approval`
   闸（协作登录两处都查）——文件面令牌无设备绑定语义，泄漏的令牌可用至 TTL 结束。
4. **后台部门树读取只挡登录**：`GET /api/admin/departments` 不做 `require_manage`，
   任何登录账号可看组织树（写操作全部门控）。口径＝组织目录对登录成员可见，同通讯录面。
5. **收件人不校验存在性**：直连 TEXT 与 NUDGE 的 `msg.to()` 不做账号存在/关系校验，
   对不存在账号的消息照常归档入队（孤儿行）。垃圾与孤儿归档量级问题，非越权读取。
6. **撤回转发目标客户端自选**：撤回本身经发送方校验，但通知扇出用客户端填的
   `msg.to()`（可指向第三方）。
7. **CLI 无授权门**：`main.cpp` 各子命令直接开同一 SQLite 库操作，本地运维信任模型
   （文件权限即边界）；唯一例外是检索导出面强制 auditor 凭据并落 `audit_reads`。
8. **登录前帧静默丢弃**：协作面对未登录帧不返错误帧，无审计留痕（无凭据无从归因）。

## 测试索引

`server/tests/test_authz.cpp`（引擎）、`test_accounts.cpp`（设备闸与登录）、
`test_archive.cpp`（归档与越权撤回）、`test_group.cpp`（群关系与权限载体）、
`test_files_api.cpp`（文件面全链 401/403/404）、`test_uplink.cpp`（令牌作用域双闸）、
`test_webhook.cpp` / `test_bot.cpp`（接入面）、`test_authz_audit.cpp`（本轮补漏）、
`admin/tests/test_admin_api.cpp`（后台面 19 腿）。
