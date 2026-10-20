# Server-Side Authorization Inventory (Platform-1)

> Corresponds to the todo "server-side authorization dedicated review": for each sensitive operation, inventory the trio of "server-side authorization decision + explicit denial path + regression test". The outputs are the inventory table and the gap-filling tests; no engine refactoring.

## Engine Conventions

All authorization decisions go through `AuthorizationService` (`server/src/authz.cpp`): a single entry point `authorize(subject, action, resource, context)`. The three rule tiers are evaluated as **ExplicitDeny > ExplicitAllow > Inherited** (within conflicting tiers, the nearest scope wins); no match falls to `default-deny` — a whitelist, not a blacklist. Rule predicates query `ServerStore` live at decision time (group membership, reporting chains, and role validity periods are all per-decision queries, never cached), so revocation and transfers take effect immediately. All four planes share the same engine and path: the collaboration protocol plane (`server.cpp`/`session.cpp`), the file plane (`files_server.cpp`), the admin plane (`admin/src/admin_app.cpp`), and search/export (`messages --as`).

## Trio Determination

| Plane | Decision point | Denial path | Regression test |
| --- | --- | --- | --- |
| Collaboration: group creation / member add | `group:create` + membership | `GROUP_RESULT ok=false` + reason | `test_group.cpp` |
| Collaboration: group messages | Membership checked live (sender) | Silently dropped, not archived | `test_group.cpp` (non-member / after dissolution) |
| Collaboration: message sender identity | Bound session identity (added this round) | Rejected + audit log | `test_authz_audit.cpp` |
| Collaboration: recall | `original_from == account_` | Silently dropped + log | `test_archive.cpp` |
| Collaboration: delivery/read events | Recipient checked live (added this round) | Rejected + audit log | `test_authz_audit.cpp` |
| Collaboration: device gate | Device registry at login | `LOGIN_RESULT ok=false` + reason | `test_accounts.cpp` |
| Collaboration: direct file authorization | Unified four questions (same department / cross department / allow / re-forward / unknown recipient) | `FILE_AUTHZ_RESULT allowed=false` | Engine leg `test_authz.cpp` |
| File: upload/download/delete/pin/quota | `file:*` rules + capability gate | HTTP 403 + reason | `test_files_api.cpp` |
| File: group file access lost on leaving | Group membership checked live per request | HTTP 403 (leg added this round) | `test_files_api.cpp` |
| File: personal/group memos, vault, group tools, CI, credentials plane | `memo:*`/`vault:*` / capability gate | HTTP 403 | `test_files_api.cpp` |
| File: tasks/approvals/reports/audit | `task-assign`/`approval-decide`/`report-read`/`audit-read` (including self-decision ExplicitDeny) | HTTP 403 (denial precedes the existence check) | `test_files_api.cpp` |
| File: token scope (intranet / uplink dual gate) | Token scope comparison | HTTP 401/403/404 | `test_uplink.cpp` |
| File: group assistant / webhook access | token + group whitelist + membership | 401/403/404 | `test_bot.cpp` / `test_webhook.cpp` |
| Admin: accounts/orgs/roles/policies/retention/devices | `admin:manage` → org-admin (effective_roles recomputed per request) | 403 + denial log | `admin/tests/test_admin_api.cpp` |
| Admin: archive search export | `audit:read` → auditor (org-admin is not automatically readable, platform-6 red line) | 403 + `audit.denied` persisted | `admin/tests/test_admin_api.cpp` |
| Search CLI: `messages --as` | auditor credential check | Exit code + `audit_reads` audit trail | `test_visibility.cpp` |
| Engine itself | Three-tier priority / nearest scope / default-deny | No match means denial | `test_authz.cpp` |

## Gap-Filling This Round (Four Code Sites + Eight Tests)

