# Code Review: Full-Repo Deep Review (2026-11-01)

> Scope: full repo — server/ (asio FileServer + protocol + CLI), client/ (Qt6 desktop, engine + app),
> common/ (frame codec), admin/ (Drogon console), apps/ (Android/iOS/HarmonyOS read for stance).
> Process: user-specified six-point spec ①security static scan ②hidden-bug hunt ③quality gate
> ④this report ⑤P0/P1 fix-now / P2/P3→todo ⑥counts+commits; discipline per the code-review skill
> (every finding verified at file:line by this reviewer, no agent claim passed through unchecked).
> Engine validation: NOT ASSESED — no engine configured (game-engine skill, N/A here); the skill's
> verify-every-finding rule was applied instead.

## ① Security Static Scan

| Category | Verdict | Evidence |
|---|---|---|
| Hardcoded secrets | **PASS** (1 deployment-grade P3) | No secrets in code paths: passwords PBKDF2-HMAC-SHA256 60000 iters (server/src/cred.cpp:53), sessions/AES-256-GCM, Ed25519 device identity, HKDF-SHA256 channel keys. Test literals are fixtures inside tests/. The object-store compose generator defaults to `minioadmin/minioadmin` (CR-4 below, P3, documented "must change at deploy"). |
| Shell injection | **PASS** (1 P3 defense-in-depth) | Only 3 `std::system()` sites in the repo, all operator-CLI (compose generator), zero network-reachable: server/src/storage.cpp:350,355,364. Inputs are operator-supplied config paths/endpoints, not request data; unquoted → CR-3 (P3). No `popen`/`exec*` elsewhere; HTTP request paths never touch a shell. |
| SQL injection | **PASS** | 339 `sqlite3_prepare_v2` sites / 840 `sqlite3_bind_*` / **zero** SQL string concatenation with request data — parameterized throughout (grep over server/src+common/src). The one dynamic-SQL function (list_group_memos, store.cpp:7295) concatenates only static fragments + escaped-LIKE placeholder and still binds the pattern. |
| eval / dynamic code | **PASS** | No eval-class mechanism in any request path (C++17; no dlopen, no embedded interpreter). |
| Deserialization | **PASS** | Wire formats are protobuf (common/src/frame codec) and JSON. `json::parse` at 85 sites repo-wide; the HTTP boundary (files_server.cpp) wraps parses in try/catch → 400 (spot-verified at every handler class read). No raw C++ object deserialization, no unchecked casts on wire data. |
| Path traversal | **PASS** | Object keys are server-computed (`groups/{gid}/…`, `users/{uid}/{sha256}`); client-supplied filenames are stored as DB metadata, never used as filesystem paths. Download emits a header-safe filename filter (files_server.cpp:3720-3745). Percent-decode is bounds-checked (`i + 2 < raw.size()` before reading i+1/i+2, files_server.cpp:121-129). |

## ② Hidden-Bug Hunt (verified findings, severity order)

### CR-1 (P2 — FIXED this round) Stale agent heartbeat badge text

- **Where**: client/app/group_server_dialog.cpp:74.
- **Defect**: connected badge reads "已连接（agent 心跳 15s · 掉线 60s 判离线）"; the real constants are
  heartbeat 30s (server/src/agent_main.cpp:139 `interval_s = 30`) and offline judgment 90s
  (server/src/files_server.cpp:35 `kAgentOnlineMs = 90 * 1000`).
- **Impact**: misleads the operator about group-server liveness timing (expects离线 at 60s; server holds
  90s) — the same drift was present in docs (audit round 6, finding E5); source-wins, UI text corrected to
  "30s · 90s".
- **Fix**: one-line literal change; see the fix commit in ⑥.

### CR-2 (P2 — to todo) `atoi` overflow on `limit`/`offset` can produce negative SQL LIMIT

- **Where**: server/src/files_server.cpp:1405,1409 / 3778,3782 / 4141,4145 / 4245,4249 (four list
  handlers: uplink mine, inbox list, memo list, group memo list) → server/src/store.cpp list_uplink_files
  (:6956, binds `LIMIT ? OFFSET ?`), list_memos (:7108), list_group_memos (:7295).
- **Defect**: the query params are digit-gated (`find_first_not_of("0123456789") == npos`) but **not
  length-clamped**; `std::atoi` on a ≥20-digit string is UB — glibc's strtol saturates to LONG_MAX and the
  int cast wraps to `-1`. `LIMIT -1` in SQLite means **no limit**: pagination silently disappears and the
  whole (owner/group-scoped) result set is materialized into a vector.
