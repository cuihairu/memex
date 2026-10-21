> English | **中文**

# 群内知识沉淀闭环设计（客服问答知识库机器人）

> 状态：设计稿（2026-10-19 需求：群内问答知识沉淀闭环）。定位：客服群的「问答→收集→审核→复用」闭环——已有人工回答过的问题自动应答并标注来源；新问题的群内回答被收集进待审核，审核通过才升级为可自动应答的正式知识。**只出设计，不含实现。**
>
> 互引：机器人平台（bot 收发面）、群内智能助手（指令与网关回环先例）、权限模型（群能力开关/群内角色）、群组知识共享（R24 群备忘录=人工维护知识条目）、模型网关（归档红线）。

## 0. 摘要与三条铁律

本设计在既有 bot 面上叠加一个「知识管家」机器人（下称 **知识 bot**），四步闭环：

① 识别群内提问 → ② 查已确认知识库，命中即自动应答（附来源）→ ③ 未命中则监听群内人工回答 → ④ 有效回答进入待审核队列，审核通过升级为已确认知识，下次自动应答。

**三条铁律（P0，不可协商）**：

1. **未审核知识不得以事实口吻输出**。任何待审核（pending）内容被引用时必须带「⚠️ 待审核」显式标注与免责话术（§9 模板 T2/T3）；已确认（approved）内容方可直接作答。
2. **自动判定只决定「收不收」，永不决定「能不能答」**。相似度、回答有效性评分等自动信号只用于把内容送进待审核队列；只有人工审核通过才能升级为 approved（§3.3、§3.4）。
3. **知识条目默认群内闭环**。跨群复用必须显式升级（§8），且任何条目永久携带来源（群/消息/人/时间）。


## 1. 现状盘点（每条断言给 file:line）

### 1.1 群聊消息实体与存储

- **消息表 `messages`**：`msg_id`（唯一）、`from_account`、`to_account`、`type`（整数）、`text`、`ts_ms`、`recall` 七列（server/src/store.cpp:124-132）。群消息落档时 `to_account="group:<群号>"`（server/src/store.hpp:164 注释、server/src/session.cpp:257）。
- **消息类型**：单一 `MsgType` 枚举，`TEXT=10`、`NOTICE=48` 等（common/proto/memex.proto:9-53）。**没有** reply/quote、reaction/点赞、thread、system-message 类型——全枚举无此概念（common/proto/memex.proto:9-53）。
- **群表 `groups`**：`group_id`、`name`、`owner`、`announcement`、`topic`、`purpose`、`created_ms`（server/src/store.cpp:342-349）。
- **群成员 `group_members`**：`(group_id, account, joined_ms)` 复合主键（server/src/store.cpp:350-354）；后加 `role` 列（server/src/store.cpp:1055），取值 owner／admin／member（server/src/store.hpp:67），判定函数 `group_role()`（server/src/store.cpp:6772）。
- **群消息路由**：TEXT 收到后先成员校验（非成员拒收不归档，server/src/session.cpp:261-263），派生 msg_id 归档（server/src/session.cpp:271-275），再对群成员逐人 `queue_offline` 扇出＋在线直投（server/src/session.cpp:265-267、287-289）。bot 伪账号在 `group_members` 里即自动收群消息（零改动，server/src/store.cpp:399-401 注释）。
- **离线队列 `offline_messages`**：`(msg_id, to_account)` 唯一，信封 BLOB，按接收方 ACK 清队（server/src/store.cpp:115-123）；`queue_offline`（server/src/store.cpp:1631）。
- **已读/撤回/事件溯源**：`message_reads`（server/src/store.cpp:356-360）、`recall_events`（server/src/store.cpp:135-139）、`message_events` append-only 事件流（server/src/store.cpp:142-150）。
- **群能力开关 `group_capabilities`**：`(gid, capability, enabled)`，词汇表七项 notice/memo/vault/tools/server_tools/files/uplink（server/src/store.cpp:207-213、4145-4156）。
- **审批台账 `approvals`**：`status` 默认 `pending`，含 `decider`、`decision_note`、`decided_ms`（server/src/store.cpp:244-255）——本设计审核队列的表结构先例。
- **群备忘录 `group_memos`**：群维度人工维护知识条目（标题+正文）＋逐笔全文快照修订史（server/src/store.cpp:508-526；docs/design/群组知识共享.md:12）。

### 1.2 机器人与助手概念（已有）

