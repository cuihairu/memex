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

## Round 5 (2026-11-01 · README public face: badges / links / language routing / live counts)

Scope: both READMEs (badge row, every link incl. github.io and cuihairu.site, logo assets), repo About face (`gh repo view`), and every drift-prone live count in README / apps/android/README / guide pages. Standard: same three buckets + six steps (~/workspaces/standards/文档与源码一致性审计规范.md).

| ID | Doc page | Claim | Source evidence (file:line) | Category | Severity | Fix |
|---|---|---|---|---|---|---|
| D1 | README.md:4 / README.zh.md:4 | docs badge link `https://cuihairu.github.io/memex//` (zh: `…/memex/zh//`), badge img on the logo line | Both double-slash URLs still return 200 (GitHub Pages normalizes) — hygiene, not breakage | Mismatch | P2 | Single slash; docs badge moved into the single centered badge row, logo on its own line |
| D2 | README.md:25 | "product plan review report" → docs/建设方案评审报告-V10.0.md | Path does not exist in the repo (English digest at docs/plan-review-report-v10.0.md, zh original at docs/zh/建设方案评审报告-V10.0.md); the rendered GitHub link 404s (curl-verified) | Mismatch (dead link) | **P0** | Retargeted to docs/plan-review-report-v10.0.md (200) |
| D3 | README.zh.md:25 | Same dead path on the zh side | Same missing path; correct zh original verified live (docs/zh/建设方案评审报告-V10.0.md, 3×200 after one transient 000) | Mismatch (dead link) | **P0** | Retargeted to docs/zh/…; English digest linked alongside |
| D4 | README.zh.md:21-24 | zh README's four doc links point to the English site root (memex/, /guide/product, /guide/decisions, /guide/prototypes) | docs/src/.vitepress/config.mts — zh locale at link '/zh/' (lang zh-CN); /zh/, /zh/guide/{product,decisions,prototypes} all 200 with lang="zh-CN" | Mismatch (wrong-language link) | P1 | All four retargeted to /zh/ variants; source links to docs/src/zh/… |
| D5 | README.md:104 / README.zh.md:94 | Status badge embedded inside the bash code block | A stray `<p align="center">[![Status]…]</p>` line inside the ```bash fence (both files) — renders as literal text, duplicates the status message | Mismatch | **P0** | Removed from the code block; status badge merged into the single centered badge row |
| D6 | README.md:74 / README.zh.md:68 | "39/39 unit tests" (Android) | `grep -c '@Test' apps/android/app/src/test` → 101 across 10 files | Code ahead of doc | P1 | 39/39 → 101 (both READMEs) |
| D7 | apps/android/README.md:12-13,37 | "单测 95 项" / "95 用例" | Same 101 @Test evidence | Code ahead of doc | P1 | 95 → 101 (both spots); test-scope note gains 文件面 |
| D8 | docs/src/guide/delivery-matrix.md:65 / zh:54 | "95 JVM unit tests" / "JVM 单测 95 项" | Same 101 @Test evidence | Code ahead of doc | P1 | 95 → 101 |
| D9 | docs/src/guide/delivery-matrix.md:9,39 / zh:7,34 | "83 ctest items" / "ctest 83 项" | `ctest -N` on the current build tree → Total Tests: 88 | Code ahead of doc | P1 | 83 → 88 (four spots) |
| D10 | docs/src/guide/prototypes.md:3 / zh:3 | Opening: mockups "cover ten desktop screens and four mobile screens" | ls docs/design/prototypes → 24 `desktop-*.html` + `main-empty.html` + 6 `mobile-*.html` = 25 desktop + 6 mobile; the page's own sections below document the larger set | Code ahead of doc | P2 | Opening states the initial ten+four set and the current 25+6 |
| D11 | README.md:24 / README.zh.md:24 | UI previews "(live screenshots; 8 desktop screens + 4 mobile screens)" | guide/prototypes now carries 12 desktop sections + 6 mobile sections with live shots | Code ahead of doc | P2 | Hardcoded count dropped ("live screenshots, light/dark paired") |
| D12 | todo.md:220,222 | Evidence lines: Android "39→53→57", iOS "swift test 38 用例" | Android 101 @Test (D6); iOS `grep 'func test' apps/ios/Tests/MemexKitTests` → 53 | Code ahead of doc | P2 | Rewritten to current counts (至 101 / 续补至 53) |
| D13 | Sweep row (verified-consistent) | README badge targets, logo asset, repo About face, docs-site routing, ports/presets/baseline/compose | docs/src/public/logo.svg exists (1358 B); CI / Codecov / releases badge targets 200; `gh repo view` description matches current vocabulary (dual-engine, Drogon admin), homepageUrl = docs root, 13 topics; 9 github.io routes 200 with correct lang (root lang="en", /zh/ lang="zh-CN"; base '/memex/' + cleanUrls per config.mts); ports 24360/24361/24362-default-off/24561/2425/8089 vs main.cpp:43-71 & docker-compose.yml:64-67 (8088:24360); 9 presets + vcpkg builtin-baseline vs CMakePresets.json / vcpkg.json | Consistent | — | No fix needed |

### Round-5 distribution of the three classes

- **Ahead (doc ahead of code)**: 0.
- **Behind (code ahead of doc, stale counts/sets)**: 7 (D6–D12).
- **Mismatch**: 5 (D1–D5).

### Round-5 P0 list

- **D2/D3**: dead review-report links in both README headline sections (curl-verified 404).
- **D5**: status badge swallowed by a code block in both READMEs (invisible as a badge).

### Verification boundary (recorded, not a defect)

cuihairu.site tunnels: this seat's DNS sits behind a fake-ip proxy (even `dig @1.1.1.1` returns 198.18.x), so public reachability cannot be authoritatively verified from here. admin.memex answers GET / with 404 (plausibly real — the console API is POST-only) and server.memex is a raw TCP tunnel, not HTTP-probeable by design.

### Round-5 fix receipts (2026-11-01)

- D1–D12 all fixed this round (README.md, README.zh.md, apps/android/README.md, delivery-matrix en/zh, prototypes en/zh, todo.md); receipts in the round's `docs:` commits. Single centered badge row per README (docs / CI / Codecov / Status / platform / C++17-Qt6 / License).
- todo.md tidy (user order, one requirement per line, one evidence line per done item): 668 lines / 116,288 B → 668 lines / 91,260 B (−21.5%); 164 [x] + 2 [ ] + 46 headings unchanged vs HEAD; lines >200 chars 58 → 24; no ✅-date/hash infixes or pseudo-counts (HTTP codes) left in evidence parens.

## Round 6 (2026-11-02 · doc-vs-source drift: guide pages, design docs, todo/BUGS evidence paths, third_party registry)

Scope: guide pages (en+zh) drift-prone claims (tables/counts/paths/defaults), the concrete implementable
claims of the design docs (endpoints, ports, defaults, set sizes), todo.md and BUGS.md evidence paths,
the third_party registry versions. Standard: same three buckets + the six steps
(~/workspaces/standards/文档与源码一致性审计规范.md). Fix direction: source-wins.

| ID | Doc page | Claim | Source evidence (file:line) | Category | Severity | Fix |
|---|---|---|---|---|---|---|
| E1 | docs/src/guide/authorization.md:63 / zh:82 | "identities table" | The table is `identity_bindings` (server/src/store.cpp:783); no `identities` table exists | Mismatch | P2 | → `identity_bindings` (both pages) |
| E2 | docs/src/guide/backup-restore.md:11 / zh:18 | Backup key prefix `inbox/{uid}/…` | Object prefixes are only `groups/{gid}/` and `users/{uid}/` (files_server object-key computation); `inbox` is a category of `files.kind`, not a key prefix — the `inbox/{uid}/` prefix does not exist | Mismatch | P2 | Dropped the phantom prefix; stated category = `files.kind` |
| E3 | docs/src/guide/report.md:3 / zh:3 | Links `docs/建设方案评审报告-V10.0.md` | Path does not exist; the zh original is `docs/zh/建设方案评审报告-V10.0.md`, English digest `docs/plan-review-report-v10.0.md` — the rendered GitHub link 404s | Mismatch (dead link) | **P0** | Retargeted to the real zh path + English digest linked |
| E4 | docs/src/guide/file-storage.md:4 / zh:7 | Links `docs/design/文件存储与外网单向传输.md` | That Chinese-named path moved to `docs/zh/design/` (7fe568f); the en original is `docs/design/file-storage-and-one-way-outbound-transfer.md` | Mismatch (dead path) | **P0** | Retargeted to the moved zh path + en design doc |
| E5 | docs/src/guide/prototypes.md:136 / zh:326,335 | Agent heartbeat "15s / 掉线 60s" | Actual: heartbeat 30s (server/src/agent_main.cpp:139 `interval_s = 30`), offline 90s (server/src/files_server.cpp:35 `kAgentOnlineMs = 90 * 1000`); the client badge repeated the stale 15s/60s (client/app/group_server_dialog.cpp:74) | Mismatch (+ client UI bug) | P1 | Doc → 30s/90s (both pages); UI badge → 30s/90s (source-wins, code commit) |
| E6 | docs/src/guide/crash-reporting.md:109 / zh:92 | "83 tests" | `ctest -N` on the current tree → Total Tests: 88 (D9 already moved delivery-matrix to 88; this page lagged) | Code ahead of doc | P1 | 83 → 88 (both pages) |
| E7 | README.md:287 / README.zh.md:237 | "All console APIs are POST-only" | 47 `/files/*` GET routes exist; read-only GET = session-only, state-changing POST = session + X-Memex-Admin header (admin surface design); "all POST-only" understates the surface | Mismatch | P2 | Corrected: session required; state-change POST needs the admin header; read GET is session-only |
| E8 | todo.md:151 | Evidence path `client/tests/test_window_layout.cpp` | The file is at `client/app/tests/test_window_layout.cpp`; `client/tests/` does not contain it | Mismatch | P2 | → `client/app/tests/…` |
| E9 | todo.md (12 refs: 59,63,81-85,229,264,277,288,335,346,357,360,387,405,541,548,596) | Evidence paths `docs/design/<Chinese>.md` | 23 Chinese-named design files moved to `docs/zh/design/` (7fe568f); only `基础平台化改造计划（身份-组织-授权-归档）.md` remains in `docs/design/` | Mismatch (broad dead-path cluster) | P1 | 12 moved refs → `docs/zh/design/`; the unmigrated file left at the old path |
| E10 | BUGS.md:91,92 | `docs/design/消息标识与归档去重.md` | Moved to `docs/zh/design/` (same 7fe568f move) | Mismatch | P2 | → `docs/zh/design/…` |
| E11 | third_party/README.md:45 | Vite 6.4.3 | pnpm overrides pin 6.4.4 (docs/package.json + pnpm-lock) | Code ahead of doc | P3 | 6.4.3 → 6.4.4 |
| E12 | third_party/README.md:30 | swift-protobuf 1.38.1 | 2026-11-01 resolution 1.39.0 (apps/ios Package.resolved; Package.swift declares from 1.26.0) | Code ahead of doc | P3 | 1.38.1 → 1.39.0 |
| E13 | todo.md:486 | "90 个 `/files/*` HTTP 路由 + 19 个 CLI 子命令" | 104 distinct `/files/*` path literals (124 route-match legs) + 21 top-level subcommands (server/src/main.cpp:3111-3131); both counts grew past the 2026-10-11 inventory | Code ahead of doc | P2 | → 104 routes + 21 subcommands |
| E14 | todo.md:596 | "设计稿 `docs/design/` 23 篇" + enumeration | The 23-file zh set moved to `docs/zh/design/`; the enumeration missed `knowledge-deposition-loop.md` and wrongly listed `基础平台化改造计划…` (which never moved, still in `docs/design/`) | Code ahead of doc | P2 | Retargeted to `docs/zh/design/`; enumeration corrected (23 incl. knowledge-deposition-loop; unmigrated file noted in parentheses) |
| E15 | docs/design/remote-assistance.md:30 | "GET /audit?id=" | Route is `POST /files/assist/audit` (id in the JSON body) — there is no GET audit route on the assist surface | Mismatch | P2 | → POST /files/assist/audit |
| E16 | docs/design/brand-assets.md:116-117 | Login-window default file port 24362 | `default_files_port()` returns 24561 (client/engine/collab/collab_defaults.hpp:24-26); 24362 is the model-gateway port — a user following the doc configures the wrong port | Mismatch | **P0** | → 24561 |
| E17 | docs/design/file-storage-and-one-way-outbound-transfer.md:7 | Uplink default dangerous-extension blacklist "24 items" | The `UplinkPolicy` ext_denylist has 23 entries (files_server.hpp:78-81, exe…rpm) | Mismatch | P2 | 24 → 23 |

### Round-6 distribution of the three classes

- **Ahead (doc ahead of code)**: 0 — every "not implemented" design doc is explicitly marked planned; no ghost surface claimed.
- **Behind (code ahead of doc, stale counts/sets)**: 5 (E6, E11, E12, E13, E14).
- **Mismatch (description wrong / dead path)**: 12 (E1–E5, E7–E10, E15–E17).

### Round-6 P0 list

- **E3/E4**: dead paths in published guide pages (report.md, file-storage.md) — the rendered site links 404.
- **E16**: brand-assets design doc tells the reader the login window's default file port is 24362 (it is
  24561; 24362 is the model gateway) — a user following the doc misconfigures the connection.

### Verification boundary (recorded, not a defect)

- The prototype-conformance ledger's running ctest totals ("84/84 → 85/85 → 86/86", todo.md:613-647) were
  **verified accurate at their own commits** (`git grep add_test` at afa99fd = 84, 34944f9 = 85,
  803a188/7881b64/611545f = 86); current tree is 88. These are per-commit snapshots, not drift — left
  unchanged (the originally-suspected "stale ctest totals" item is excluded as a false positive).
- README public-face items (badges/links/routing) were re-checked in Round 5 and are unchanged this round.

### Round-6 fix receipts (2026-11-02) — four-segment general ledger

**① Findings & buckets**: 17 findings (3 P0, 3 P1, 9 P2, 2 P3); doc-ahead 0 / code-ahead 5 / mismatch 12.

**② P0 disposition**: all three P0 (E3, E4, E16) fixed this round; none required a code change (source-wins).

**③ Fix receipts** (file:line → change; commits in the round's `docs:` + `fix:` commits):
- E1 authorization.md / zh:82; E2 backup-restore.md / zh:18; E3 report.md / zh:3; E4 file-storage.md / zh:7;
  E5 prototypes.md / zh:326,335 **+ client/app/group_server_dialog.cpp:74** (code commit); E6 crash-reporting.md
  / zh:92; E7 README.md:287 / README.zh.md:237; E8 todo.md:151; E9 todo.md 12 refs; E10 BUGS.md:91,92;
  E11/E12 third_party/README.md:45,30; E13 todo.md:486; E14 todo.md:596; E15 remote-assistance.md:30;
  E16 brand-assets.md:116-117; E17 file-storage…md:7.

**④ Gates & commits**: docs build green — local pnpm is 12.10.1 which refuses the legacy `pnpm.overrides`
in docs/package.json (CI pins pnpm 10), so the gate ran the exact command line `pnpm docs:build` expands
to (`vitepress build src`, vitepress 1.6.4): **zero dead links, build complete in 8.17s** — and the gate
earned its keep: the first E3 attempt used a site-relative link to a repo-root file and the build caught
it (2 dead links, exit 1); fixed to the guide-page convention (absolute GitHub URL) before landing.
Release build green; ctest 88/88; one commit per batch (badge fix `fix:`, docs drift `docs:`, audit report
`docs:`); `git pull --rebase` before push; no tags, no force push. Commit list in the round's push.

## Re-audit 2026-11-02 (G3 batch)

Scope: public face (README.md / README.zh.md) re-evidenced against
docker-compose.yml, CMakePresets.json, server/src/main.cpp, git tags, and
docs/package.json; AI-flavor sweep over all md surfaces. Prior rounds
(2026-10-08/10-19) not re-litigated; new findings below — all fixed this round,
source untouched.

| ID | Doc page | Claim | Source evidence (file:line) | Category | Severity | Fix |
|----|----------|-------|------------------------------|----------|----------|-----|
| G1 | README.md:10 / README.zh.md:10 | Badge block | Every badge rendered twice (linked `<a>` + bare `<img>` dup), and a stray outer `<p align="center">` nested around it (invalid HTML) | Mismatch | P1 | Deduped 12→7 badges, single centered line, both READMEs |
| G2 | README.md Engineering Discipline / README.zh.md 同段 | "no git tags / 不打 git tag" | `git tag` = `nightly` (rolling); repo does carry one tag | Mismatch | P2 | Reworded to "no version tags / 不打版本 tag" |
| G3 | README.md Documentation / README.zh.md 文档 | "build with `pnpm docs:build`" (from repo root) | No root package.json; the script lives in docs/package.json (`docs:build: vitepress build src`) | Mismatch | P2 | Both READMEs now say run it from `docs/` |

Verified, no action: compose ports 8088→24360 / 24360 / 24361 / 24561 / 8089 match
docker-compose.yml; presets dev/ci/server-release/client-release(-win/-mac/-ohos)/
server-release-mac all present in CMakePresets.json; server CLI flags (`--db`,
`--port`, `--webhook-port`, `--files-port`, `--s3-endpoint`, `--model-port`) and
`account add … --role` / `org role grant` subcommands present in server/src/main.cpp;
install.sh / install.ps1 exist; honest in-house + third_party disclosure intact.
AI-flavor sweep: no actionable hits — 自研/100%/永不 occurrences are decision
records, coverage figures and design invariants (legal per the copy standard).
todo.md: already in the compact requirement+evidence format (676 lines, 169
evidenced done rows); no changes.

---
## Re-audit 2026-11-14 (G2 batch)

Scope: the surfaces that drift after recent code churn — test-count claims,
platform/install matrices, CLI flag inventory, README badges and About line,
nightly artifact channel. Prior rounds (2026-10-08 / 10-19 / 11-02) not
re-litigated. All claims below re-evidenced against source this round; every fix
is a doc-side change, source untouched.

Three buckets: **code ahead of doc 0 · doc missing 1 · mismatch 2** (P0 0 · P1 0 · P2 3).

| ID | Doc page | Claim | Source evidence (file:line) | Category | Severity | Fix |
|----|----------|-------|------------------------------|----------|----------|-----|
| G2-1 | docs/src/guide/delivery-matrix.md:9,39 and docs/src/zh/guide/delivery-matrix.md:7,34 | "ctest test names (88 items, ctest names deduplicated)" / "ctest 88 项" | `ctest -N` → 94 registered names, all distinct (0 duplicates); 94 `add_test(` declarations across server/ (35) + client/ (57) + admin/ (1) + common/ (1) | Mismatch (stale figure) | P2 | 88 → 94 in both languages, both lines each; wording tightened to "`ctest -N` names deduplicated" |
| G2-2 | docs/src/guide/crash-reporting.md:109-110 and docs/src/zh/guide/crash-reporting.md:92 | "the baseline has since grown to 88 registered tests (ctest names deduplicated)" | same as G2-1 | Mismatch (stale figure) | P2 | 88 → 94 in both languages |
| G2-3 | docs/src/guide/delivery-matrix.md:8 and docs/src/zh/guide/delivery-matrix.md:6 | "Verification date: 2026-10-13" standing as the page's only currency marker, one month on | re-verification run 2026-11-14 over the evidence classes named on that same line (`.github/workflows/`, `ctest -N`) | Mismatch (no re-check marker) | P2 | Date line now reads "2026-10-13 (ctest item count re-checked 2026-11-14)" in both languages |
| G2-4 | no doc page | strict CLI parsing semantics undocumented | server/src/serve_args.cpp:90-92 — `--assistant`, `--files-port`, `--model-port`, `--port`, `--webhook-port` et al.; strict rules (unknown flag rc2, bare positional rc2, missing trailing value rc2, negative port rejected, `--flag=value` inline form) have no page describing them (config-effects.md covers in-app settings and only `--uplink-port` as a server CLI key) | Code ahead of doc (missing) | P2 | Not fixed this round — todo.md entry (needs a new page, not an inline edit) |

Verified, no action: 23 EN guide pages ↔ 23 ZH pages, `comm -23` on basenames empty
(no EN page lacking a ZH twin); install.sh / install.ps1 both present and match the
README file-tree rows 81-83; nightly artifact channel claim (Actions artifacts are
intermediates, the rolling `nightly` Release plus `sha-`/latest image tags are the
delivery face) matches .github/workflows/nightly.yml:3-7 and README:332; README
platform badge (Windows | Linux | macOS | Android | iOS | HarmonyOS) matches the About
line's honest per-end status (Android shipped, iOS code-only with CI, HarmonyOS NEXT
code-only with CI logic verification); no ipk/OpenWrt artifacts claimed (none exist in
nightly.yml); C++17 + Qt 6 badge matches the CMake presets.

Language face: conforms to sanctioned form ② (vitepress `locales` — root = English,
`/zh/` = Chinese, both declared at docs/src/.vitepress/config.mts:98,126) **plus**
sanctioned form ① (README.md ↔ README.zh.md). URL contract probe passes:
`/` → 200, html lang `en`, first-screen CJK 4; `/zh/` → 200, html lang `zh-CN`,
first-screen CJK 46. Note: the probe's candidate-path list misses
`docs/src/.vitepress`, so memex reads `site=null` there — the site is real.
**No browser-language auto-switch**: no `navigator.language` / `rootLocale` /
`detectLocale` / Accept-Language handling anywhere in the config or in
docs/src/.vitepress/theme/index.ts (theme only registers ShowcaseCarousel and
imports custom.css). Recorded as "manual switching + explicit links only";
todo.md carries the P2 entry for the user to decide the scope.
