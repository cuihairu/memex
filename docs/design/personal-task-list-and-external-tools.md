> **English** | [中文](../zh/design/个人任务清单与外部工具接入.md)

# Personal Task List and External Tool Integration: Survey Analysis (R27 Design Draft)

> Status: survey analysis draft (2026-10-04 user directive: "survey and analyze first — which ones can be integrated, which ones can be implemented"). Scope: personal todolist (self-created + others-assigned) + external project/task management tool provider integration + jump-to-detail. Cross-references: todo R27, the permission model (assignment = group/organization permission semantics), the group tools framework (the R25 provider shape can be referenced).
>
> **Implementation status (2026-10-08)**: R27-1 local todolist landed — the server-side tasks table + five routes (/files/tasks create/list/done/reminded/delete), with "who can assign" = the az rule task-assign (co-members of any common group or same department; unrelated = default-deny, adjudicated live); the client's TaskDialog + the main window's「task」first-level menu; reminders = 30s polling while the window is open for due todos → notification center strong reminder + the receipt reminds only once (background reminders after the window closes are left to the notification center's resident-ization, for later). R27-2 landed — the Provider SPI (client/engine/task/task_provider: capabilities bit-declaration L1/L2/L3, detail_url(key,project) pure virtual, L2/L3 base class unsupported by default) + the L1 framework pieces (UrlTemplateProvider {project}/{key} template, UrlPassthroughProvider passthrough links that only allow http(s), TaskProviderRegistry with built-in presets GitHub Issue/PR · GitLab · Jira · Linear · passthrough + add() for custom ones); external task registration goes through the tasks table (provider/ext_key reference columns, required as a pair; personal registration is not forwarded to an assignee), and detailUrl is required = the client SPI resolves it locally before registration, and if resolution fails nothing is sent to the network or persisted; the server only stores the reference; TaskDialog's 🌐 row double-click jumps to the external detail, and checking an external entry = a local memex mark (write-back is L3). The credential surface (token/OAuth encryption surface) and the custom-template settings page land with R27-3. R27-3 landed (three engine entries on 2026-10-08 + the fourth entry on 2026-10-10 wiring up pulls and the settings surface) — the first batch of three providers (GitHub L1|L2|L3, DingTalk L2|L3, Feishu L3) use mock transport to lock behavior (this machine has no tenant credentials, so no real connection was verified — honestly noted); credentials are persisted to disk with passphrase-derived encryption via TaskProviderStore (PBKDF2 600k + AES-256-GCM, same level as the password vault), with the settings surface handling entry/passphrase-change/unlock, and TaskDialog's「pull external」pulls the L2 list directly and renders ⇣ read-only rows (you can mark them in the list only after registration; write-back is L3); the custom-template settings page landed (2026-10-10) — the「模板…」settings window adds/removes URL-template providers (the {key} slot is mandatory, and /id avoids the built-in presets, persisted in plaintext QSettings + registry.reload_custom to re-register); custom items immediately enter the registration dropdown and re-registration resolution; R27-4's second batch L1 jump is fully covered by the registry's built-ins, with L2/L3 started on demand (integration automatically gains the pull and credential surfaces).

## 0. UI placement (2026-10-04 user decision)

The task list entry = a **first-level menu in the left menu bar** (at the same level as conversations/contacts, permanently visible), not buried inside the personal settings page — it is a high-frequency independent surface: the desktop left column, the mobile first-level Tab.

## 1. Personal todolist design points

- **Each person has their own task list**: add your own, check done, set reminders;
- **Assigned by others**: other people (same group / same organization, per the permission model) can assign tasks to you — they appear in your list with their source marked and a jump to see the assignment context;
- **External tasks**: tasks from external tools (GitHub/Jira/Feishu…) are pulled in via the provider and displayed, **click to jump to the external detail** (detailUrl), and the completion state can be written back (per the provider's capability).

## 2. Provider tiers (integration depth)

| Tier | Capability | Implementation |
|---|---|---|
| **L1 jump** | Open the external detail with a detailUrl | A URL template suffices; the lowest cost |
| **L2 read-only** | Pull external task lists / display status | The provider implements list() |
| **L3 bidirectional** | Create/complete in memex and write back to the external side | create()/complete() + webhook or polling sync |

Provider SPI: `id / name / auth / capabilities(L1|L2|L3) / list / create / complete / detailUrl` — a capability-declaration system, where each tool implements only the tiers it supports.

## 3. External tool survey matrix (verified 2026-10-04)

| Tool | API surface | Reachable depth | Authentication | Conclusion |
|---|---|---|---|---|
| **GitHub Issues/Projects** | REST/GraphQL full | L3 | PAT/OAuth/App | ✅ First batch (the natural source of engineering tasks) |
| **Feishu Tasks** | Task v2 (create/update/query, assignee/watchers/custom fields — verified at open.feishu.cn) | L3 | tenant_access_token | ✅ First batch (domestic office suite) |
| **DingTalk To-do** | To-do task API (create/query/delete, **detailUrl jump directly** — verified) | L2-L3 | enterprise self-built app token | ✅ First batch (domestic office suite) |
| Jira | REST v3 | L3 | OAuth/API token | ✅ Second batch |
| Linear | GraphQL | L3 | OAuth/PAT | ✅ Second batch |
| Asana / Trello / ClickUp | REST | L3 | OAuth/token | ✅ On demand |
| Notion | REST | L2-L3 | OAuth | ✅ On demand (tasks = database rows) |
| Redmine | REST | L3 | API key | ✅ On demand |
| WeCom To-do | Server-side API | L2 | enterprise app | On demand |
| ZenTao | weak API (large version differences) | L1-L2 | token | ⚠️ Deferred (jump only for now) |
| Teambition | defunct | — | — | ❌ Not integrating |

## 4. Conclusions and scheduling suggestions

- **First batch**: local todolist (full L3 functionality) + GitHub + Feishu + DingTalk (each provider implemented to L2-L3; L1 jump is mandatory);
- **Second batch**: Jira/Linear; the rest added on demand per user request;
- **Not integrating**: Teambition (defunct); ZenTao first gets L1 jump only;
- All provider credentials go through the encrypted surface (R24 persona reference piece); **jump links are a basic requirement** (every provider must be able to jump to the external detail).

## 5. Delivery items (todo R27)

- R27-1 local todolist: self-created/completion/reminders + others-assigned (the permission model governs who can assign)
- R27-2 Provider SPI + L1 jump framework (URL template / detail jump)
- R27-3 first batch providers: GitHub, Feishu, DingTalk (each implemented to its declared capability depth)
- R27-4 second batch providers: Jira/Linear (started on demand)
- R27-5 server-side provider integration and push (2026-10-12 increment, see §6.1)
- R27-6 task type and time fields (2026-10-12 increment, see §6.2/§6.3/§6.5)
- R27-7 leadership view (2026-10-12 increment, see §6.4)

## 6. Increment design (2026-10-12 user directive: server-side provider integration and push / type column / start_ms + deadline / leadership view / kanban)

> Status: supplementary design sections (2026-10-12); this round is design only, no code changes — implementation will be scheduled after the user has reviewed it.
> Where this conflicts with the above (§0–§5), the existing landed stance prevails and the conflict is noted; use only established vocabulary (task/provider/deadline/start/status/看板/泳道/截止/上级), do not coin new terms.
> Decision status: every item in this section is finalized (the §6.6 finalized-decision table, 2026-10-12 user decision, no pending items left).

### 6.0 Current-state anchors (read from reality, not re-invented)

- `tasks` table (server/src/store.cpp:206): owner/creator/title/note/**due_ms** (deadline)/reminded_ms/**done**/done_ms/created_ms/provider/ext_key — **no type, no start time, no status**.
- `group_tasks` table: due_ms/**status** (default 'todo')/assignee/claimed_ms/done_by/done_ms (claim-based; done terminal state leaves a trace, row not deleted).
- `approvals` table: status four states (pending|approved|rejected|withdrawn).
- Permission decisions are at the routing layer az: task-assign (co-members of any common group or same department, default-deny); reporting relationships are in R14 (MemberProfile.manager holds the direct manager as a separate field, manager_chain walks up level by level — this section uses only one level, no recursion).
- External tasks: the client can already pull (L2 list(), TaskDialog's「pull external」) and double-click to jump (🌐 row, with detail_url resolved live by the client SPI from provider/ext_key).

### 6.1 Server-side provider integration and push (R27-5)

- The server periodically pulls GitHub/GitLab/Jira/Linear and other providers (per their declared capability depth) → lands in the `tasks` table (the provider/ext_key reference columns, required as a pair per the current stance) → pushes to online clients over the **existing long connection** (the re-delivery stance is the same as the current offline queue behavior); the client's「active pull」is demoted to a **fallback** (open-window refresh / offline catch-up view) and is no longer the main channel in the online state.
- **The pull is an optional switch, default off** — the server going out to pull external tools is a data-outbound surface, following the hard stance "data outbound is off by default"; only when on is there periodic pulling, and when off the behavior is the same as today.
- Credentials landing on the server must be encrypted: passphrase-derived KEK + AES-256-GCM (same level as the password vault, PBKDF2→KEK→AES-256-GCM, same stance as the current R24-3 password vault / R27-3 TaskProviderStore; the server side persists ciphertext, and plaintext only lives in session memory).
- Jumping **reuses the existing detail_url and double-click jump, with no new jump mechanism created**: the server-side rows carry only provider/ext_key, and at render time the client still resolves detail_url live via the SPI → the 🌐 row double-click jumps to the external detail (the same resolution path as the current registration flow; the server neither resolves nor stores URLs).

> **Implementation status (2026-10-12, R27-5 closed out)**: Landed — with `--task-pull-secret` unconfigured the whole surface is not enabled (zero outbound calls, zero timers); if any of the three gates (master key / admin switch `task_pull_enabled` absent row = off / a single source disabled) is unmet, there are zero outbound calls; outbound transport goes through drogon's HttpClient (certificate verification on by default, a 512KB response-body cap, trantor with its own I/O thread, and the response posted back via asio::post onto the single io thread); currently only GitHub issues is connected (filtering PRs / empty titles, ext_key = issue number), with GitLab/Jira/Linear to be integrated on demand later; the 60s main loop pulls each source per its interval_s level → `task_upsert_external` lands in the tasks table (same content not pushed; changes only update title/note and do not touch done) → new/changed rows are pushed to online clients via the existing deliver_notice (offline queueing + re-delivery same as the current stance, client polling demoted to fallback, zero protocol changes); credentials are sealed with PBKDF2 600k rounds + AES-256-GCM and persisted without plaintext; the user route /files/task-sources (POST/GET/DELETE, with partial semantics = missing fields keep the original row), the admin surface /api/admin/task-pull + the console's「task source pull」page (the switch defaults off, and both the explicit-enable confirmation box and the audit row state the pull scope); and for intranets with no external network, an injectable TaskHttp validates the link, with the real-transport leg hitting a local http stub and not the external network.

### 6.2 Task type: the type column (R27-6 part one)

- `tasks` gains `type TEXT NOT NULL DEFAULT '任务'`.
- **Decision (receipt #6)**: the four default types = **任务/需求/缺陷/其他** (the server-side configuration's initial values); other categories are added by admins in the admin console (the server-side configuration part is decided, see below).
- **Decision (2026-10-12 supplementary directive)**: **the type is configured on the server** — the type list lives on the server (with one default type), and desktop/phone pull it from the server rather than hardcoding it in the client; the admin console can add/delete/modify (aligned with the「admin management console」project start on 2026-10-11).
- Migration: existing tasks' type migrates to the default type「任务」(the column's DEFAULT lands it; no backfill script is needed).

### 6.3 Start time and deadline: start_ms + deadline (R27-6 part two)

- `tasks` gains `start_ms INTEGER NOT NULL DEFAULT 0` (default 0 = not set).
- `due_ms` already exists; the create/edit UI explicitly exposes it as **deadline** (no new field added; the semantics are exposed).
- **Decision (2026-10-12 supplementary directive ②)**: the start-time arrival reminder = **must remind, and must be switchable on/off** — the reminder is made a settings switch (default on); once off, it no longer reminds at the arrival time; the deadline's expiry reminder likewise gets a switch; **the two reminder switches go in the same place** (same position in the UI).
- The reminder channel follows the existing stance: 30s polling for due todos → notification center IMPORTANT strong reminder + the receipt reminds only once (reminded_ms is written once and never rolled back, R27-1's current behavior).

### 6.4 Leadership view (R27-7)

- The subordinate set = R14 reporting relationships' **direct reports** (reverse lookup by manager==account, **not recursive**).
- **Decision (receipt #1)**: **non-direct reports are not visible** — subordinate tasks and the kanban are open only to the direct manager; peers and other departments' managers are uniformly not visible, with a new az rule written in stating「only an R14 direct manager can read」(the precedent is reports' direct_reports + az report-read, but this rule has **no org-admin fallback**; written per the receipt's original wording); no separate permission system is created — it goes through the unified AuthorizationService.
- **Decision (receipt #2)**: the subordinate view is **on by default** — no per-person configuration needed; the direct manager sees it on entry; an off switch remains (an individual can turn it off), with on as the default.
- The workload-statistics stance is **written in**: incomplete count (status≠completed), overdue count (due_ms has passed and not completed), due-within-7-days count (0<due_ms−now≤7 days and not completed).
- **Decision (2026-10-12 supplementary directive)**: **only show one level (direct reports); cross-level is off** — once the hierarchy gets deeper the task volume cannot be displayed; the kanban follows the same stance (swimlanes only go one level deep).
- **Decision (2026-10-12 supplementary directive)**: the kanban form defaults to **A swimlane style** (one swimlane per direct report, for a glance at the pressure distribution) — only at one level does the volume fit; if B filter style (select a person first, then see their kanban) is wanted, please ask the user.

> **Implementation status (2026-10-13, R27-7 closed out)**: Landed — the server-side az's new rule `task-team-read` writes in「only an R14 direct manager can read」(resource=`user:<subordinate>` + context `owner=<subordinate>`, `manager_chain(owner).front()==subject` = the direct-report determination; **no org-admin fallback**, default-deny on no match, and nonexistent accounts likewise do not leak existence); `GET /files/tasks/team` = a `direct_reports(account)` reverse-lookup loop + **per-subordinate live az adjudication** (a subordinate who fails adjudication does not appear as a swimlane at all; not recursive, only one level), with the three workload figures' stance written in: incomplete = status≠completed (both to-do and in-progress count), overdue = due_ms>0 and passed and not completed, due within 7 days = 0<due_ms−now≤7 days and not completed (both paths agree during the done double-write transition period); no subordinates or subordinates without tasks both return 200 (a 0-count swimlane is still issued = the pressure distribution is visible at a glance); task row serialization is unified into `task_row_json` (mine/assigned/team from the same source, closing out the R27-6 missing start_reminded_ms column problem). Desktop: TaskDialog's「leadership view」switch is **on by default** (QSettings task_lead_view; an individual can turn it off, and off is persisted), with QScrollArea swimlanes (one QGroupBox per direct report, header「account (incomplete n · overdue n · due within 7 days n)」, cards carrying type + start + deadline, overdue red / imminent dark golden-yellow (threshold same as the kanban's task_due_warn_days) / completed gray), and with no subordinates or when off, the swimlanes auto-hide; toggling back on re-pulls immediately without waiting for the 30s poll. Residual gaps are honestly noted: Android TasksActivity's leadership swimlanes are not built (the R27-6 phone legs landed the task list and kanban first, with the leadership view to be completed in a next phone iteration consuming the same-source endpoints); the protocol legs (test_files_api: 401 / the stance across status flow / direct-report visible / cross-level off / org-admin no fallback / empty team with no subordinates) and the dialog legs (test_task_dialog: on by default / 0-count swimlane / three-figure header / hidden when off + persisted / re-pull on re-open — swimlane pulling goes through the QNAM connection pool, GET and the preceding POST are not ordered, and the tests align via throttled re-toggle re-pull, with the product surface converging to the same stance via the 30s poll) cover both sides.

### 6.5 Kanban (R27-6 part three)

- The kanban = **another view of the same batch of task data**, with no new data model and no new channel; it coexists with the existing list view (switchable).
- **Decision (receipt #3)**: personal tasks **need** an「in-progress」state — `tasks` gains `status TEXT NOT NULL DEFAULT '待办'` (enum: 待办/进行中/完成, default 待办); existing-row migration = done=1→「完成」, otherwise→「待办」; group_tasks and external tasks **keep their respective existing states** (group_tasks' todo/done, approvals' four states, external tasks carrying the provider's original state), and **the kanban columns uniformly read the status semantics**; the column done is retained writing both (during the transition reads defer to status; after migration closes out, done is a compatibility read only).
- Card times: the start time (start_ms) and the deadline (the deadline exposed from due_ms) both go on the card; **decision (receipt #4)**: **overdue marked red, due within 3 days marked yellow**, with a configurable threshold (default 3 days).
- Same source across three surfaces: desktop/phone/admin use the same batch of data; no second set of task states appears.
- **Decision (receipt #5)**: the phone surface = **horizontally-scrolling kanban + auto-degrading to a list on narrow screens** (the two coexist; not an either/or choice); if the user's intent is actually only a list, that will be corrected later — implement this way first.
- It does not conflict with the mailbox (docs/design/信箱.md) or OIDC (docs/design/OIDC身份认证接入设计.md) proposals: task due reminders still go through the notification center, and the mailbox only adds「inbox/unread/read」state views (its boundary ①), without moving task data.

> **Implementation status (2026-10-13, R27-6 closed out)**: Landed — §6.2 the type configured on the server: tasks gains `type TEXT NOT NULL DEFAULT '任务'` + the task_types configuration table (the four initial types 任务/需求/缺陷/其他, is_default=任务; ALTER follows the R27-2 ensure-column stance, table not rebuilt); the /files/task-types endpoint (readable after login) and the admin's three routes /api/admin/task-types GET·POST·delete (the default type and types referenced by tasks reject deletion = existing tasks never dangle; additions/deletions land audit rows) + the console's「task type」page; desktop FilesClient::fetch_task_types / Android FilesClient.taskTypes() pull from the same source (defaulting to the default type; admin changes refresh with the next login). §6.3: tasks gains `start_ms INTEGER NOT NULL DEFAULT 0`; the create/edit UI exposes the deadline (desktop's「截止（deadline）」label, phone's「截止」row; the edit leg is honestly noted = there has been no edit functionality since R27-1, this round records type/start/due along with creation, and editing remains for later); the start-arrival reminder and the deadline expiry reminder each get a switch (default on) in the same place (desktop QSettings task_remind_start/task_remind_due, Android SharedPreferences with the same key-name stance), with reminders going through the notification center/system notification's current stance — 30s polling (phone = checked after each refresh) + the receipt reminds only once: reminded_ms and the new start_reminded_ms are two independent receipt positions, routed by kind=start, never grabbing each other. §6.5: tasks gains `status TEXT NOT NULL DEFAULT '待办'` (enum 待办/进行中/完成; existing done=1→完成 otherwise→待办 = idempotent backfill); the done column retains writing both (task_set_done also updates status, task_set_status also updates done/done_ms, reads defer to status); the kanban = another view of the same batch of task data — desktop TaskDialog's「看板」button switches a three-column (待办/进行中/完成) QSplitter, with cards carrying type + start + deadline, overdue red / imminent dark golden-yellow (threshold QSettings task_due_warn_days, default 3 days), and double-clicking a card advances its status; Android TasksActivity scrolls three columns in a HorizontalScrollView + auto-hides the kanban entry and degrades to a list when sw<600dp; the admin console reads the type list from the same source, and no second set of task states appears. group_tasks/external tasks keep their respective existing states untouched (as the design stance says). Residual gaps are honestly noted: the harmony/ios task surface is not built (these two platforms have no task side at all, historically so — the R27 phone legs take Android first this round, with harmony/ios following their task-surface project rounds to complete the same-source endpoints and screens).

### 6.6 Finalized-decision table (2026-10-12 user decision; the pending-decision list is closed)

| # | Item | Decision (user's original wording) | Date |
|---|---|---|---|
| 1 | Non-direct-report visibility | **Not allowed** — subordinate tasks and the kanban are open only to the direct manager; peers and other departments' managers are uniformly not visible, with an az rule written in stating this (only an R14 direct manager can read) | 2026-10-12 |
| 2 | Subordinate-view default switch | **On by default** — no per-person configuration needed; the direct manager sees it on entry; an off switch remains (an individual can turn it off), with on as the default | 2026-10-12 |
| 3 | Personal task「in-progress」 | **Needed** — the tasks table gains a status column (待办/进行中/完成, default 待办); existing-row migration: done=1→完成, otherwise→待办; group_tasks and external tasks keep their respective existing states, and the kanban columns uniformly read the status semantics | 2026-10-12 |
| 4 | Color threshold | **Per default** — overdue red, due within 3 days yellow, with a configurable threshold (default 3 days) | 2026-10-12 |
| 5 | Phone-surface kanban | **Horizontally-scrolling kanban + auto-degrading to a list on narrow screens** (the two coexist; not an either/or choice); if the intent is actually only a list, that will be corrected later — implement this way first | 2026-10-12 |
| 6 | type default entries | **Four default types = 任务/需求/缺陷/其他** (the server-side configuration's initial values); existing tasks migrate to「任务」; other categories are added by admins in the admin console | 2026-10-12 |
| 补① | Type configuration location | The type is configured on the server: the type list lives on the server (with a default type), and desktop/phone pull it from the server rather than hardcoding it in the client; the admin console can add/delete/modify | 2026-10-12 |
| 补② | Start-time reminder | Must remind, and must be switchable on/off — a settings switch (default on); once off, it no longer reminds at the arrival time; the deadline expiry reminder likewise gets a switch, and the two switches go in the same place | 2026-10-12 |
| 补③ | Leadership-view hierarchy | Only show one level (direct reports); cross-level is off — once the hierarchy gets deeper the task volume cannot be displayed; the subordinate set takes R14 direct reports without recursion, and the kanban follows the same stance | 2026-10-12 |
| 补④ | Leadership kanban form | Default A swimlane style (one swimlane per direct report) — only at one level does the volume fit; if B filter style is wanted, please ask the user | 2026-10-12 |
| 补⑤ | Server-side pull switch | The pull is made an optional switch, default off (the hard stance that data outbound is off by default) | 2026-10-12 |

### 6.7 Table-structure changes and migration summary

| Change | Migration stance |
|---|---|
| `tasks` gains `type TEXT NOT NULL DEFAULT '任务'` | Existing rows land the default「任务」(receipt #6) |
| `tasks` gains `start_ms INTEGER NOT NULL DEFAULT 0` | Default 0 = not set, no backfill |
| `tasks` gains `status TEXT NOT NULL DEFAULT '待办'` | Existing rows backfill done=1→「完成」otherwise→「待办」(receipt #3); the done column retains writing both during the transition |
| Type-list table (server-side configuration; admin add/delete/modify) | The four initial types 任务/需求/缺陷/其他; default type「任务」(补①) |

(The old-DB ALTER migration follows the existing migration style of the R27-2 provider/ext_key columns: ensure-column + default value, table not rebuilt.)
