> [English](oidc-identity-integration.md) | **中文**

# OIDC 身份认证接入设计（设计稿，未实施）

> 状态：本文档只做设计，未改任何代码；等用户看过拍板后再排实施。
> 对应计划书 docs/design/基础平台化改造计划（身份-组织-授权-归档）.md §二
>（Identity/IdentityBinding 已预留位）与 §三（Authorization）。
> 用词纪律：只用现成词（OIDC/PKCE/claims/groups/sub/RP/IdP/discovery/JWKS/
> id_token/access_token），不造词；拿不准处标「待定」。

## 一、定案

**L2 定案（用户 2026-10-12 拍板）：部署自带一个轻量 IdP 容器（compose 内）
当默认身份源，memex 只做 OIDC client（RP），不自实现 provider。**

紧随定案的一句口径：**「默认 = 传统账号密码登录；OIDC = 可选路径（配置了
IdP 才启用）」**——不改现有默认体验，未配置 IdP 的部署与今天完全一致。

为什么只做 client、不自实现 provider：

- 自实现 provider 要自己扛起签发面的全部安全责任：密钥生成与保管、JWKS
  发布与轮换、token 签名算法与时效、吊销、审计——每一件都是线上安全面，
  做错一件就是身份面失守；
- 标准 client 的责任面只有三件套（见 §二分工表）：discovery、JWKS 验签
  （iss/aud/exp/nonce）、sub+groups 映射——验签是「只验不签」，出错的
  后果是「拒绝登录」而不是「错发身份」；拒绝登录是可恢复故障，错发身份
  是安全事故；
- 组与账号生命周期管理（建组、拉人、踢人、改组）是 IdP 的成熟管理面，
  自实现等于再造一个组织管理后台。

L3（memex 自己当 issuer 给外部系统发号）**只记录不排期**：真出现「外部
系统要向 memex 发号」的需求再做（届时需补齐密钥管理、轮换、JWKS 发布等
provider 责任面）。

## 二、分工表

三方各管一段，边界按 token 流向切：

| 事项 | 客户端（RP 侧） | memex 服务端 | IdP |
|---|---|---|---|
| 登录交互 | 系统浏览器跑 authorization code + PKCE（public client，redirect_uri=127.0.0.1 回环口，RFC 8252 口径） | — | 展示登录页、发 authorization code |
| 换 token | code + verifier 向 IdP 换 id_token（无 client_secret，public client） | — | token 端点发 id_token |
| 验签 | — | discovery（启动时取 issuer 配置）+ JWKS 缓存验签：签名/iss/aud/exp/nonce，全过才受理 | 签发与发布 JWKS |
| 身份映射 | — | (iss, sub)→account 绑定；groups→部门/群映射 | claims 内容的权威来源（sub/email/name/groups） |
| 会话 | 持 memex 会话（与密码登录后的会话同一体） | 自签会话自管（§三） | — |
| 组管理 | — | 只读消费 groups claim | 建组、拉人、踢人的管理面 |

要点：memex 服务端**不做** per-request 打 IdP——验签只发生在登录受理时，
在线会话与 IdP 之间没有持续依赖（§三）。

## 三、服务端会话与 token 生命周期

这块最容易出线上问题，单独成节。原则：**memex 会话是 memex 自签的，
寿命与 IdP token 解耦**。

- **登录换会话**：客户端送来 id_token → 服务端三件套验签通过 → 查绑定得
  account → 走既有登录建档（设备指纹/新设备审批/登录记录/同类互踢，全部
  现行为不变）→ 发 memex 会话。此后连接上只有 memex 会话，id_token 用完
  即弃。
- **access token**：客户端从 IdP 拿到的 access token 在 memex 业务面上
  **无用途**（业务面全走自签会话），默认即弃不落盘。
- **刷新**：默认不持久化 refresh token（会话内存即弃，会话过期后重走一次
  登录交互）；要不要做静默续期待定——真有需要再评估，不排期。
