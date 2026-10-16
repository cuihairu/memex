> **English** | [中文](建群主题与群记录导出.md)

# Group Creation Topic/Purpose and Group Record Export (Design Draft, Not Implemented)

> Status: design draft (2026-10-12 user directive); this round is design only, no code changes — implementation will be scheduled after the user has reviewed it. The todo landing spot = a new "two group items" section (not merged into R24; existing numbering is not redistributed).
> Use only established vocabulary (topic/purpose/export/range/pagination/summary); do not coin new terms; mark uncertain points as 「待定」; all four items were finalized on 2026-10-12 (see the finalized-decision table in §4).

## I. Asking for topic/purpose at group creation

### 1.1 Dialog and fields

When creating a group, one screen pops up (the existing group-creation dialog is expanded to two steps on the same screen):

| Field | Form | Required | Landing spot |
|---|---|---|---|
| **Topic** | Single line, one sentence | **May be left empty** (decision ①: skipping = create the group directly, see 1.3) | Group profile `topic` |
| **Purpose** | Multi-line description | May be left empty | Group profile `purpose` |

- The value is persisted in the group profile and is visible **below the group avatar/group name** (same position on the group profile page and the group header in the chat page);
- **Empty-topic display stance (written into decision ①)**: an empty topic = **that line is not shown** below the group name (same for purpose — if empty, that block is not shown); the group profile page editing area still shows it and it can be filled in at any time;
- **Editable later**: the edit right follows the group-announcement precedent (group owner/admin, same permission source as `group_memo`), with the entry point on the group profile page;
- Same source across three surfaces: desktop/phone/admin all read the same `group_info` live-queried fields; no second copy of the group profile appears.

### 1.2 Persistence

Two columns are added to the `groups` table (ALTER + default values following the provider/ext_key column migration style; the table is not rebuilt):

```sql
ALTER TABLE groups ADD COLUMN topic   TEXT NOT NULL DEFAULT '';
ALTER TABLE groups ADD COLUMN purpose TEXT NOT NULL DEFAULT '';
```

Existing groups get empty strings in both columns (= that line is not shown, per the display stance of decision ①).

### 1.3 Protocol and compatibility

- The group-creation frame and the group-profile-editing frame add **optional** topic/purpose fields; an old client that leaves them out → the server stores the default empty string, with behavior unchanged (protocol compatibility stance);
- **Decision ① (2026-10-12)**: the group-creation topic **can be skipped** — the dialog still pops up by default, and both topic and purpose may be left empty to create the group directly (no forced validation at the dialog layer; if empty, the line is not shown per the display stance above);
- The server does not force-reject an empty topic/purpose (consistent with the skip decision; old-client group creation is unaffected).

### 1.4 Acceptance (testable)

- Group-creation dialog: **skipping the topic still creates the group** — a submission with both topic and purpose empty succeeds, and the two profile columns land as empty strings; if filled, they are persisted as given;
- Empty-topic display: the topic line is not shown below the group name (shown after filling); same stance for purpose;
- After persistence, `group_info` returns topic/purpose; displayed below the avatar/group name on the profile page;
- Group owner/admins can edit; an ordinary member's edit is rejected (403, same permission decision as the group announcement);
- An old-protocol frame (without the new fields) creates the group successfully and the two profile columns are empty strings.