- **bot 台账 `bots`**：`name`、`token_hash`（sha256，明文仅创建时输出一次）、`created_by`、`disabled`（server/src/store.cpp:402-407）；群/部门白名单 `bot_group_allow`、`bot_dept_allow`，名单非空才收紧（server/src/store.cpp:411-420）。
- **bot=伪账号 `bot:<name>`**：直插 `group_members`/`offline_messages`（两表无外键），加群即收 TEXT 扇出，被私聊 `to=bot:<name>` 直达（server/src/store.cpp:399-401、main.cpp:2262-2266；docs/design/机器人平台.md:38-41）。bot 消息与普通 TEXT 同链路**全量进留痕归档**（docs/design/机器人平台.md:19）。
- **bot HTTP API**：`POST /bot/send`、`GET /bot/updates`（长轮询真挂起）、`POST /bot/ack`，Bearer token 鉴权，与 webhook 共用端口（server/src/webhook.cpp:303-433；`self="bot:"+name` 在 server/src/webhook.cpp:333）。
- **群内智能助手（GroupAssistantWorker）**：内置编排器先例——独立线程轮询 `pending_offline("bot:<名>")`（server/src/group_assistant.cpp:255），识别四指令 `@助手/@纪要/@整理/@检索`（server/src/group_assistant.cpp:23-26），**非指令消息 ack 清队跳过不处理**（server/src/group_assistant.cpp:340-343）；群上下文取归档最近 N 条（server/src/group_assistant.cpp:141-153）；回群走回环 HTTP `/bot/send`（server/src/group_assistant.cpp:125-133）；模型调用一律带 `memex_archive_scope:true` 归档红线（server/src/group_assistant.cpp:173）。serve 装配 `--assistant <bot名>`（server/src/main.cpp:486-518、68-69）。设计文档：docs/design/群内智能助手.md:30-57。
- **系统消息形态**：服务端只有 NOTICE 类型（webhook 推送标题/内容/紧急程度，common/proto/memex.proto:38；信封组装 server/src/webhook.cpp:545-568）。bot 发的消息就是普通 TEXT（from=bot 伪账号），无独立 system 消息类型；自动回复先例=助手四指令回群。

### 1.3 搜索能力

- **服务端 `search_messages`**：多词子串 AND 检索——按空白/逗号分词（含全角逗号），每词独立 `LIKE` 子串命中、全部命中才出；ASCII 转小写（中文恒等），相关度=词命中次数×1000+首命中位置提前量（server/src/store.cpp:2686-2734，分词 2691-2721，相关度 2724-2734）。**无 FTS5、无倒排索引、无向量检索**（全库 grep fts/embedding/cosine 零命中，未核实未来分支）。
- **检索半径**：账号过滤天然联入「该账号所在群」的全部群消息（`group_members` 子查询，server/src/store.cpp:2744；设计口径 docs/design/群内智能助手.md:39）——bot 加群即拥有该群消息的检索半径，权限语义与「入群即继承」一致（docs/design/权限模型.md:21）。
- **客户端搜索**：`MessageSearchDialog` 跨会话检索**本机库**，子串+多词 AND+相关度，不触网（client/app/message_search_dialog.hpp:1-5；检索执行 client/app/message_search_dialog.cpp:85-97）。搜索 UX 原型见 docs/design/prototypes/desktop-search.html:78（命中分页）。
- **模型网关**：OpenAI 兼容转发＋端点路由降级＋调用审计（server/src/store.cpp:424-433 端点表含 `is_local` 位）；**归档红线**——`memex_archive_scope:true` 的请求只路由本地模型端点（docs/design/模型网关.md:38-43）。网关只做对话补全，无 embedding 接口。

### 1.4 客户端与设计文档

- **桌面端**：Qt 单体窗 `main_window`（client/app/），已有审批窗（client/app/main_window.hpp:531 懒建 `ApprovalDialog`）、群备忘录窗（client/app/group_memo_dialog.cpp）、消息搜索窗（client/app/message_search_dialog.cpp）——本设计审核 UI 与知识条目 UI 的形态先例。
- **原型**：docs/design/prototypes/desktop-group.html:97 出现「引用 …」气泡样式——**仅 UI 原型桩**，客户端工程代码无引用/回复实现（client/ 全量 grep 引用|quote 零命中，common/proto/memex.proto 无字段）。
- **移动端**：Android 会话列表＋聊天页已落地（apps/android/README.md，单聊口径为主）；iOS 有 core 面会话存储（apps/ios/README.md）；鸿蒙 core 面（apps/harmony/README.md）。**移动端群聊 UI 支持程度：未核实**（README 未提群聊）。
- **权限模型**：群内角色 owner/admin/member、入群即继承退群即失、群能力管理员配全、全程留痕（docs/design/权限模型.md:21-28、7）；能力开关停用即 403 `deny:capability-<名>`（docs/design/权限模型.md:7）。
- **相关设计稿**：群内智能助手（docs/design/群内智能助手.md，四指令＋失败口径＋装配）、机器人平台（docs/design/机器人平台.md，收发与白名单）、群组知识共享（docs/design/群组知识共享.md，公告/备忘录/密码箱）、群工具框架（docs/design/群工具框架.md:9，群=授权域）。

### 1.5 差距清单（决定本设计的保守度）