- **Impact**: authenticated-only, scoped to the caller's own uplink files / memos / group memos — an
  exhaustion-ish primitive, not a cross-tenant leak. Real but low blast radius on the single-process server.
- **Fix direction** (todo backlog): clamp digit length (e.g. >9 digits reject) and/or `std::stol` in try +
  cap (e.g. limit ≤ 1000) at the four atoi sites.

### CR-3 (P3 — to todo) `std::system()` with unquoted operator-supplied strings

- **Where**: server/src/storage.cpp:350,355,364.
- **Defect**: compose commands are built by string concatenation of `compose_path` / endpoint / bucket and
  passed to `std::system()` unquoted. All inputs are operator CLI config (not network), but a path
  containing spaces/metacharacters breaks (or worse) — defense-in-depth only.
- **Fix direction** (todo backlog): quote/escape arguments or argv-form spawn.

### CR-4 (P3 — to todo) `minioadmin/minioadmin` default credentials in the compose generator

- **Where**: server/src/storage.hpp:29-30,115-116 (defaults on `S3Storage::ensure_compose`) and
  server/src/main.cpp:2942 (CLI fallback).
- **Defect**: generated RustFS compose comes up with vendor-default credentials; the header comment says
  "部署时须改" (storage.hpp:112) and the CLI offers changing them, but nothing forces it.
- **Fix direction** (todo backlog): print a loud warning when generating with defaults / generate a random
  secret and print once.

### Verified-clean spots (checked, no finding)

- common/src/frame.cpp — length-prefix codec fully bounded: `kMaxFrameSize` on encode (frame.cpp:29) and
  decode (frame.cpp:52), partial-buffer arithmetic guarded (frame.cpp:45,55).
- server/src/cred.cpp from_hex (cred.cpp:40-48) — even-length guard before the `i+1` read; invalid char
  → empty.
- client/engine/task/task_provider.cpp — the dual-list ownership pattern (custom items in both `items_`
  and `custom_items_`) is handled correctly: destructor `qDeleteAll(items_)` (:70-71), reload removes
  custom items from `items_` **before** `qDeleteAll(custom_items_)` (:79-81) — no UAF/double-free.
- Threading: single asio io_context for message/file/uplink/webhook planes (by design, documented);
  ModelGateway and GroupAssistant own threads with own DB handles; spot-reads of the shared-session
  paths (session auth, sender binding, uplink scope gates) found no cross-thread mutation.
- URL decode / query parsing (files_server.cpp:105-140) — bounds and separator checks verified.

## ③ Quality Gate

- Build: Release tree full build green (554 targets) — receipt in ⑥.
- Tests: full `ctest` on the Release tree — result recorded in ⑥ (counts below).
- Lint/type-check: no clang-tidy/cppcheck config or lint CI step exists in this repo (hand-written test
  macros, no gtest); honest verdict: **NOT CONFIGURED** — not "passing". TS/ArkTS sides have `tsc`
  type-check in CI (apps/harmony), not re-run in this review (out of the C++ deep-read scope, no C++
  surface touched by this review's findings beyond CR-1).

## ⑤ Disposition per the brief (P0/P1 fix-now, P2/P3→todo)

- **P0/P1: none found.** The scan surface (SQL/shell/deserialization/path/crypto) is defensively coded.
- **P2/P3 → todo**: CR-2, CR-3, CR-4 appended to todo.md backlog this round (one line each, evidence
  file:line).

## ⑥ Counts and Commits

- Findings: **4** (P2 ×2, P3 ×2). Fixed this round: **1** (CR-1). Remaining (todo backlog): **3**.
- Commits:
  - CR-1 fix: `fix: correct agent heartbeat text in group server badge`
  - CR-2/3/4 backlog + this report: `docs: add code review report and p2 p3 backlog`
- Gates: build green / ctest result: *(filled at landing — see below)*.

> **Gate receipt (2026-11-02, added at landing):** Release build green (554 targets, badge object
> recompiled after the fix); ctest 88/88 passed (Release tree, 253 s); docs site build green
> (`vitepress build src`, the exact command `pnpm docs:build` expands to — local pnpm 12 refuses the
> legacy `pnpm.overrides`; CI pins pnpm 10). This file lives outside docs/src, so the site build does
> not process it.