| Gap | Fix |
| --- | --- |
| The collaboration plane's TEXT/NUDGE archive and delivery take the sender from the client-supplied `from` and never compare it against the session identity (recall and file authorization both use `account_`; only these two sites) | Add a binding gate at both `session.cpp` entries: `msg.from() != account_` rejects and logs (same convention as "unauthorized recall rejected"). The dedup key `sha256(from:seq)` is bound to the session identity accordingly |
| A non-recipient can record `delivered`/`read` events on others' messages and push fake `DELIVER_NOTICE`/`READ_NOTICE` to the original sender (queue eviction is already bounded by `to_account`; only the event ledger and the notification plane missed the check) | `session.cpp` ACK/READ goes through `message_by_id` to fetch `to_account`, and `is_recipient()` is checked live (direct message = self; group message = group member); non-recipients are rejected with no trace and no notification |
| No regression leg covers group file access after leaving the group (group membership is judged live, but "leaving loses access" was never covered by tests) | Add a leg in `test_files_api.cpp`: download returns 403 after `group_leave`, 200 after membership is restored |
| No test pins the behavior after a device is disabled | Pin the current behavior in `test_authz_audit.cpp`: existing sessions are unaffected; a new login with the same fingerprint is rejected |

## Known Conventions and Open Decisions (recorded faithfully in the inventory; not a defect list)

1. **Accounts have no "disable" control**: the `accounts` table has no `enabled` column; `credentials.disabled` exists but has no write path anywhere in the repo. The only revocation lever today is the device registry `devices.enabled`. "Disabling an account" would require a new column + write surface + login/session validation surface — a product decision.
2. **Revocation only blocks new logins**: disabling a device does not kick existing collaboration sessions (the session state `logged_in_` is set once at login), nor does it invalidate already-issued file-plane tokens (in-process table + 12h TTL, cleared on restart) or admin sessions. The admin plane is the exception: roles are recomputed per request, so revocation takes effect immediately. Whether "disable device = kick existing sessions" requires server-side cooperation (the admin is a separate process; only the server can sense the kick) — a decision item.
3. **File-plane token issuance does not check devices**: `/files/session` only verifies credentials and has no `new_device_approval` gate (both collaboration login sites check it) — file-plane tokens carry no device-binding semantics, and a leaked token remains usable until its TTL expires.
4. **Admin department-tree read is login-only**: `GET /api/admin/departments` does not enforce `require_manage`; any logged-in account can view the org tree (all write operations are permission-gated). Convention = the org directory is visible to logged-in members, same as the address-book plane.
5. **Recipients are not checked for existence**: direct TEXT and NUDGE `msg.to()` perform no account-existence/relationship validation; messages to nonexistent accounts are archived and queued as usual (orphan rows). A spam and orphan-volume concern, not unauthorized reads.
6. **Recall forward targets are client-chosen**: recall itself is validated against the sender, but the notification fan-out uses the client-supplied `msg.to()` (which can point to a third party).
7. **The CLI has no authorization gate**: `main.cpp` subcommands directly open the same SQLite database — a local-ops trust model (file permissions are the boundary); the only exception is the search/export plane, which enforces auditor credentials and records `audit_reads`.
8. **Pre-login frames are silently dropped**: the collaboration plane returns no error frame for unlogged frames, with no audit trail (without credentials there is nothing to attribute).

## Enterprise Directory Authentication (LDAP/AD)

todo 544 (step 2 of the external three-step suggestion; step ③ OIDC awaits clarified requirements): a pluggable authentication backend that does not change the lightweight deployment or the independent trust model of login-free direct connections — **with no `--ldap-host` configured there is zero LDAP path** (the construction never happens); local accounts (platform-2 password) are always available = an independent intranet and emergency-recovery entry point.

Red-line conventions (implemented in `server/src/ldap.hpp`/`ldap.cpp`, a minimal hand-written BER client, no openldap dependency):