| 缺口 | 现状依据 | 对本设计的影响 |
| --- | --- | --- |
| 无 reply/引用/点赞等结构化互动信号 | common/proto/memex.proto:9-53 全枚举；client/ 无引用实现（仅原型桩 desktop-group.html:97） | §3.3 回答有效性判定只能用「文本信号＋显式指令」，不能依赖点赞/引用 |
| 无 FTS/向量检索 | server/src/store.cpp:2686-2734 仅 LIKE 子串 | §6 相似度第一期用归一化＋字符 n-gram 本地算，不引入新存储引擎 |
| 无消息编辑 | `messages` 无编辑列，撤回仅 recall 位（server/src/store.cpp:124-132） | 知识条目必须**快照**当时的问答文本，源消息撤回不影响条目，但展示时标注「源消息已撤回」 |
| bot 收信面=至少一次投递 | offline 队列 ACK 清队，重复投递靠 msg_id 幂等（server/src/store.cpp:115-123、group_assistant.cpp:279-280 注释） | 知识收集与自动应答都必须幂等（msg_id 去重） |
| 无审批推送通道 | approvals 只能客户端轮询（client/app/main_window.hpp:531 懒建窗） | 审核提醒第一期复用 bot 群发文本＋NOTICE，不做专门推送 |

## 2. 设计原则

1. **叠加不改造**：知识 bot 是又一个 bot 伪账号（与群助手同收信面），不改协议、不改消息表、不新增消息类型；全部状态进自有新表。
2. **先收集后审核再复用**：收集面可以宽（宁可多收进待审核），应答面必须严（只有 approved＋高相似才开口）。
3. **显式优于猜**：memex 无互动信号，第一期凡能靠显式指令（#采纳/#纠错）拿到的确定信号，不靠模型猜。
4. **群内闭环为默认**：相似度检索半径=提问群；跨群/跨部门是显式升级动作（§8）。
5. **红线继承**：知识 bot 走模型网关的一切调用（相似判等、问题归一）都带 `memex_archive_scope:true`，归档红线原样生效（先例 server/src/group_assistant.cpp:173）。

## 3. 闭环四步设计

### 3.1 ① 提问识别与触发：双轨制（显式应答＋静默收集）

**判断依据**：

- bot 反正收全量群消息（TEXT 扇出直插离线队列，server/src/session.cpp:265-289），「识别所有提问」在管道上零成本，成本全在判定准确率；
- 无互动信号可借力（§1.5），纯文本意图识别必有误判；客服群闲聊占比高，误答=把错答案推给客户，漏答=可补救——**误答代价 ≫ 漏答代价**；
- 现有助手已建立「@召唤」用户习惯（四指令前缀，server/src/group_assistant.cpp:23-26）。

**建议（采纳）：双轨。**

| 轨道 | 触发 | 行为 | 误判代价 |
| --- | --- | --- | --- |
| **应答轨（显式）** | `@知识 <问题>`（或复用 `@助手` 指令族加 `@查库`） | 查 approved 知识库 → 命中即答（§9 T1）；未命中回「知识库暂无，已登记本问题」并进 §3.3 监听 | 低——用户主动召唤，答错也只是没帮上 |
| **收集轨（静默）** | 全量群消息过**提问分类器**，命中「疑似提问」 | **不回群、不打扰**，仅登记 open_question 并启动 §3.3 回答监听窗口 | 零——只多一条待审核候选，由审核过滤 |

**提问分类器（第一期规则版，写死中文口径）**，命中任一即「疑似提问」：

1. 文本以 `@知识`/`@助手` 开头（显式，最高置信）；
2. 含疑问词（什么/怎么/如何/为什么/哪/吗/？/?/能不能/有没有/求/求助）且非指令非纯表情；
3. 对某人消息的追问（文本含 `@` 引用他人且带疑问词）。

**排除项**：bot 自己的消息（from 以 `bot:` 前缀，防自激循环）、NOTICE、撤回、纯 URL/文件名。是否全群开放收集轨由群能力开关 `knowledge` 控制（§10），默认关——开了收集轨的群才进静默登记。

**第二期起**分类器可升级为网关 LLM 判定（归档红线照带），规则版保留为兜底与离线口径。

### 3.2 ② 已确认知识库命中 → 自动应答（附来源）

- 触发：应答轨收到问题（或第二期自动应答开关开启时收集轨命中高相似）。
- 流程：问题归一化（去标点/全半角/大小写）→ 在 **approved 且未过期且未停用** 的条目中按相似度检索，半径=提问群（含显式升级到本群可见的跨群条目，§8）。
- 命中（相似度 ≥ 阈值高线，§6）：以 bot 身份回群，**必须带来源四元组**——某群·某时间·某人的回答＋审核人，格式见 §9 T1。同一 open_question 短窗口内只答一次（幂等：以归一化问题指纹＋群键去重）。
- 边界带（低线 ≤ 相似度 < 高线）：**不直接作答**，降级为「可能相关的待审核条目」引用（§9 T2，显式标注待审核）或提示用 `@检索` 自查（先例 server/src/group_assistant.cpp:320-339）。
- 未命中：回「知识库暂无命中」，同时把该问题登记为 open_question（含提问 msg_id/人/时间），启动监听窗口；已登记过（指纹重复）则只续期不重复回复。

