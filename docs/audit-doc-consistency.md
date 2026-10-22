> **English** | [中文](zh/审计-文档一致性.md)

# Documentation–Source Consistency Audit Report

**Audit date**: 2026-10-08 (round 1: single doc on the three group tools; round 2: repo-wide doc sampling)  
**Audit scope**: README.md + all pages under docs/ + design docs + source implementation  
**Standard of reference**: `/home/cui/workspaces/standards/文档与源码一致性审计规范.md` (Documentation–Source Consistency Audit Standard)

---
## Audit Table

| ID | Doc page | Claim | Source evidence (file:line) | Category (ahead/missing/mismatch) | Severity | Fix |
|---|---|---|---|---|---|---|
| A1 | docs/design/群工具三件.md | Poll: `topic` non-empty and `options` 2–10 required at creation | files_server.cpp:4453-4482 — `if (!j.contains("topic") || !j["topic"].is_string() || !j.contains("options") || j["options"].is_array() || (j.contains("deadline_ms") && !j["deadline_ms"].is_number_integer())` plus `if (topic.empty() || options.size() < 2 || options.size() > 10)` | Code ahead of doc (stricter detail) | P2 | No fix needed; matches design intent |
| A2 | docs/design/群工具三件.md | Poll: re-vote = overwrite (upsert overwrites `choice`) | store.hpp:1291-1300 — `struct GroupPollVote { std::string account; int choice; std::int64_t ts_ms; }` and the poll_vote method implementing overwrite semantics | Code ahead of doc (mechanism already implemented) | P3 | No fix needed; matches design intent |
| A3 | docs/design/群工具三件.md | Poll: close authorization — creator or group owner/admin | files_server.cpp:4620-4623 — `if (!group_manage_allowed(gid, account, p->created_by))` returns "只有发起人或群主/管理员可截止" | Consistent | P2 | No fix needed |
| A4 | docs/design/群工具三件.md | Ghost 404 for unknown poll/chain/task ids | files_server.cpp:4562-4565 — `if (!p.has_value() || p->group_id != gid) { respond_json(404, ...)`; same for chain_by_id and task_by_id | Consistent | P2 | No fix needed |
| A5 | docs/design/群工具三件.md | Chain: `format_hint` optional, defaults to empty string | files_server.cpp:4653-4654 — `const std::string format_hint = j.contains("format_hint") ? j["format_hint"].get<std::string>() : ""` | Consistent | P3 | No fix needed |
| A6 | docs/design/群工具三件.md | Chain: one entry per person (upsert) — resubmitting updates your own entry | files_server.cpp:4744 — `if (!impl_.store.chain_join(chain_id, account, content, now_ms()))` and the store-level UPSERT keyed on account+chain_id | Consistent | P2 | No fix needed |
| A7 | docs/design/群工具三件.md | Group task: non-empty assignee must be a member; ghost → 404 | files_server.cpp:4834-4837 — `if (id == 0) { respond_json(404, {"error", "群或负责人不存在"}); }`; same ghost check in gtask_create | Consistent | P2 | No fix needed |
| A8 | docs/design/群工具三件.md | Group task completion = assignee/creator/owner-admin | files_server.cpp:4958-4963 — `if (account != found->assignee && account != found->created_by && !group_manage_allowed(gid, account, found->created_by))` | Consistent | P2 | No fix needed |
| A9 | docs/design/群工具三件.md | `done` is terminal: no reopen, no delete | files_server.cpp:4954-4956 — `if (found->status != "todo") { respond_json(409, {"error", "任务已完成"}); }`; only todo can move to done | Consistent | P2 | No fix needed |
| A10 | docs/design/群工具三件.md | Prose mentions HTTP 401 | Consistent — the unauthenticated 401 comes from `account_or_respond()` (files_server.cpp:743), and the test leg asserts `status == 401` and is green (test_files_api.cpp:2512); "non-member 403" is the other authorization leg. The original 401 is correct | Consistent (previously mis-corrected to 403, then reverted) | P1 | Fixed: §4 back to 401; the original mis-judgment ("change to 403") was reverted — source is the source of truth, unauthenticated ≠ non-member |
| A11 | docs/design/群工具三件.md | "Anonymous voting, multiple choice, auto-close at deadline (currently display-only)" marked as deferred | design §0-§4 — several features marked "留后续" (deferred); not implemented in current code | Doc ahead of code (planned item) | P1 | Keep the "planned/not implemented" marker, do not delete; fill in as increments land |
| A12 | docs/design/群工具三件.md | "Chain format validation, task reminder push, result card back to group (bot surface could host these)" marked as deferred | design §0-§4 — several features marked "留后续"; not implemented in current code | Doc ahead of code (planned item) | P1 | Keep the "planned/not implemented" marker, do not delete; fill in as increments land |
| A13 | docs/design/群工具三件.md | Poll list carries per-person tally / named ledger | files_server.cpp:4511-4531 — polls_list returns each item with counts and votes, one record per person | Consistent | P3 | No fix needed |
| A14 | docs/design/群工具三件.md | Routes: POST /files/group-polls (create), GET /files/group-polls?gid= (list), POST /files/group-polls/vote (vote), POST /files/group-polls/close (close) | files_server.cpp:1044-1054 — full route dispatch; all 12 group-tool routes implemented | Consistent | P2 | No fix needed |
| A15 | docs/design/群工具三件.md | Routes: POST /files/group-chains (create), GET /files/group-chains?gid= (list), POST /files/group-chains/join (join), POST /files/group-chains/close (close) | files_server.cpp:1056-1066 — full route dispatch; all group-tool routes implemented | Consistent | P2 | No fix needed |
| A16 | docs/design/群工具三件.md | Routes: POST /files/group-tasks (create), GET /files/group-tasks?gid= (list), POST /files/group-tasks/claim (claim), POST /files/group-tasks/done (complete) | files_server.cpp:1068-1078 — full route dispatch; all group-tool routes implemented | Consistent | P2 | No fix needed |
| A17 | docs/design/群工具三件.md | All authorization follows the file:read group inheritance | files_server.cpp:4475,4558,4607,4655 — `if (!tool_call_allowed(account, gid))` with the same shape: `authorize({account,"file:read",group_resource(gid),"owner="+account}).allowed` | Consistent | P2 | No fix needed |
| A18 | docs/design/群工具三件.md | Identity constraint: close poll/chain = the creator or group owner/admin | files_server.cpp:4620-4623,4785-4788 — `group_manage_allowed(gid, account, p->created_by)` decides creator‖owner‖admin | Consistent | P2 | No fix needed |
| A19 | docs/design/群工具三件.md | Storage: group_polls (id/topic/options_json/deadline_ms/closed/created_by/created_ms) + group_poll_votes (poll_id+account PK/choice/ts_ms) | store.cpp DDL — six tables inserted; table shape and fields fully aligned | Consistent | P2 | No fix needed |
| A20 | docs/design/群工具三件.md | Storage: group_chains/chain_entries/group tasks aligned to the same table shapes | store.cpp / store.hpp — `group_chains`+`idx_gch_gid`, `group_chain_entries`, `group_tasks`+`idx_gtk_gid` indexes and fields all in place | Consistent | P2 | No fix needed |

