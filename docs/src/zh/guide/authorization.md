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

## 企业目录认证（LDAP/AD）

todo 544（外部建议三步走第②步；③ OIDC 待明确需求）：可插拔认证后端，不改轻量
部署与免登录直连的独立信任模型——**不配 `--ldap-host` 零 LDAP 路径**（构造都不
发生），本地账号（平台-2 口令）常备＝独立内网与应急恢复入口。

红线口径（实现 `server/src/ldap.hpp`/`ldap.cpp`，最小 BER 客户端自写，无 openldap
依赖）：

| 红线 | 落法 |
| --- | --- |
| 目录不可用不绕过认证 | fail-closed：绑定 issuer=ldap 的账号在目录不可达/未配置/配置半齐时一律拒登（`LdapUnavailable`），绝不回落本地口令——回落即绕过目录。错误明示入日志与拒绝原因，不静默降级 |
| 密码不明文 | 口令只在 bind 报文走一次：不落盘、不进日志/LoginRecord/审计正文（审计只记谁/何时/方式/成败，LDAP 登录成功记 `ok_ldap`）。TLS 三态恰一：`--ldap-ssl`（ldaps 整连接）/`--ldap-starttls`/`--ldap-allow-plain`（默认拒明文，启动校验违者退出码 2）；自签证书 `--ldap-tls-insecure`（启动警告） |
| 组→角色显式映射 | `--ldap-group-role 组DN=角色名`（可重复；env `MEMEX_LDAP_GROUP_ROLE` 逗号分隔）。映射表外组一律忽略＝普通成员；表为空＝不做任何自动授权；表不含 admin 就永不授管理员——目录管理员组不隐式提权（显式映射含 admin 时启动警告） |
| 目录禁用 | 新登录直接拒（服务模式绑定前查禁用三元组＝服务权限口径；AD 停用位 `userAccountControl`+规则 `1.2.840.113556.1.4.803`+值 `2`，属性可配）。存量会话：`--ldap-kick-disabled`（默认 1＝踢）＋`--ldap-recheck-seconds`（默认 60s）复检，affirmative Disabled 才踢——目录不可达不误踢 |
| 首登身份绑定 | identities 表：issuer=`ldap://<host>`，subject=用户 DN（模板模式为转义后绑定 DN）。默认不自动绑定：`--ldap-auto-bind`（已有账号无绑定→目录认证成功即绑）、`--ldap-auto-provision`（账号不存在→建影子账号，本地口令随机不可知＝仅目录可登）都默认关；关闭时只有 `identity bind` CLI 显式绑定的账号走目录判登 |
| 本地账号常备 | 未绑定 issuer=ldap 的账号走原本地判登（目录零触达，不做任何探测）；目录面故障不影响本地账号与免登录直连 |

绑定路径二选一：模板直绑（`--ldap-bind-dn-template uid={u},ou=...`，OpenLDAP
常用）或服务账号先查后绑（`--ldap-bind-dn`+口令，AD 常用——DN 拼不出须先定位
`--ldap-user-base`/`--ldap-user-attr`）。服务账号口令走 env
`MEMEX_LDAP_BIND_PASSWORD` 注入（不进 shell 历史/ps；配置文件须 0600）。

熔断：一次目录不可达后 30s 冷却 fail-fast（目录挂掉时不让每次登录拖住协作会话
一个超时时长）。目录不可用判定与组映射口径的完整使用者视角见
`packaging/ldap.env.example`（带注释示例配置）。

测试：`server/tests/test_ldap.cpp` 16 腿（假目录＝进程内 POSIX 监听，脚本化
bind/search 应答）：TLS 姿态、服务/模板双模式线上核对、口令错 49、查无此人、
禁用命中（用户绑定不发生）、不可达＋熔断 fail-fast、DN/过滤器转义、路由矩阵
（未绑定默认 Local 零触达／绑定→LdapOk／口令错→LdapBad／目录未配→Unavailable）、
auto_bind/auto_provision 默认关、组未映射零授权、会话级真登录链（成功＋角色
同步／fail-closed 拒登／禁用拒／本地账号照常）、存量会话踢下线。LDAP 不可用/
组未映射/目录禁用三类必测用例分别对应腿 6/13/5+16。

## 测试索引

`server/tests/test_authz.cpp`（引擎）、`test_accounts.cpp`（设备闸与登录）、
`test_archive.cpp`（归档与越权撤回）、`test_group.cpp`（群关系与权限载体）、
`test_files_api.cpp`（文件面全链 401/403/404）、`test_uplink.cpp`（令牌作用域双闸）、
`test_webhook.cpp` / `test_bot.cpp`（接入面）、`test_authz_audit.cpp`（本轮补漏）、
`test_ldap.cpp`（企业目录认证 16 腿）、
`admin/tests/test_admin_api.cpp`（后台面 27 腿）。