### 3.3 ③ 未命中 → 监听群内人工回答，识别「有效回答」

**监听窗口**：open_question 登记后 N 小时（建议 24h，群能力可配）内，该群所有 TEXT 都进入窗口判定；窗口关闭仅留台账。

**「这是一次有效回答」判据（按信号强度分层）**：

| 层 | 信号 | 判定 |
| --- | --- | --- |
| **强（显式）** | 有人回复 `#采纳 上一条`／`@知识 采纳 <msg_id>`；或提问者回复含「谢谢/解决了/可以了/明白了」 | 直接判定该（被指向的）回答为候选答案，进待审核 |
| **中（结构）** | ① 提问中 `@` 过某人，此人随后首条实质回复；② 回答者为群 owner/admin（server/src/store.cpp:6772 可判）；③ 时间邻近（提问后 10 分钟内）；④ 实质回复=长度 ≥ 阈值且非纯表情/纯 URL | 每项计分，加权总分 ≥ 阈值 → 该回复成为候选答案 |
| **弱（内容）** | 疑问词消失、出现「是…/在…/用…」等陈述句式 | 仅作加分项，单独不足以成候选 |

**评分模型（第一期写死）**：候选分 = 显式确认(100) / 被@者回复(40) + 管理员身份(20) + 邻近(10) + 实质长度(10) + 提问者致谢(30)。候选分 ≥ 50 → 进待审核队列（附各信号明细供审核人参考）。

**关键裁剪**：信号再强也**只送审，不直接确认**（铁律 2）。误判为「有效回答」的代价=审核队列多一条噪音，由审核人丢弃；不会污染知识库。

- 一问多答：窗口内多个候选都进待审核（同 open_question 挂多条候选），由审核人合并择优（§4.1 `merged_from`）。
- 源消息撤回：候选照收（快照文本），审核界面标注「源消息已撤回」。

### 3.4 ④ 待审核队列与审核动作

- **进队**：候选问答对落成 `knowledge_entries(status='pending')` 一行，附 open_question 引用、候选分、信号明细、群/人/msg_id/时间快照。同一问题指纹已存在 pending 条目则不重复入队（去重指纹 `canonical_qhash`）。
- **审核权限**：群 owner/admin（server/src/store.cpp:6772 判定）可审本群条目；跨群升级条目由组织架构 admin 审（复用群能力门铺，docs/design/权限模型.md:7）。
- **审核动作**：
  - **通过** → `status='approved'`，记 `approved_by/approved_ms`，可选填 `expires_ms`（有效期，0=永久），可选填 `scope`（group/dept/global，§8）；通过即进入自动应答面（§3.2）。
  - **拒绝** → `status='rejected'`，**必填 `reject_reason`**（留台账，供后续同类候选过滤参考）；终态不复活，同问题再次收集开新条目并在审核界面提示「该问题曾被拒绝：原因 X」。
  - **编辑后通过** → 支持审核人改写问题/答案再保存（进 `knowledge_revisions` 快照，可回滚，先例 server/src/store.cpp:519-526）。
- **审核提醒**：有 pending 条目时，知识 bot 向群管理员周期性提醒（群能力 `knowledge` 开启时，默认 24h 一次，只回文本摘要「待审核 3 条」）；第一期不做专门推送通道（§1.5）。
- **留痕**：全部审核动作落 `knowledge_audits`（谁/何时/动作/前后状态/理由），对齐 approvals 台账口径（server/src/store.cpp:244-255）与权限模型「全程留痕」要求（docs/design/权限模型.md:26）。

## 4. 数据模型设计（新表，均无外键——与 bots/group_members 同口径，server/src/store.cpp:399-401）