---
## P0 List (must fix)

| ID | Item | Severity | Fix content |
|---|---|---|---|
| (See the summary at the end — in round 1, A10 was mis-flagged as "doc says 401, wrong"; reversed after source forensics. The repo-wide round's P0 list is in the audit-conclusion section.) | — | — | — |

---
## Round-1 Conclusion (single doc: three group tools)

1. **Overall consistency**: the three group tools (polls / chains / group tasks) design doc is highly consistent with the source. Core functional paths, authorization (file:read group inheritance), identity constraints (creator/owner/admin), storage (six-table DDL), and HTTP response codes/error text all match the design.

2. **Distribution of the three difference classes**:
   - **Consistent**: A3-A9, A13-A20 (authorization/identity/storage/routes/audit trail checked row by row against code).
   - **Description mismatch**: 0 (round-1 entry A10 was reversed on review — the source returns 401 when unauthenticated, the test asserts 401 and is green, so the original 401 was correct; the mis-applied change to 403 was reverted).
   - **Doc ahead of code**: 0 true ghosts — A11/A12 are explicitly marked "留后续" planned items, the "register implementation plan" form from step 5 of the standard, not unmarked ghost features.

3. **Post-fix state**: this doc can be used as-is with the source as the source of truth; no reverse code change needed.

---

# Repo-wide Round (2026-10-08, round 2: README + docs sampling)

> Method: enumerate the doc surface (root README.md/BUGS.md/admin/README.md/todo.md top matter + 14 pages in docs/src/guide/ + 16 docs in docs/design/), sample 2–8 verifiable technical claims per doc (endpoints/ports/CLI/defaults/deps/file paths/test names), and grep the source for each. Negative conclusions were pre-checked for path validity (search for synonyms after excluding build*/third_party/node_modules). Subagent channels were unavailable; everything was checked directly in the main session.

| ID | Doc page | Claim (excerpt) | Source evidence (file:line) | Category | Severity | Fix |
|---|---|---|---|---|---|---|
| B1 | report.md §部署 | UDP 2425–2436 / TCP 2426–2437 fixed ports | client/engine/direct/discovery.hpp:19 (kDiscoveryPort=2425, "first of the UDP 2425–2436 range"), client/engine/direct/direct_transport.hpp:30 (kDirectTcpPortEnd=2437) | Consistent | P2 | — |
| B2 | report.md | third_party/README.md component list | third_party/README.md exists | Consistent | P2 | — |
| B3 | report.md | Review report ~1200 lines | docs/建设方案评审报告-V10.0.md = 1204 lines | Consistent | P2 | — |
| B4 | roadmap.md | Android shipped (T6.3) | apps/android/ exists | Consistent | P2 | — |
| B5 | roadmap.md | iOS code-only + CI / HarmonyOS blocked, archived / ArkTS | apps/ios/, apps/harmony/, .github/workflows/ios.yml, harmony.yml | Consistent | P2 | — |
| B6 | platform-adapter.md | ctest -R direct_chat\|collab_chat all green | client/engine/direct/tests/test_direct_chat.cpp, test_direct_file.cpp, client/engine/collab/tests/test_collab_chat.cpp, test_collab_resend.cpp | Consistent | P2 | — |
| B7 | platform-adapter.md | client/tests/test_screenshot.cpp offscreen verification | client/tests/test_screenshot.cpp exists | Consistent | P2 | — |
| B8 | platform-adapter.md | xvfb-run ./build/client/memex_client | build/client/memex_client artifact exists (CMake target memex_client) | Consistent | P2 | — |
| B9 | wan-drill.md | scripts/wan_drill.sh <test path>; message port defaults to 24360, changeable via serve --port | scripts/wan_drill.sh exists; server/src/main.cpp:40 (kDefaultPort=24360) | Consistent | P2 | — |
| B10 | notify.md | webhook defaults to 24361; --webhook-port 0 = disabled | server/src/main.cpp:43 (kDefaultWebhookPort=24361), :63 ("int is needed to express 0 = disabled") | Consistent | P2 | — |
| B11 | notify.md | token = whk_ prefix, POST /hook/<token> | server/src/main.cpp:2288 ("whk_"+random_salt_hex), server/src/webhook.cpp (/hook/ route family, 6 sites) | Consistent | P2 | — |
| B12 | notify.md | webhook create/list/revoke CLI | server/src/main.cpp:2247 (usage line), :2332 (revoke subcommand) | Consistent | P2 | — |
| B13 | compliance.md | vcpkg.json as the single source of truth + third_party/组件清单.md | vcpkg.json exists, third_party/组件清单.md exists | Consistent | P2 | — |
| B14 | compliance.md | pnpm.overrides.esbuild ^0.25.0 | docs/package.json:15-16 | Consistent | P2 | — |
| B15 | compliance.md | Log prefix [MEMEX], "[MEMEX] MemexServer listening" | server/src/server.cpp:104 | Consistent | P2 | — |
| B16 | backup-restore.md | scripts/backup_restore_drill.sh <memex_server path> | scripts/backup_restore_drill.sh exists | Consistent | P2 | — |
| B17 | backup-restore.md | client-local memex-local.db | client/engine/direct/direct_engine.cpp:40 | Consistent | P2 | — |
| B18 | README | vcpkg builtin-baseline=10541e31… | vcpkg.json:5 exact match | Consistent | P2 | — |
| B19 | README | /files/health unauthenticated probe | server/src/files_server.cpp:756-760 (unauthenticated-probe comment + fixed 200 response) | Consistent | P2 | — |
| B20 | README | storage compose subcommand | server/src/main.cpp:2473 | Consistent | P2 | — |
| B21 | README | compose file surface 24561 + MEMEX_FILES_PORT | docker-compose.yml:61 ("24561:24561"), :64 (healthcheck probes /files/health) | Consistent | P2 | — |
| B22 | README | webhook.cpp (T4.10 notification integration) | server/src/webhook.cpp exists | Consistent | P2 | — |
| B23 | product.md | Media capability via libdatachannel (MPL-2.0) or WebRTC | No hits in vcpkg.json or third_party/组件清单.md (libdatachannel not a dependency); live remote assist uses a server-side HTTP relay (docs/design/远程协助.md self-notes "WebRTC P2P media base deferred") | **Doc ahead of code** | P1 | Fixed: product.md reworded to "not introduced, deferred — live media uses the server-side HTTP relay" |
| B24 | 一级菜单管理.md | Top-level menu visibility config surface (full rules) | Zero implementation in client/app/ (grep menu_visibility / 一级菜单 hits only the R27-1 task-list entry comment) | Doc ahead of code (doc header marks "design draft" but not "not implemented") | P2 | Fixed: status line now adds "未实现（设计先行）" (not implemented; design first) |
| B25 | decisions.md #6/#9/#12/#13 | Current engineering column reconciled with code | 2026-10-08 morning reconciliation notes (commit 1c1ac0a): retention policy / model_gateway.cpp red line / four remote-assist hard requirements / office-map floor cross-reference all match code; the "pending decision" column is not a difference | Consistent | P2 | — |
| B26 | development.md | Four presets (dev/ci/server-release/client-release) + vcpkg_installed-server | CMakePresets.json:17/27/39/84, :35 (VCPKG_INSTALLED_DIR=vcpkg_installed-server) | Consistent | P2 | — |
| B27 | design/ endpoint sampling | office-map three routes, /files/tasks, uplink three CLIs, /files/assist/* ten routes, /files/approvals, /files/memo, bot three routes, /v1/chat/completions, memex_archive_scope | files_server.cpp (office-map ×4 / tasks / assist ×10 / approvals / memo), main.cpp (uplink ×5), webhook.cpp (bot ×6), model_gateway.cpp (chat/completions ×3, archive_scope ×4) | Consistent | P2 | — |
| B28 | BUGS.md | BUG-001~004 fixed, BUG-005 on hold (recorded, not fixed) | Status column [x]×4 + [ ]×1 matches the ledger; the referenced files test_tray.cpp, scripts/bug-004-tray-walkthrough/run.sh, screenshots bug-004-tray-*.png all exist | Consistent | P2 | — |

## Repo-wide round: distribution of the three classes

- **Consistent**: 26 entries (B1-B22, B25-B28).
- **Doc ahead of code**: 2 (B23 libdatachannel media base — reworded to "not introduced, deferred"; B24 top-level menu management — status line now says "not implemented").
- **Code ahead of doc**: 0 sampled hits (each R increment carries a synced design draft + todo progress note; exhaustive coverage outside the sample is out of scope for this round).
- **Description mismatch**: 0 (round-1 A10's "401 misstatement" was reversed by source forensics — the original was right, the mis-edit reverted).

## P0 List (repo-wide round)

No P0 — no "doc claims implemented, code is a ghost" class difference; both ahead-of-code items (B23/B24) are design-first cases, re-marked per the standard, with no reverse code change.

---

# Round 3 (2026-10-13 · repo-wide forensics in four batches: README/About · design & guide pages · guide pages II · todo ledger)

Audit baseline: HEAD `1dbd1f5`. Four independent forensics batches (README/About; product/requirements/roadmap/decisions guide pages; authorization/feiq/file-storage guide pages; todo ledger counts), every claim checked row by row against code. Three categories = ahead (doc ahead of code) / behind (code ahead of doc, including missing) / mismatch (description mismatch).

| ID | Doc page | Claim | Source evidence (file:line) | Category | Severity | Fix |
|---|---|---|---|---|---|---|
| C1 | README.md:34 | "UDP 2425–2436" | client/engine/direct/discovery.hpp:27 only uses 2425; 2426–2436 are a reserved range (decisions.md #11) | Mismatch | P2 | Note that UDP actually uses only 2425; 2426–2436 reserved |
| C2 | README.md (ports section) | Model-gateway port not mentioned | server/src/main.cpp:49 kDefaultModelPort=24362 (off by default, :103 --model-port) | Behind | P2 | Add the model-gateway surface (off by default) |
| C3 | README.md:103 | Runtime layer "zero .so" | server/Dockerfile:9-10 is a self-description comment only; the produced image was not tested | Mismatch | P2 | Soften to static-linking wording |
| C4 | README.md:82 | Dependency list of six | vcpkg.json:73-82 admin feature (drogon), :100-112 crashpad missing | Behind | P2 | Add drogon and crashpad to the list |
| C5 | README.md:12 | Platform badges for five platforms | apps/harmony/ exists; Win/macOS are nightly-only (nightly.yml:65-94, ci.yml:22 ubuntu only) | Behind | P2 | Add a HarmonyOS badge or note nightly |
| C6 | README.md:65-66 | Android "shipped" | Real-device walkthrough + 39/39 unit tests (todo.md:170/194); but .github/workflows has no android leg | Behind | P2 | Note that there is no CI leg |
| C7 | README.md:24 / README.zh.md:24 | "8 desktop screens" | docs/design/prototypes/ actually has 9 desktop-*.html, but the prototypes.md desktop table has exactly 8 screens (one screen with two drafts: desktop-direct list state + desktop-direct-chat expanded state, :211 lists 9 links) | False positive excluded | — | No fix needed: the 8-screen × 9-draft reading holds; two agents reported it as P1 and matched; the draft-screen mapping decides |
| C8 | README.md:68 | HarmonyOS "T6.6 code-only" | todo.md has no T6.5/T6.6 entries (only :5 header note, roadmap.md:20, apps/harmony/README.md:3 references) | Mismatch | P2 | Add a T6.6 label to the [~] item (T6.5 is referenced nowhere; do not invent it) |
| C9 | README.md:70-71 / README.zh.md:67 | admin/ "Admin console (planned; placeholder)" | admin/ is implemented (src+tests+www+Dockerfile+CMakeLists, Drogon: admin/CMakeLists.txt:5; compose service: docker-compose.yml:81-106; docker.yml:29 pushes the image) | Behind | P1 | Change to "Drogon admin console, 8089, started with compose" |
| C10 | README.md:45-61 | client tree lists only engine/direct, engine/collab | client/engine/task/ exists (dingtalk/feishu/github providers, R25) | Behind | P2 | Add engine/task/ to the tree |
| C11 | README.md:58-61 | server/src file mapping enumeration | main.cpp, model_gateway.cpp, ldap.cpp, group_assistant.cpp, task_pull.cpp, agent_main.cpp (R26-1) are also present but unlisted | Behind | P2 | Add key files to the tree note |
| C12 | README.md:43-76 | monorepo structure tree | The root also has install.sh, install.ps1, scripts/, packaging/, examples/, BUGS.md not in the tree | Behind | P2 | Add the one-click installers and scripts/packaging |
| C13 | README.md:97 | Presets list only 4 | CMakePresets.json:57/69/80/96 also has client-release-win/mac/ohos, server-release-mac — 9 in total | Behind | P2 | Add the four platform presets |
| C14 | README.md:97 | "By default only server dependencies are installed" | vcpkg.json:1-6 installs the client/server shared deps by default; the server feature adds server-specific packages | Mismatch | P2 | Reword to "shared packages for both ends; the server feature installs server-only packages" |
| C15 | README.zh.md:111 | "Two services healthy = ready" | docker-compose.yml has three services (rustfs:21, server:39, admin:81); README.md:119 says "all three" at the same spot | Mismatch | P1 | Change to "three services" |
| C16 | README.zh.md:107 | One-command startup note lists only server + RustFS | README.md:114 at the same spot includes "+ admin web console" | Mismatch | P1 | Add "＋admin console" to the note |
| C17 | README.zh.md:118 | Image publish pushes only memex-server | docker.yml:29 builds both images in parallel; README.md:128 includes memex-admin | Mismatch | P1 | Add the memex-admin image |
| C18 | README.zh.md (whole file) | The entire "admin console" section is missing | README.md:240-256 has the full Drogon process/bootstrap/az-role/30-minute-session section; the zh version has none of it | Mismatch | P1 | Add the admin-console section to zh |
| C19 | README.zh.md:17 / README.md:17 | Client sentence mentions only Android / iOS | README.md:68's harmony line is missing from the form-factor sentence | Behind | P2 | Add "HarmonyOS NEXT (code-only)" |
| C20 | repo About description | "dual-engine client…self-hosted server" | The current shape includes mobile (Android shipped / iOS / HarmonyOS) and the admin console; the description mentions neither | Behind | P1 | Add mobile and the admin console to the description |
| C21 | repo About topics | Ten topics | No android/kotlin/swift/harmony/mobile | Behind | P2 | Add android, harmony, mobile |
| C22 | README.md:304 / README.zh.md:234 | "No tags, no releases" | nightly.yml:542 updates the nightly Release (rolling); docker.yml:60-68 produce sha-/latest tags | Mismatch | P2 | Change to "no git tags, no formal releases" |
| C23 | README.md:78 | Module boundaries correspond to "Chapter 4" of the report | 建设方案评审报告-V10.0.md:556-566 has module division and data model in Chapter 5 | Mismatch | P2 | Change to "Chapter 5" |
| C24 | product.md:42 | Collaborative state uploads only transport metadata into the archive | Cannot be verified: FILE_AUTHZ does authorization only and does not persist (server/src/session.cpp:805-833); no transport-metadata table anywhere (store.cpp:78-637) | Ahead | P1 | Reword to "authorization via the server; metadata not archived for now" or add the table (change the doc if not the code) |
| C25 | requirements.md:27 | R8 covers at least UOS and Kylin | platform-adapter.md:24-29 self-admits "field verification pending, no such machines" | Ahead | P1 | Note sandbox-only verification, real machines pending |
| C26 | requirements.md:35 | R16 remote assist (incl. WebRTC media base) | Media goes through the file-surface HTTP relay (client/app/assist_dialog.hpp:5); WebRTC not implemented | Mismatch | P1 | Note the live HTTP relay; WebRTC deferred |
| C27 | requirements.md:64 | A21 verifies WebRTC can be established on target terminals | vcpkg.json has no libdatachannel; platform-adapter.md:14 self-admits not started | Ahead | P1 | Re-scope the acceptance item: screenshot works per platform + degradation hint |
| C28 | requirements.md:65 | A22 component list has only 8 entries | Missing crashpad (third_party/组件清单.md:24 has it) and drogon (vcpkg.json:43-49 has it; the list does not) | Behind | P1 | Register both components and refresh the verification date |
| C29 | requirements.md:55 | A12 completes real tests on UOS/Kylin terminals | platform-adapter.md:9-13 only Ubuntu 24.04 sandbox | Ahead | P1 | Same root as C25: note real machines pending |
| C30 | roadmap.md:22 | Phase 2 and later (to be scoped in a separate project) | todo.md:209-226 has all eight phase-2 items [x] with code present (approvals store.cpp:244, tasks :216, assist_sessions :184, audit_reads :326, bots :402) | Mismatch | P1 | Note that phase 2 has landed |
| C31 | decisions.md:23 | Appendix FeiQ compatibility: incompatible, no IPMsg compatibility (decided) | Implemented: client/engine/direct/ipmsg.hpp:14, net_remote_settings.cpp:55-61 (net/ipmsg_enabled off by default), net_guard.cpp:88,92; feiq-compat.md:3 records the 2026-10-11 user-issued reversal | Mismatch | P0 | Add the reversal note "2026-10-11: sidecar compatibility, off by default" to the decision-ledger appendix |
| C32 | decisions.md:45 | Decision record: no IPMsg (plaintext protocol) compatibility | Same as C31 (ipmsg codec + same port 2425: discovery.cpp:304) | Mismatch | P0 | Record the reversal; align the decision ledger with the implementation |
| C33 | report.md:17 | Network admission UDP 2425–2436 / TCP 2426–2437 | UDP uses only 2425 (discovery.hpp:27); TCP 2426–2437 (direct_transport.hpp:30-31) | Mismatch | P2 | Note UDP uses only 2425; 2436 reserved |
| C34 | compliance.md:12 | A22 list has only 8 components | Same as C28 | Behind | P1 | Register both components; extend the verification date |
| C35 | development.md:23-32 | Dependency list of 7 | Missing drogon (vcpkg.json:43-49) and crashpad | Behind | P1 | Add two rows to the dependency table |
| C36 | development.md:80-86 | Preset table of 4 | CMakePresets.json:56-104 actually has 9 | Behind | P2 | Complete it or mark it "excerpt" |
| C37 | development.md:11 | Doc site Node 22 + pnpm | CI uses node 24 (docs-check.yml:30, docs-deploy.yml:32) | Mismatch | P2 | Change to Node 24 |
| C38 | development.md:5 | C++17 compiler (GCC 15 or MSVC) | ci.yml:22 runs-on ubuntu-latest with no pinned compiler | Mismatch | P2 | Note that the CI toolchain is unpinned |
| C39 | authorization.md:109 | test_admin_api.cpp has 19 admin-surface legs | admin/tests/test_admin_api.cpp:210-798 actually has 27 numbered legs | Mismatch | P2 | Update the leg count to 27 |
| C40 | feiq-compat.md:28-29 | Ciphertext (SECRETOPT decoded on the UTF-8 path) | ipmsg.hpp:28 only defines kSecretOpt with no consumer; decoding actually keys on kUtf8Opt (discovery.cpp:230) | Mismatch | P2 | Change to "this bit is ignored, not implemented; decoding follows the UTF8OPT bit" |
| C41 | file-storage.md:5 | Decision background in Decision List #7 | decisions.md #7 = centralized deployment / storage volumes; no file-storage-specific entry | Mismatch | P2 | Note it is the "storage-volume implementation" sub-item |
| C42 | backup-restore.md:46 | compose deployment host-mounts /var/lib/memex/rustfs | The repo docker-compose.yml:30-31 uses the named volume rustfs-data; a host mount exists only in the generated storage compose (server/src/main.cpp:2941-2942) | Mismatch | P2 | Note "the compose generated by the storage compose subcommand" |
| C43 | crash-reporting.md:91 | ctest full run 61/61 green | Now 83 registered (deduped add_test; 82 at audit time, +1 from Task B checkbox cases); 61/61 was the 2026-10-08 snapshot | Mismatch | P2 | Add the qualifier "baseline at the time: 61" |
| C44 | platform-adapter.md:9-11 | Environment = Xvfb (X11/xcb) for ctest | CI actually runs offscreen (client/CMakeLists.txt:207; ci.yml has no xvfb) | Mismatch | P2 | Change the environment column to "offscreen (CI)"; Xvfb is local walkthrough only |
| C45 | delivery-matrix.md:36,122 | release-variant unit tests intermittently red = BUG-005 (recorded, unfixed) | BUGS.md:40 marks BUG-005 [x] fixed and accepted (2026-10-10 dual-variant 84/84×2 all green) | Mismatch | P1 | Update the matrix note and gap list to "fixed" |
| C46 | delivery-matrix.md:54 | Android JVM unit tests 57 | apps/android/app/src/test/ now counts 95 @Test (apps/android/README.md:12,36 in sync) | Mismatch | P2 | Update to the real count |
| C47 | delivery-matrix.md:56 | iOS MemexKit core unit tests 37 | apps/ios/Tests/MemexKitTests/ now counts 53 func test (apps/ios/README.md:17 in sync) | Mismatch | P2 | Update to the real count |
| C48 | delivery-matrix.md:65 | New-device approval (T3.4) ✅ accounts | That leg lives in server/tests/test_org.cpp:436-474; test_accounts.cpp covers only T3.3 (:224) | Mismatch | P2 | Point the ctest reference at org |
| C49 | delivery-matrix.md:33 | Post-install self-check smoke (--self-test) 🔧 Windows | The client has no --self-test (client/app/main.cpp:18-31 only --version/--screenshot/--smoke); --self-test is server-only (server/src/main.cpp:3096) | Mismatch | P2 | Note "--self-test is server-only; the client = --version/--smoke" |
| C50 | decisions.md appendix vs feiq-compat.md whole page | "FeiQ incompatible" still listed as the current position | feiq-compat.md is the 2026-10-11 user-issued landing (same root as C31/C32) | Mismatch | P1 | Add the reversal note to the decisions appendix |
| C51 | todo.md:201-202 | swift test 37 cases | apps/ios@6a701db is actually 38 (the todo's own itemized sum is also 38; the headline is short by 1) | Mismatch | P2 | Change 37 → 38 |
| C52 | todo.md:305 | node http mock 14 cases | apps/harmony@1fcd788 files_client.test.ts is actually 15 (88−73=+15 is self-consistent) | Mismatch | P2 | Change 14 → 15 |
| C53 | todo.md:311 | ext_denylist default denylist 24 entries | server/src/files_server.hpp:78-82 is actually 23 entries (the todo's own enumeration also counts 23) | Mismatch | P2 | Change 24 → 23 |
| C54 | apps/android/README.md:13 | Stale BUG-005 description | BUGS.md:40 already marks it [x] fixed (2026-10-10 dual-variant all green) | Mismatch | P2 | Update to the "fixed" reading |
| C55 | todo.md ledger-count batch | 24 counting-class claims (ctest timeline, leg counts, route counts, JVM/Node counts, protocol numbers, constants, ports, topic/token counts, 24 commit references, dates) | TX-01..24 all match the real counts (incl. store.hpp:462 block = 64, files_server.cpp:33 TTL 12h, admin_app.cpp:203/208 port/session, memex.proto:38-52 protocol numbers, theme.cpp 22 sets/27 tokens, and all 24 commit hashes present on main with matching dates) | Consistent | — | No fix needed |
| C56 | Repo-wide clean surface | README ports and deployment chain (24360/24361/24561/8089/8088→24360, 30×10s, auto-rollback), fifteen-plus-one decisions, six modules, vcpkg baseline same-source, pnpm docs:build, iOS/Harmony CI, the four-phase split; all remaining claims across product/requirements/decisions/roadmap/report/compliance/development/settings/prototypes/config.mts (ten pages) | Row-by-row file:line in the batch notes (main.cpp:334/:50, store.cpp:78-637, session.cpp:930-934, theme.cpp:47, zero missing links across the 18 pages in docs/src/guide/, etc.) | Consistent | — | No fix needed |

## Round-3 distribution of the three classes

- **Consistent / false positives excluded**: C55, C56 (two summary rows covering dozens of item-level matches); C7 false positive excluded (8 screens × 9 drafts, two agent reports matched, the draft-screen mapping decides).
- **Ahead (doc ahead of code)**: 4 (C24 file metadata archiving, C25/C29 domestic-OS real tests, C27 WebRTC acceptance).
- **Behind (code ahead of doc, incl. missing)**: 15 (C2, C4, C5, C6, C9, C10, C11, C12, C13, C19, C20, C21, C28, C34, C35).
- **Mismatch (description mismatch / stale)**: 34 (C1, C3, C8, C14-C18, C22, C23, C26, C30-C33, C36-C54).

## Round-3 P0 List

- **C31/C32 (same root)**: decisions.md still writes "FeiQ/IPMsg compatibility: incompatible, not doing (decided)", but the 2026-10-11 user order reversed this and landed sidecar compatibility (off by default): ipmsg.hpp/cpp codec, same-port UDP 2425 demux, net/ipmsg_enabled switch, the full feiq-compat.md design. A decision ledger contradicting current reality is the highest-risk documentation debt. Fix = add the reversal note to the ledger appendix (no code change).

## Round 4 (2026-10-14 · fix receipts)

- **C24–C49, C54 (guide pages + apps README, 26 entries)**: fixed; row-by-row file:line and writing notes are in the fix commits. Key real counts: admin_api 27 legs (C39), Android JVM 95 @Test (C46), iOS core 53 func test (C47), ctest registered 83 (C49, sync table 0), Node 24 (C37), drogon/crashpad registered (C28/C34/C35; drogon's entry in third_party/组件清单.md still pending).
- **C51**: todo.md swift test 37 → 38 done. **C52**: the todo.md evidence line was rewritten to "Node unit tests 89 cases green (incl. files_client 15)" — a 2026-10-14 real run of npm test was 89/89, one more than at audit time (88). **C53**: the enumeration sentence was deleted wholesale during tidy-up; the stale count no longer exists, no number change needed.
- **C31/C32/C50 (P0)**: both decisions.md decision #15's appendix and the decision record now carry "2026-10-11 reversal: sidecar compatibility, off by default (net/ipmsg_enabled, see feiq-compat.md)"; the ledger now matches reality; no other ledger decisions touched.
- **C8**: the [~] item now carries the T6.6 label; T6.5 is referenced nowhere in the repo and the number gap is kept as-is rather than invented.
- **Incidental fix (found outside the audit)**: the README.md "local testing conventions" section had been embedded inside a Build code block (introduced by 4251810) — moved out into its own section, with README.zh.md getting the same section; the zh deployment chain gained a sentence about compose pulling memex-admin on upgrade (aligning with EN:290).
- **C1–C23 (README/About)**: fix receipts are in their respective commits; C20/C21 (GitHub About description/topics) are judged by the `gh repo edit` result (see receipts).
- **todo.md ledger tidy**: 564 → 526 lines (312KB → 69KB); all 121 checkboxes kept (120 [x] + 1 [~]), 37 headings intact, the iron rules and R-entry table verbatim; each completed item is one line of requirement + one line of evidence.