- **IdP 宕机的降级行为**（口径写死）：
  - 已登录会话**一律保留**：验签只在登录受理时发生，IdP 不可达绝不成为
    踢人理由——**绝不静默把人踢下线**；
  - 新的 OIDC 登录不可用：拿不到新 id_token 或 JWKS 验不了，明确报
    「IdP 不可达」，不重试风暴；
  - **密码轨保持可用**（这正是「密码登录保留为默认路径」的兜底价值）；
  - 验签**不可跳过**：JWKS 取不到时降级到「拒绝新 OIDC 登录并明示原因」，
    绝不降级为「跳过验签放行」；
  - IdP 恢复后无需人工介入：discovery/JWKS 下次登录自动重取。

## 四、直连态边界

直连态（免服务器本机使用）没有也不需要有 IdP：

- 保留现有本地认证入口，行为不变；
- 两套入口（直连本地认证 / 服务端 OIDC 或密码登录）收敛到**同一个内部
  身份对象**（计划书§二 Identity 模型：Account/Device/Session/Credential/
  IdentityBinding）——OIDC 登录在服务端映射到既有 account 上，
  **不新建平行账号体系**；
- 上层 az 规则、归档、审计只认这个内部身份对象，不出现「oidc 用户」
  单独一支裁决路径。

## 五、身份绑定与 sub 口径

- 绑定落计划书§二已预留的 IdentityBinding：`identity_bindings(idp_iss,
  idp_sub) → account`，(idp_iss, idp_sub) 唯一约束；首次 OIDC 登录时
  绑定到既有账号（绑定确认方式待定：管理员预登记 / 首登页确认，设计期
  二选一）。
- claims 映射：sub→绑定键；email/name→既有账号字段（email 列现状待定，
  没有则只存绑定扩展）；groups→部门/群（§六）。
- **sub 作为稳定标识进归档与审计，不用用户名**：新写入的归档行/审计行
  附 sub（账号改名不影响追溯）；存量行不回填，检索按 account↔sub 联表
  口径。

## 六、授权面：az 读 groups 与兼容期

- 现状：`cross-department` 等规则按自有部门路径现查现裁
  （server.cpp `member_profile().department_path`），群规则按自有群表。
- 改造：IdP groups claim 是组事实源时，登录受理时同步到本地映射
  （groups→`ensure_department_path` 部门路径；哪些 IdP 组算 memex 群
  待定）。
- **兼容期裁决（旧部门表还在时怎么裁决）**：账号级身份源开关
  （local=旧部门表 / idp=groups 映射），**单一事实来源，不做混合加权**
  ——同一账号同一时刻只有一个事实源，谁是事实源由开关定；跨源比对
  （同账号 local 与 idp 部门不一致）按从严口径处理，冲突告警进审计。
  建议：存量账号默认 local（现行为零变化），新建/已迁移账号默认 idp；
  默认值待用户拍板。
- 验收锚点：IdP 移出组 → 下次登录同步后 az 裁决变化（如跨部门文件
  禁发）。

## 七、与密码箱的边界

- **OIDC 只解决「你是谁」**；外部工具凭据（钉钉/飞书/GitHub provider 的
  appKey/appSecret 等）照旧走密码箱（PBKDF2→KEK、AES-256-GCM 加密落盘，
  R24-3/R27-3 现口径），两者不互相替代。
- OIDC 自己的会话凭据（id_token/access_token）不入密码箱：会话内存即弃
  （§三）。
- 机器对机器（client credentials 流）：**标「以后可能要」，不做**——
  真出现服务端组件要向 IdP 证明自己的场景再评估。

## 八、部署与代价（如实列）

- **多一个常驻容器组**：IdP 本体 + 数据库（+可选缓存），内存按 GB 级估
  （选型定后实测为准，见 §九）。
- **多一份备份面**：IdP 自己的数据库要进备份预案（成对拍口径同 memex
  双资产）；备份策略待用户拍板（§十二）。
- **升级面**：IdP 是独立上游、升级节奏较快，要跟进版本并回归。
- **主密码遗忘 = 全锁死**：IdP 管理员口令丢失则身份面整体不可管理。
  **必须给本地恢复口**：部署时设紧急管理员 + 恢复手段离线保存；具体
  重置流程（recovery 流程/容器内重置命令）在选型定后实测核实后写入部署
  文档（待定：具体命令）。