### 4.1 `knowledge_entries`（知识条目主表）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `id` | INTEGER PK | 自增 |
| `group_id` | INTEGER NOT NULL | 来源群（条目归属域，检索半径默认锚点） |
| `question` | TEXT NOT NULL | 规范化问题（归一化后文本） |
| `question_raw` | TEXT NOT NULL | 原始提问快照 |
| `answer` | TEXT NOT NULL | 规范化答案（审核人可编辑） |
| `answer_raw` | TEXT NOT NULL | 原始回答快照（源消息文本） |
| `qhash` | TEXT NOT NULL | 归一化问题指纹（去重/相似检索键） |
| `source_msg_id` | TEXT NOT NULL | 提问源消息 msg_id（messages 表键，server/src/store.cpp:126） |
| `source_answer_msg_id` | TEXT NOT NULL | 回答源消息 msg_id |
| `answerer` | TEXT NOT NULL | 回答人账号 |
| `asker` | TEXT NOT NULL | 提问人账号 |
| `answered_ms` | INTEGER NOT NULL | 回答发生时间 |
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / approved / rejected / suspended（§5） |
| `confidence` | INTEGER NOT NULL DEFAULT 0 | 自动判定候选分 0-100（§3.3 评分） |
| `signals` | TEXT NOT NULL DEFAULT '' | 命中信号明细（JSON，供审核人参考） |
| `scope` | TEXT NOT NULL DEFAULT 'group' | group / dept / global（§8） |
| `dept_path` | TEXT NOT NULL DEFAULT '' | scope=dept 时的部门路径（复用 bot_dept_allow 口径，server/src/store.cpp:416-420） |
| `expires_ms` | INTEGER NOT NULL DEFAULT 0 | 有效期截止（0=永久；到期自动 suspended，§5） |
| `approved_by` / `approved_ms` | TEXT/INTEGER DEFAULT ''/0 | 审核人/时间 |
| `reject_reason` | TEXT NOT NULL DEFAULT '' | 拒绝原因（必填于拒绝动作） |
| `canonical_id` | INTEGER NOT NULL DEFAULT 0 | 冲突组/权威条目指针（0=无；§7） |
| `merged_from` | TEXT NOT NULL DEFAULT '' | 一问多答合并的来源条目 id 列表 |
| `hit_count` / `last_hit_ms` | INTEGER DEFAULT 0/0 | 自动应答命中统计（运营面） |
| `correct_count` | INTEGER NOT NULL DEFAULT 0 | #纠错 计数（熔断依据，§6） |
| `created_ms` / `updated_ms` | INTEGER NOT NULL | 台账时间 |

索引：`(group_id, status)`、`qhash`、`(status, expires_ms)`。

### 4.2 `knowledge_revisions`（修订史，镜像 group_memo_revisions 形态 server/src/store.cpp:519-526）

`id`、`entry_id`、`question`、`answer`、`editor`、`ts_ms`——每次编辑全文快照，可回滚。

### 4.3 `knowledge_audits`（审核留痕，镜像 approvals/assist_audits 口径 server/src/store.cpp:199-205、244-255）

`id`、`entry_id`、`actor`、`action`（submit/approve/reject/edit/suspend/restore/expire/upgrade_scope）、`detail`（理由/变更摘要）、`ts_ms`。

### 4.4 `open_questions`（开放问题登记/监听窗口）

| 字段 | 说明 |
| --- | --- |
| `id`、`group_id` | 来源群 |
| `qhash`、`question_raw` | 指纹与原始提问 |
| `asker`、`ask_msg_id`、`asked_ms` | 提问溯源 |
| `via` | explicit（@召唤）/ silent（收集轨） |
| `window_until_ms` | 监听窗口截止（登记时 + N 小时） |
| `status` | open / collected（已产生 pending 条目）/ closed / answered（命中已答） |
| `answer_entry_id` | 命中/收集后指向的条目 |

索引：`(group_id, status)`、`qhash`。同一 `qhash` 群内只保留一条 open 行（重复提问续期窗口并累加 `ask_count`）。

## 5. 状态机（文字描述）

```
            ┌─────────────── approve ───────────────┐
            │                                       ▼
 [收集/送审] ──► pending ──────────► approved ──► (自动应答面 §3.2)
            │         │                 ▲  │
     reject │         │ 编辑后通过       │  │ expires_ms 到期 / #纠错熔断 / 冲突裁决
            ▼         ▼                 │  ▼
         rejected   (仍 pending)     ┌──┴───┐
            │                        │suspended（停用：不自动应答）
            ▼                        └──┬───┘
        终态（留台账，不复活）            │ 复审（管理员 re-review）
                                        ▼
                                     pending（重新走审核）
```

- **pending → approved**：审核通过（可带有效期与 scope）。
- **pending → rejected**：审核拒绝，必填原因；**终态**——同问题再收集开新条目，旧条目仅作审核参考，不回 pending（避免死循环重审）。
- **approved → suspended**：三种触发——① `expires_ms` 到期（定时扫描，自动）；② `correct_count` 达熔断线（默认 2 次 #纠错，§6）；③ 冲突裁决中被判定为非权威版本（§7）。suspended 条目**立即退出自动应答**。
- **suspended → pending**：管理员复审（政策过期场景：有效期到了重新审，更新答案/续期后可再 approved）。**「能否回到 pending」的答案：能，但只允许从 suspended 回 pending，且必须人工触发**；pending 之间不互相跳。
- **政策过期怎么办**：`expires_ms` 到期 → 自动 suspended → 提醒群管理员「条目已过期待复审」→ 复审时更新答案（新 revision）或直接续期 → approved。过期期间自动应答静默（宁可漏答不可错答），对 @ 召唤只回「该答案已过期，正在等管理员复审」。
- 全部迁移落 `knowledge_audits`。