> **Implementation status (2026-10-13, group item 1 closed out)**: Landed — `groups` gains `topic`/`purpose` (TEXT NOT NULL DEFAULT '', table-creation columns + ALTER migration following the ensure-column stance, table not rebuilt; existing groups get empty strings in both columns = the "not filled in" state); the protocol's GroupCmd adds topic=6/purpose=7 (create carries the profile along with the group; a new op="profile" edits later; both fields are overwritten as a set = leaving empty clears them; both are optional — decision ①'s skip = create the group directly, and the server does not force-reject; proto3 empty strings do not go on the wire = old-client frames keep the same wire shape, and unknown-field skip behavior is unchanged); GroupInfo adds topic=6/purpose=7 (GROUP_QUERY/GROUP_DATA return them from the same source as the admin /api/admin/groups ledger); the edit permission decision follows the group-announcement precedent (group owner/admin, adjudicated at the store layer by group_set_profile, not entered into the announcement edit history — the design did not require profile retention, so the announcement history stance is untouched; the profile is not a group capability, so the seven-item vocabulary is not expanded). Desktop: the group-creation dialog adds 「topic」 (single line) + 「purpose」 (multi-line), both may be left empty, and the group session meta line is assembled per the decision-① display stance (topic/purpose shown only when non-empty; purpose's internal newlines are folded to 「／」 and single-lined, with the full text visible in the edit window); the group right-click menu adds 「设置群资料…」 (next to the group announcement), and after the profile receipt is settled the group list is re-fetched to align. The admin group ledger adds topic/purpose columns (read-only display — the edit right belongs to the group owner/admin via the collaboration-surface protocol; admin does not set it on their behalf). Tests: test_group is extended with DB-level legs (create-with-group persistence + default empty strings / group owner sets + whole-set overwrite / leaving empty clears / admin sets on behalf / ordinary member rejected / ghost group rejected / groups_list·groups_of return from the same source / existing DB ALTER lands empty strings that can be backfilled) plus protocol-level legs (create with profile → GROUP_DATA returns it / non-owner profile rejected / owner sets + clears / old frame creates group with both columns empty); test_admin_api's ㉙th leg extends topic/purpose read-back assertions. Residual gaps are honestly noted: Android has no group UI (historically so — group messages are receivable with peer=group:N, and the management surface comes with later blocks, which will consume the same GroupInfo fields); the desktop's "below the group name" lands as the session-header meta line (desktop has no standalone group profile page; the dialog is the profile-editing entry).

## II. Group record export

### 2.1 Export content (three fixed sections)

| Section | Content | Data source (existing) |
|---|---|---|
| **Participants** | Member list + roles + join time | `group_members.joined_ms` (existing); **the departure time leaves no trace in the current DB** (leaving the group removes the row) — honestly noted as 「离开时间不含」; if it must be carried, a separate member-change ledger would have to be established and is not part of this phase |
| **Time range** | Group creation time + the selected range | `groups.created_ms` (existing) + the export parameters from/to |
| **Chat records** | Messages within the selected range, including sender and timestamp | `search_messages` (see 2.2), with range parameters since_ms/until_ms |

### 2.2 Reusing existing capability (no new export engine)

- **Query layer = `search_messages`** (store.hpp `MessageSearch`): `peer="group:N"` + `account=exporting user` + `since_ms/until_ms=selected range` — the **same query layer** as session history / CLI / admin console search (demand-batch ⑧ stance), **what you see is exactly what the history surface sees**, with no over-privileged new data surface; once the exporter has left the group, the peer can no longer be joined = empty result, a natural permission boundary.
- **Two precedents for the export surface**: ① admin archive search export (`messages --as`, auditor credentials + lands in `audit_reads`, platform-6 red-line stance); ② `/files/group-export` (R25 snapshot export: permission decided at the routing layer az + the export itself leaves a trace) — this feature takes ②'s **trace-leaving idea** + ①'s **audit table** (audit_reads); the permission decision is written separately per decision ② (all members = members of this group, not a group-owner/admin threshold, see 2.5).
- **Format (decision ④, 2026-10-12)**: default **Markdown (.md)** — produced in-repo (dialogue form: `[time] sender: content`, recall lines carry a marker); if docx were to be offered additionally, the conversion tool and dependencies must be noted (an external tool like pandoc); **with no dependency, only Markdown is produced**, and no converter is built into the repo.
- **Count cap and pagination (default values, adjustable)**: 1000 entries per page (current magnitude of `MessageSearch.limit`); the per-export cap is **10000 entries**, beyond which pagination is automatic — the response carries a `has_more` + `next_since` cursor, and the client stitches page by page; when the cap is hit and truncation occurs, a `truncated` marker is written at the end of the document (the truncation moment and the resumption entry are stated; no silent truncation).

### 2.3 Two tiers: plain document and AI summary tier

- **Without AI = plain document**: purely the three record sections, no summary;
- **With AI = a「summary」section added at the top of the document**: depends on the R11 intranet model and the model gateway (docs/design/模型网关.md, 群内智能助手.md — **currently design drafts only, phase three**);
- **Degradation stance (written in)**: when the model is not wired up / the gateway is unreachable, **it must automatically degrade to a plain document and note「AI 未启用」in the export result** — no faking a summary, no empty summary section as a placeholder;
- **Iron rule**: archived data is only sent to the local model (R11 stance), and the export-summary path likewise does not leave the intranet — same origin as the hard constraint "data does not leave by default".

### 2.4 Trace-leaving boundary

- The export is a **copy**, and **does not touch the archive** (R3 iron rule: provide no function that can erase archived content; the export path is read-only against the archive);
- **One audit entry is recorded for the export action (decision ③, 2026-10-12)**: who, when, which group, time range, entry count, **whether the AI summary was included** — landed in the existing `audit_reads` table (action=`group_record_export`, filters=gid/from/to/count/AI marker), aligned with the trace-leaving principle; **the audit can be queried in the admin console** (following the console's existing audit query surface, `audit:read`→auditor permission decision unchanged) — no new table.

### 2.5 Permissions and the platform-6 boundary

- **Decision ② (2026-10-12): all members** — any member of the group can export that group's records (not only group owner/admins, not only one's own segment); permissions still go through the **routing layer az**: the exporter must be a member of this group (`group_members` live query — you have it the moment you join, and lose it the moment you leave), **non-members can never export (403)**;
- The export surface **is open only to members of this group**; an org-admin without membership in this group cannot see it — this does not touch the platform-6 red line (message content is not opened to org-admins); cross-group content never appears in an export.

### 2.6 Large-group / long-range acceptance (testable)

- Pagination: a range at the 10,000-entry scale is stitched completely page by page, with no duplicates or gaps in page order (cursor resumption);
- Progress: the client shows progress as「page x/y」and it can be interrupted;
- Failure retry: a failed page is retried 3 times (idempotency guaranteed by the since/until cursor; retries do not double-enter the document);
- Truncation: hitting the per-export cap → a `truncated` marker at the tail + a resumption entry;
- **Permission (decision ② acceptance)**: a non-member exporting is rejected with **403** (routing layer az); a member of this group (including an ordinary member) gets 200; after leaving the group, the same request gets 403;
- **Content (decision ④ acceptance)**: the Markdown file has **all three sections** — participants (list + roles + join times), time range (group creation time + the selected range), chat records (including senders and timestamps); the file is .md;
- **Audit (decision ③ acceptance)**: each export writes exactly one row to `audit_reads` (two rows across two exports); the row contains who/when/which group/range/count/whether the AI summary was included; the admin console can query that row;
- Archive untouched: the archive's row count and content are bit-for-bit identical before and after the export (R3 assertion);
- Degradation: model gateway unreachable → plain document +「AI 未启用」note (no summary section).

> **Implementation status (2026-10-13, group item 2 closed out)**: Landed — the query layer follows the design and uses the same source as `search_messages` (`peer="group:N"` + `account=exporting user`, with membership enforced by a `group_members` subquery = naturally empty result once you leave); for pagination, MessageSearch gains `asc` (ascending, default false = the CLI·admin console·session-history existing reverse-order behavior unchanged) + `after_id` (keyset cursor `AND id > ?`, default 0 = no filtering), and ArchivedMessage gains `row_id` (=messages.id, an extra field defaulting to 0, existing consumers unaffected); a new `group_members_detail` (participants = account + role + joined_ms, empty for ghost groups) — group owner identity lives in `groups.owner`, not in the `group_members.role` column (that column defaults to member and the owner row rejects modification), and the export surface maps it via an owner live query; GroupInfo gains `created_ms` (`group_info`/`groups_list` return it from the same source = the group-creation-time true value). Route `GET /files/group-record-export` (gid/from/to/with_ai/page_size/cap/fetched/after_id): the permission decision is decision ②'s all-members = `is_group_member` routing-layer live query (not an az rule table — group membership is this surface's criterion; non-member 403, group nonexistent 404, gid not a positive integer 400); the first page (after_id=0) assembles three sections of Markdown (participants include the「离开时间不含」note / time range includes group creation time + the selected range / chat records `[time] sender: content`, with recall lines carrying the (撤回) marker), and subsequent pages contain only chat-record rows (the client stitches page by page); hitting the per-export cap immediately marks `truncated` (the remaining budget = this page's cap; no silent truncation); with_ai always degrades to a plain document +「AI 未启用」note (no「summary」section placeholder); the export's close-out writes exactly one row to `audit_reads` (action=`group_record_export`, filters=gid/from/to/with_ai, no body text, and first_page is a fallback so a single-page export does not double-write); the archive is read-only (messages is SELECT only). Desktop: FilesClient `group_record_export` + `group_record_export_page`/`group_record_export_failed` (failures surfaced as-is, not swallowed); the group right-click menu adds「导出群记录…」(two range options + AI checkbox + progress「page x/y」+ interruptible + auto-save .md on completion; the QNAM non-ordering stance follows files_ensure_login + SingleShot logged_in for re-pulling). Tests: test_files_api group-2 legs (401/400/404/403 non-member / ordinary member 200 / all three sections present / participant roles and join times / group creation-time true value / recall marker / out-of-range exclusion / has_more·truncated boolean semantics / with_ai degradation note / exactly two audit rows with the AI marker queryable / three pages with no duplicates or gaps + continuation pages with no three-section header / cap hit → truncated / 403 after leaving the group with no added audit row / archive bit-for-bit identical); test_group DB-level legs (group_members_detail role and join time · row removal on leaving / row_id monotonic / ascending pagination with no duplicates or gaps / time window / empty result for non-members); test_admin_api's ㉮th leg extends (the `group_record_export` row is queryable via /api/admin/audit-reads = no new table). Residual gaps are honestly noted: the AI summary tier is not wired up (R11 model gateway is phase three) = always degrades to a plain document (the design-written stance, no summary section); docx is not offered (no conversion dependency, only Markdown is produced);「failure retry 3 times」= the client's page-by-page resumption is naturally idempotent (the cursor guarantees it; retries do not double-enter the document), and no separate retry leg was written; Android has no group UI (historically so; it will consume the same data surface at that time).

## III. Persistence and interface summary

| Change | Stance |
|---|---|
| `groups` adds `topic`/`purpose` (TEXT DEFAULT '') | ALTER + default value; existing rows land empty strings = the「topic not filled in」state |
| Group-creation / group-profile frames add optional topic/purpose | An old client leaving them out → empty strings, behavior unchanged |
| Export route (a new files-surface route, `peer=group:N` query) | Permission decision = decision ② (members of this group, routing layer az, non-members 403); lands in `audit_reads`; pagination cursor; AI tier degradation note |
| `audit_reads` reused | action=`group_record_export`, no new table |

## IV. Finalized-decision table (2026-10-12 user sign-off receipt; the pending-decision list is closed)

| # | Item | Decision (user's original wording) | Date |
|---|---|---|---|
| ① | Group-creation topic skip | **Can be skipped** — the dialog still pops up by default, and both topic and purpose may be left empty to create the group directly; the empty-topic display stance = that line is not shown below the group name (written into §1.1) | 2026-10-12 |
| ② | Export permission | **All members** — any member of the group can export that group's records (not only group owner/admins, not only one's own segment); permissions still go through the routing layer az, and non-members can never export (403) | 2026-10-12 |
| ③ | Export action audit | **Record** — who, when, which group, time range, entry count, whether the AI summary was included, aligned with the trace-leaving principle; the audit can be queried in the admin console | 2026-10-12 |
| ④ | Default format | **Markdown (.md)** — if docx were to be offered additionally, note the conversion tool and dependencies; with no dependency, only Markdown is produced | 2026-10-12 |

Finalized-decision acceptance anchors (testable; see §1.4/§2.6 for the full text): skipping the topic still creates the group (a fully empty submission succeeds; the topic line is not shown below the group name); a non-member exporting gets 403 (an ordinary member of this group gets 200; 403 after leaving); the audit row is persisted and queryable (including the AI marker, visible in the admin console); the Markdown file has all three sections (participants + time range + chat records).