- **部署形态**：1 个 compose 变 2 个（memex compose + IdP compose）；
  网络要求：memex 服务端可达 IdP（discovery/JWKS），用户浏览器可达 IdP
  （authorization 端点）。
- 双轨兜底让上述故障面可控：IdP 全挂时密码轨仍可登录（§三）。

## 九、IdP 选型对比与推荐

| 维度 | Authentik | Keycloak | Zitadel |
|---|---|---|---|
| 资源占用 | 中：server+worker+DB，GB 级内 | 重：JVM，1–2 GB 级 | 轻：Go 单二进制+DB，百 MB 级 |
| OIDC+组映射 | 默认 claims 即带 groups，映射零定制 | 组映射最强（protocol mapper），需配置 | 以 roles claim 为主，groups 口径待核实 |
| 备份难度 | 备自己的 DB（+worker 无状态） | 备 DB（配置全在 DB） | 备 DB **+ masterkey**（丢失=解锁无门） |
| 许可证 | 核心开源（MIT 口径，企业版另售） | Apache-2.0 | Apache-2.0（核心） |
| 形态 | 容器标准件，界面与文档成熟 | 容器标准件，文档海量 | 单二进制，最省资源 |

**推荐：Authentik。** 一句理由：默认 claims 就带 groups——sub+groups
映射零定制（§二分工表里服务端最薄的一档），而它的资源与备份代价用
「双轨兜底 + 备自己的 DB」可控。

第二选项：钉钉/企业微信。它们的网页登录是 OAuth 2.0 形态而非完整标准
OIDC（id_token/标准 scope 口径各家不同，待定核实），且身份数据在其公有
云——与「数据默认不出门」硬约束冲突。可行形态不是 memex 直接连钉钉
端点，而是**自托管 IdP 把钉钉扫码配成上游**（联合登录），身份数据仍落
自托管侧；Authentik 对钉钉的上游支持程度待定核实。仓里已有的钉钉/飞书
provider 是「外部待办工具凭据接入」（appKey/appSecret→accessToken），
属任务工具面，不是身份认证面，不能当 IdP 用。

## 十、分期与可测验收

| 阶段 | 内容 | 可测验收 | 拍板点 |
|---|---|---|---|
| 0 | IdP 容器部署 + 注册 public client（PKCE） | discovery 端点内网可达；浏览器可达登录页 | IdP 备份策略；具体选型复核 |
| 1 | 服务端三件套 + identity_bindings 表 | 测试 IdP 签发的 id_token 经 LOGIN 换出 memex 会话；篡改/过期/错 aud/错 nonce 一律拒；密码登录回归全绿 | — |
| 2 | 客户端 authorization code + PKCE + 登录页双轨 | 配置 IdP 后可用 IdP 账号登录成功；不配置 IdP 时入口不出现、行为与现状一致；密码登录成功；登录记录可见 OIDC 来源 | 登录页默认轨（当前定案：密码轨默认） |
| 3 | groups 映射 + az 兼容期裁决（§六） | IdP 移出组→下次登录后 az 裁决变化；旧账号绑定后历史归档/文件可达（数据不丢） | 身份源默认值；迁移批次 |
| 4 | sub 入归档与审计（§五） | 新归档行/审计行带 sub；按 sub 可检索；存量行不回填口径明确 | — |

不排期但记录在案：L3 发号（§一）、client credentials（§七）、静默续期
（§三）。

## 十一、待定清单

- 首登绑定确认方式（§五）：管理员预登记 vs 首登页确认；
- groups claim 具体口径与哪些 IdP 组算 memex 群（§六）；
- 身份源开关默认值（§六，建议存量 local/新建 idp）；
- refresh token 持久化与否（§三，默认不持久化）；
- IdP 管理员恢复口的具体命令（§八，选型定后实测核实）；
- email 落点（§五，账号表无列则入绑定扩展）；
- LOGIN 帧扩展形态（新可选字段 vs 新帧，实施期按协议兼容性定）。

## 十二、仍需用户拍板

1. **密码登录是否长期保留**——当前定案是「默认路径、双轨长期并存」；
   若未来想关（全员迁移后），单独拍板，本文档不含关闭排期；
2. **IdP 备份策略**——进 memex 现有每日备份（同窗口成对拍）还是独立
   备份窗口与保留份数。