## 6. 相似问题判定与误答控制

**用什么相似度（第一期，无向量前提下）**：

1. 归一化：去标点、全半角统一、ASCII 小写、去「请问/一下/谢谢」等衬词（复用 search_messages 的分词/转小写经验，server/src/store.cpp:2691-2721）。
2. 主相似度：归一化文本的**字符 bigram Jaccard 系数**（纯内存计算，无新引擎；qhash=归一化全文 sha256 用于精确去重）。
3. 可选二次判等（有网关且相似度落入边界带时）：LLM 判「两问是否同一问题」，请求带 `memex_archive_scope:true` 走本地模型（红线，server/src/group_assistant.cpp:173）。

**阈值（写死，可配在群能力上）**：

| 相似度 | 行为 |
| --- | --- |
| ≥ 0.80（高线） | approved 条目 → 直接自动应答（§9 T1） |
| 0.60 ~ 0.80（边界带） | 不直接答 → 引用待审核/候选提示（§9 T2），或 LLM 判等通过后升级为直接应答 |
| < 0.60 | 不命中，走未命中流程 |

**误答代价控制（四道闸）**：

1. 只有 approved 且未 suspended/expired 才可自动答（铁律 1、2）；
2. 自动应答永远带来源与「答非所问？回复 #纠错」出口（§9 T1）；
3. **#纠错 熔断**：同条目累计 2 次 #纠错 → 自动 suspended → 回 pending 复审；纠错人须为本群成员；
4. 命中统计（hit_count/correct_count）进审核面，纠错率高的条目被标记降权（边界带处理）。

## 7. 冲突处理：同一个问题在不同群有不同答案

- **默认隔离**：条目归属 `group_id`，检索半径=提问群（§2 原则 4）。两个群各自 approved 各自答案，互不可见、互不干扰——这是**特性不是缺陷**（客服口径、环境差异本来就群间不同）。
- **冲突发现**：同一 `qhash`（或相似度 ≥ 高线）的 approved 条目在 ≥2 群存在且答案文本不同 → 系统置 `canonical_id` 指向同一冲突组（取最早条目 id 为组键），并向两群管理员各发一条冲突提示（文本，含双方答案摘要）。
- **冲突不自动裁决**：系统不判谁对，只做三件事——① 各群自动应答继续用本群答案（群内自洽）；② 跨群引用时必须带冲突警示（§9 T4）；③ 升级 global/dept 作用域时审核面强制显示冲突列表，由组织架构 admin 择一为权威（其余条目 `canonical_id` 指向权威、本群保留但标注「与全局口径冲突，见权威条目」）。
- **多租户/多群隔离语义**：memex 现为单服务器多部门形态，租户边界=部门树。`scope=dept` 条目按 `dept_path` 前缀匹配部门子树（对齐 bot_dept_allow 与组织可见性口径，server/src/store.cpp:416-420；docs/design/权限模型.md:7）；`scope=global` 全服务器可见。检索优先序：本群条目 > 本部门 > global；同分时本群优先。

## 8. 隐私边界：哪些知识可跨群复用，哪些必须隔离

| 层级 | 默认 | 升级条件 |
| --- | --- | --- |
| **群内（scope=group）** | 所有条目的默认归宿 | 无需升级 |
| **部门（scope=dept）** | 不可见 | 群主发起申请 + 组织架构 admin 审核（审核面显示来源群与冲突检测）；`dept_path` 限定子树 |
| **全局（scope=global）** | 不可见 | 同上，且必须通过冲突裁决（§7） |

**硬边界（任何情况下不进知识库、不参与相似检索）**：

1. **群密码箱内容**：密码箱全程密文、显式解锁（docs/design/群组知识共享.md:22-23 密码箱语义），知识 bot 永不读取、永不收录；回答中若含疑似凭据（token/密码样式文本），审核面告警且建议脱敏。
2. **备忘录「禁止放密码」红线同理**（docs/design/群组知识共享.md:12）。
3. **跨部门单聊/私聊内容**：知识 bot 只加群、只收群消息扇出（伪账号在 group_members），单聊它本就收不到；不新增「私聊收集」能力。
4. **归档红线继承**：知识 bot 的一切网关调用带 `memex_archive_scope:true`——涉及归档资料的判定只路由本地模型（server/src/group_assistant.cpp:173；docs/design/模型网关.md:39-40）。
5. **成员退出**：条目是快照，不因回答人退群删除；但展示来源时对已不在组织架构的账号按组织可见性脱敏（org_visibility 口径，server/src/store.cpp:378-384）。

## 9. 展示格式：引用待审核内容必须显式标注（话术模板）

四套模板（T1-T4），全部以 bot 身份回群文本消息（无新消息类型，§1.5）。**待审核内容一律带 ⚠️ 前缀与「尚未审核」字样，且不给「这就是答案」的语气**。