| Red line | Implementation |
| --- | --- |
| An unavailable directory never bypasses authentication | fail-closed: accounts bound to issuer=ldap are always refused at login when the directory is unreachable / unconfigured / half-configured (`LdapUnavailable`); never fall back to the local password — fallback is bypassing the directory. The error is stated explicitly in logs and the denial reason; no silent degradation |
| Passwords never in cleartext | The password travels only once, in the bind message: never persisted, never in logs/LoginRecord/audit bodies (the audit records only who/when/how/success-or-failure; a successful LDAP login is recorded as `ok_ldap`). Exactly one of three TLS states: `--ldap-ssl` (ldaps for the whole connection) / `--ldap-starttls` / `--ldap-allow-plain` (cleartext denied by default; startup validation exits with code 2 on violation); self-signed certificates via `--ldap-tls-insecure` (startup warning) |
| Explicit group→role mapping | `--ldap-group-role 组DN=角色名` (repeatable; env `MEMEX_LDAP_GROUP_ROLE` comma-separated). Groups outside the mapping table are ignored = ordinary members; an empty table = no automatic authorization at all; a table without admin never grants admin — the directory admin group is not implicitly privileged (startup warning when the explicit mapping contains admin) |
| Directory-disabled | New logins are refused directly (service mode checks the disabled triple before binding = service-permission convention; AD disabled bit `userAccountControl` + rule `1.2.840.113556.1.4.803` + value `2`, attribute configurable). Existing sessions: `--ldap-kick-disabled` (default 1 = kick) + `--ldap-recheck-seconds` (default 60s) recheck; only an affirmative Disabled kicks — an unreachable directory does not cause false kicks |
| First-login identity binding | identities table: issuer=`ldap://<host>`, subject=user DN (template mode is the escaped bind DN). No automatic binding by default: `--ldap-auto-bind` (existing account without binding → bind on successful directory authentication) and `--ldap-auto-provision` (account does not exist → create a shadow account with a random, unknowable local password = directory-only login) are both off by default; when off, only accounts explicitly bound via the `identity bind` CLI go through directory authentication |
| Local accounts always available | Accounts not bound to issuer=ldap go through the original local authentication (zero directory contact, no probing); directory-plane failures do not affect local accounts or login-free direct connections |

Binding path, choose one of two: template direct binding (`--ldap-bind-dn-template uid={u},ou=...`, common with OpenLDAP) or service-account search-then-bind (`--ldap-bind-dn` + password, common with AD — when the DN cannot be constructed, locate it first via `--ldap-user-base`/`--ldap-user-attr`). The service-account password is injected via the env `MEMEX_LDAP_BIND_PASSWORD` (not into shell history/ps; config files must be 0600).

Circuit breaker: after one directory-unreachable event, a 30s cooldown fail-fast (when the directory is down, each login attempt must not stall a collaboration session for a full timeout duration). The full operator view of the directory-unavailable determination and the group-mapping conventions is in `packaging/ldap.env.example` (annotated example configuration).

Tests: `server/tests/test_ldap.cpp`, 16 legs (fake directory = in-process POSIX listener with scripted bind/search responses): TLS posture, online verification of both service/template modes, wrong password 49, user not found, disabled hit (user binding does not occur), unreachable + circuit-breaker fail-fast, DN/filter escaping, routing matrix (unbound defaults to Local with zero contact / bound → LdapOk / wrong password → LdapBad / directory unconfigured → Unavailable), auto_bind/auto_provision off by default, unmapped group grants zero authorization, session-level real login chain (success + role sync / fail-closed rejection / disabled rejection / local account unaffected), existing sessions kicked offline. The three mandatory test categories — LDAP unavailable / group unmapped / directory disabled — correspond to legs 6/13/5+16 respectively.

## Test Index

`server/tests/test_authz.cpp` (engine), `test_accounts.cpp` (device gate and login), `test_archive.cpp` (archiving and unauthorized recall), `test_group.cpp` (group relations and permission carriers), `test_files_api.cpp` (file-plane full chain 401/403/404), `test_uplink.cpp` (token scope dual gate), `test_webhook.cpp` / `test_bot.cpp` (access planes), `test_authz_audit.cpp` (gap-filling this round), `test_ldap.cpp` (enterprise directory authentication, 16 legs), `admin/tests/test_admin_api.cpp` (admin plane, 27 legs).