**T1 已审核命中（可直接作答）**：

```
📖 知识库（已审核）：
Q：联调环境今天几点就绪？
A：每天 10:00 前就绪，异常会在本群公告。
来源：#客服支持群 · 2026-10-12 09:52 · 周航 的回答 · 审核人：李明
答非所问？回复「#纠错」
```

**T2 引用待审核条目（显式标注，不作事实陈述）**：

```
⚠️ 以下回答【尚未审核】，仅供参考，不作为正式结论：
Q：签到接口字段名以哪份文档为准？
A：以 v2 文档为准，样例有误。
（待审核 · 来自本群 2026-10-12 张三的回答 · 审核中）
管理员可回复「#采纳」将其转为正式知识。
```

**T3 边界带候选提示（低置信，仅提示不回答）**：

```
🔍 知识库有一条相关但未完全匹配的待审核条目（相似度不足，未作答）：
Q：<问题摘要>…（待审核）
可用「@知识 <完整问题>」重问，或「@检索 <关键词>」自查归档。
```

**T4 冲突警示（跨群引用时，§7）**：

```
⚠️ 注意：该问题在不同群存在不同答案（冲突未裁决）：
· 本群口径：<答案A>（已审核）
· #其他群 口径：<答案B>（已审核）
以下引用的是【本群】口径，请以本群为准或联系管理员裁决。
```

**T5 未命中/登记回执**：

```
📭 知识库暂无「<问题摘要>」的已审核答案。
已登记本问题并开始留意群内回答；若有人在 24 小时内回答，将整理后提交审核。
也可「@检索 <关键词>」先查群聊归档。
```

**T6 纠错回执（私聊回纠错人，不刷群）**：

```
已收到您对知识条目的纠错（Q：<摘要>），该条目已暂停自动应答，等待管理员复审。
```

**硬规则**：T2/T3/T4 中凡出现未审核内容，标注词「尚未审核/待审核」**不可省略、不可折叠**；bot 的回复永远可追溯到来源（群/人/时间/msg_id）。

## 10. 机器人定位：助理还是自动应答？

**定调：默认「资料台助理」（被动、只答已审核），全量自动应答是群能力开关下的增强模式。**

| 维度 | 助理模式（默认） | 自动应答模式（开关开启） |
| --- | --- | --- |
| 触发 | 仅 @召唤（应答轨） | 收集轨命中高相似 approved 条目即插话 |
| 用户预期 | 「我问它才答」——漏答无感知伤害，答错归因于工具 | 「它盯着群」——不答有期待落差，答错打断对话 |
| 错误容忍 | 高 | 低 → 必须绑三重护栏：已审核＋高相似＋可纠错（§6） |
| 打扰度 | 零打扰 | 有打扰 → 同问题指纹冷却期内不重复答（建议 6h） |

**开关设计**：群能力词汇表新增第 8 项 `knowledge`（词汇表先例 server/src/store.cpp:4145-4156），管理语义分三档：`collect`（只开收集轨）/ `auto_answer`（开自动应答）/ 关。默认关（对齐「未配置=允许」的现行渐进口径还是从严？——**从严：未配置=仅 @召唤可用**，收集轨与自动应答都要显式开，因为本能力产出会对外发声，比公告更敏感）。

**对用户预期的影响（明确写出）**：

- 助理模式下用户把 bot 当「查得到的备忘录」，容错高，适合第一期铺开建立信任；
- 自动应答一旦答错，损伤的是「知识库可信度」本身，且客服场景错误直达客户——所以自动应答必须等审核闭环跑顺（第一期验收后）再按群灰度；
- 两种模式下 bot 都**不装人**：回复固定带「知识库」字样与来源，不模拟真人语气，避免「到底是谁在说话」的混淆（bot 消息 from 本就是 `bot:` 伪账号，客户端已按 bot 身份渲染）。

## 11. 交互流程

### 11.1 用户视角（客服群）

1. 客户在群问「VPN 怎么配？」——若本群开了收集轨，bot 静默登记，群内无感；
2. 客户或同事 `@知识 VPN 怎么配` ——bot 命中已审核条目 → T1 带来源作答；未命中 → T5 登记回执；
3. 群内老员工文字回答——窗口期内被识别为候选（被@者回复/管理员/邻近/实质回复加分），**群内依然无感**（收集不打扰）；
4. 回答人可主动 `#采纳 上一条` 加速；提问者回「解决了」亦触发候选；
5. 群管理员收到「待审核 N 条」提醒，打开审核界面（第一期 CLI/管理端，第二期客户端窗）逐条通过/拒绝/编辑；
6. 下次有人问同样问题 → T1 自动带来源作答；答案过时有人 `#纠错` → 条目停用复审。

### 11.2 系统视角（数据流）

```
TEXT 群消息 ─► bots 离线队列（既有扇出，session.cpp:287-289）
   ─► 知识 bot 轮询 pending_offline（group_assistant.cpp:255 同款）
        ├─ 提问分类器 ──疑似──► open_questions 登记 + 监听窗口
        │        └─ @召唤 ──► qhash 精确 + bigram 相似检索 knowledge_entries(approved)
        │              ├─ ≥高线 ──► T1 回群（幂等冷却）+ hit_count++
        │              ├─ 边界带 ──► T2/T3
        │              └─ 未命中 ──► T5 + 登记
        └─ 监听窗口内的群消息 ──► 有效性评分（§3.3）
                 └─ 候选分 ≥ 50 ──► knowledge_entries(pending) + knowledge_audits(submit)
群管理员 ──► approve/reject/edit ──► approved（进入应答面）/ rejected（台账）
定时扫描 ──► expires_ms 到期 ──► suspended + 管理员提醒
#纠错 计数 ──► 熔断 ──► suspended → pending 复审
```

## 12. 最小可行版本分期

**第一期（MVP：显式闭环跑通，零模型依赖）**

1. 四张新表建表＋CLI 管理命令（`knowledge list/show/approve/reject/edit`，对齐 cmd_bot 风格，server/src/main.cpp:2262）；
2. 知识 bot（复用 bot 面＋GroupAssistantWorker 轮询骨架）：`@知识 <问题>` 应答轨——qhash 精确命中＋bigram 相似 ≥ 0.80 → T1 带来源作答；未命中 → T5 登记 open_question；
3. 显式收集：`#采纳` / `@知识 采纳 <msg_id>` → 直接成 pending 条目（第一期**不做**静默评分收集，信号可信度最高、实现最薄）；
4. 审核：CLI/管理端审 pending（approve/reject/edit），approve 后即可被 T1 命中；
5. 展示模板 T1/T2/T5/T6；#纠错 → suspended；
6. 群能力开关 `knowledge` 接入（默认关）。

验收口径：一问一答一审一复用全链跑通；pending 内容永不出现在 T1 语气中（测试断言）。

**第二期（静默收集与相似度增强）**

1. 收集轨：提问分类器（规则版）＋回答有效性评分（§3.3 全信号）；
2. LLM 判等进边界带（网关＋归档红线）；冲突检测与冲突组提示（T4）；
3. 客户端审核窗（形态对齐 ApprovalDialog，client/app/main_window.hpp:531）；
4. 管理员周期提醒；一问多答合并（merged_from）。

**第三期（跨域与治理）**

1. scope 升级流程（dept/global）＋冲突裁决审核面；
2. 有效期治理：到期扫描、复审提醒、政策类条目批量续期；
3. 纠错率运营报表（hit_count/correct_count 面）；移动端审核提醒（依赖移动端群聊 UI 成熟度，§1.4 未核实项）。

## 13. 测试口径（对齐既有设计稿风格）

- 真真真＋假上游骨架（同 test_group_assistant 口径，docs/design/群内智能助手.md:67）：真 CollabServer＋WebhookServer＋真知识 bot worker＋假模型上游；
- 必测腿：非指令不触发＆不收集（收集轨关时）/ `@知识` 精确命中回 T1（断言来源四元组与审核人出现）/ 未命中回 T5 且 open_questions 落行 / `#采纳` 成 pending / approve 后同问题自动命中 / reject 后不命中且台账留痕 / pending 条目永不被 T1 语气引用（对 T2 断言「尚未审核」字符串存在）/ #纠错 两次熔断 suspended / 有效期到期 suspended / 群能力关闭时全功能拒绝 / 重复投递幂等（同 msg_id 不重复登记与回复）/ 归档红线标志随网关请求出现。

## 14. 未核实清单（如实注明）

1. **移动端群聊 UI 成熟度未核实**：apps/android/README.md 载明会话列表与聊天页，但未载明群聊能力；iOS/鸿蒙 README 亦未提群聊——第三期移动端审核面依赖此项。
2. **本地开发库 memex-server.db 的实际群规模与消息量级未核实**：分期阈值（窗口 24h、冷却 6h、熔断 2 次）按经验值给定，需以真实客服群数据校准。
3. **未来是否引入向量检索未核实**：本设计按「无向量」现状设计（§1.3）；若后续引入 embedding 服务，§6 相似度层可替换，接口面（qhash+score）不变。
4. **客户端 bot 消息渲染差异未核实**：bot 伪账号消息在客户端的实际显示形态（是否已有 bot 标识 UI）未逐行核实，只核实了协议层 from=bot:<name> 直传（server/src/webhook.cpp:333、374）。
5. **群能力「未配置=允许」向「未配置=拒绝」的迁移计划未核实**（docs/design/权限模型.md:7 注明全面 deny-by-default 留后续）——本设计 `knowledge` 项按从严默认，若全局口径变化需对齐。
