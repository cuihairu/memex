> **English** | [中文](group-remote-access.md)

# Group Remote Access Authorization: memex Handles Only the Authorization Relationship; the Channel Belongs to cockpit (Design 2026-10-15)

> Status: **design draft** (2026-10-15 user design brief; this round only lands the document, no code). The division of labor and red lines are pinned down by the user brief; the interface contract is a **draft**, and it may enter implementation only after being marked「待 cockpit 确认」.
> Related: the permission model (docs/design/权限模型.md — group roles / tier-order engine / capability gates / ledger), R16 remote assist (implemented, point-to-point consent; see todo gap inventory §5 — that is a different feature from this design: R16 manages「help me look at my screen」, while this design manages「authorize in a group who may connect to which machine」).

## 0. Division of labor and red lines (the core of the user brief; must not be broken)

| Layer | Ownership | Does memex implement it |
| --- | --- | --- |
| Remote-control channel SSH/RDP/VNC (the Guacamole stack) | **cockpit** (guacd byte pipe + tickets + egress policy + recording) | ❌ Never implement |
| Credential custody, policy confirmation, host suggestion | **persona** (vault + agent policy engine) | ❌ Never implement |
| **In-group authorization relationship** (who, authorized for what, which machine, what permissions) | **memex** | ✅ The only thing to do in phase one |
| Chat-window embedded session UI (presentation only; the channel comes from cockpit) | memex | Phase two |

**Current-state basis (verified 2026-10-15, not conjecture)**: cockpit's three protocols all already go through Guacamole (`/api/remote/guacamole` → guacd; Telnet falls back to the agent's raw TCP), with the design + real-device testing in cockpit's `docs/remote-access-integration-design.md`; persona already has SSH host management and a policy engine (`PERSONA_AGENT_REQUIRE_CONFIRM`/`ENFORCE_KNOWN_HOSTS`/`MIN_INTERVAL_MS`/`CONFIRM_ON_*`, the SSH Agent section of persona/TODO.md) plus an external token/listener integration surface (persona/docs/EXTERNAL_INTEGRATION.md); on the memex side there is no audio-call/screen-sharing frame-stream foundation (todo gap inventory §5: video conferencing/voice calls not implemented; screen sharing is only R16's JPEG polling).

### Red line: memex never touches credentials anywhere in the chain

- memex's authorization records **only carry**「who, authorized for what, which machine, what permissions」; **no password/private key/token is stored, forwarded, relayed, or appears in logs/audit**.
- Host registration stores only address/port/name/protocol availability, and **never stores connection credentials** — credentials are custodied by persona and injected by cockpit itself when a session is established (cockpit ticket fields `username/password/private_key` are a black box to memex).
- This design's interface contract (§5) **never carries a credential field in any direction**; during implementation, schema review treats「could a new field carry credentials?」as a checklist item.
- Basis: permission model §4 already has the same boundary (「credentials are never plaintext-visible just because you joined a group — permissions = actionable operations, while credentials still go through the encrypted surface」); this design generalizes that boundary from「group password vault」to「remote access authorization」.

## 1. Authorization model

### 1.1 Entities and relationships

```
Group ──registers──► Host (RemoteHost, an in-group resource)      Group ──members──► Account
   └──────────── remote_grants (authorization rows) ───────────────┘
              (group_id, account, host_id, level)
```

- **RemoteHost** (in-group host registration): `host_id`, `group_id`, name, address, port, protocol availability (ssh/rdp/vnc booleans, so the UI only allows the protocols the host actually has open). Registered by the group owner/admins; **contains no credential field of any kind** (red line).
- **remote_grants** (authorization rows): `group_id` + `account` (the authorized member, who must be in the group) + `host_id` + `level` (permission level) + `expires_at` (nullable = permanent).

### 1.2 Permission levels (four values; multiple rows per host stack)

| Level | Meaning | Mapping to a cockpit channel (pending cockpit confirmation) |
| --- | --- | --- |
| `ssh` | May initiate an SSH **interactive terminal** | guacd ssh (protocol=ssh) |
| `exec` | May **run a single command** (non-interactive) | the cockpit agent exec channel |
| `view` | **Read-only desktop** (watch the screen, no keyboard/mouse input) | guacd rdp/vnc + read-only |
| `desktop` | May initiate an RDP/VNC **interactive desktop** (with keyboard/mouse) | guacd rdp/vnc |

**Rationale for the granularity**:
1. **Channels are orthogonal capabilities and cannot be collapsed into a single ordered level.** "May run a command" and "may view the desktop" do not imply each other — if you rank them 1<2<3, authorizing "SSH only" would inevitably drag in the desktop, or vice versa, violating least privilege. The four values each govern one channel action, and the group owner selects as needed.
2. **The view/desktop boundary is input, not imagery.** The highest-risk remote-control action is keyboard/mouse input (you can type commands, you can power off the machine); watching the screen itself is one notch lower in risk. Diagnostic scenarios ("take a look at what's on the screen") only need view.
3. **The exec/ssh boundary is an audit stance.** exec is a one-off, known command body (cockpit records the command verbatim, reconcilable afterward); an interactive shell is arbitrary operation (pursuable only via recording playback). Automated patrol-style authorization needs only exec, with no need to hand a person a full shell.
4. **One level per row, zero-migration channel expansion.** Adding a new channel later (e.g. Telnet, a database gateway) = a new enum value + a new authorization row, leaving existing rows untouched; this saves one full migration compared with a bitmask.

### 1.3 Relationship with the group capability gates

The group capability list (the seven current items in `group_capabilities`) **gains an eighth, `remote`**: the entire remote-authorization tool (host registration + authorization management + session entry) is switched per group. Unlike the existing seven, this item is **deny-by-default on registration** (remote is a genuine control-plane entry point, security-sensitive, closed by default; this is a deliberate deviation from the existing "unconfigured = currently allowed" first-step posture, and the reason is noted here).

## 2. Authorization lifecycle

| Decision point | Ruling | Reason |
| --- | --- | --- |
| Who may authorize | **Group owner + group admins** (in-group); org-admin via the new az rule `remote-authz-manage` (cross-group fallback, Explicit Allow tier) | Aligned with existing precedents (group announcements / group profile = settable by group owner/admins); cross-group management is isomorphic with `branding-manage` |
| Can a member authorize | No (Default Deny) | Permission model §2: members = use capabilities, do not configure |
| Can it be re-delegated | **Not supported in phase one** (no delegation chain) | A delegation chain dilutes accountability (you can't say who authorized); if needed, extend later via scope rules, not in this phase |
| Sessions already established after revocation | **memex does not kill sessions** — the channel belongs to cockpit (red line §0 division of labor). memex revocation takes effect immediately on the **next connection-time validation** (pull mode, §5) and pushes a revocation notification to cockpit, with cockpit deciding whether to terminate an online session (SHOULD semantics in the contract; pending cockpit confirmation); the memex-side session ledger marks revoked | memex reaching into the channel = breaking the division of labor; "reject new sessions immediately + let cockpit rule on online sessions" is the strongest semantics achievable within the boundary |
| Leaving the group / being removed | All authorization rows for that group cascade-invalidate (choose between filter-at-query and physical delete: **ruling is filter-at-query**, with rows retained for audit traceability) | Permission model's "leaving the group = loss"; keep the row, not the right |
| Host deletion | All authorization rows for that host invalidate likewise (query filtering) | Same as above |
| Expiry | `expires_at` auto-invalidates at the deadline (a sweep scan, not lazy evaluation at query time — audit must record the expire event) | Temporary authorizations (duty windows) are common; the event must leave a trace |

## 3. Storage and audit

Four new tables in the memex server-side database (files surface 24561, evolving with the existing schema):

```sql
remote_hosts   (host_id PK, group_id, name, address, port,
                ssh_ok, rdp_ok, vnc_ok, created_by, created_ms)
remote_grants  (grant_id PK, group_id, account, host_id, level,
                granted_by, granted_ms, expires_at NULL,
                revoked_ms NULL, revoked_by NULL)
remote_authz_audit (id PK, ts_ms, actor, action, group_id, host_id,
                target_account, level, detail)
remote_sessions    (session_id PK, grant_id, group_id, account, host_id,
                protocol, mode, started_ms, ended_ms NULL,
                end_reason NULL, revoked INT DEFAULT 0)
```

**Audit field stance (full trace for authorization changes)** — `remote_authz_audit` has one row per event, `action ∈ {host_add, host_update, host_remove, grant, revoke, grant_expire, capability}`, with `detail` recording the before/after delta (e.g. the before/after values of a level change); **credentials are never recorded** (red line). `remote_sessions` is written by cockpit callbacks (§5), isomorphic with R16's assist audit (precedent at `files_client.cpp:1000-1124`). The trace iron rule aligns with todo §0: audit is append-only, never modified or deleted; the group owner/admins may query their own group, and org-admin may query everything.

## 4. External boundary interface contract (draft, pending cockpit confirmation)

Form: memex files-surface HTTP JSON (24561), prefix `/files/remote/`, isomorphic with `/files/assist/*`. **No request or response carries a credential field** (red line). The authentication method (cockpit logging in with a service account via FilesClient, or org-admin issuing a long-lived token) is **pending cockpit confirmation**.

### 4.1 memex → cockpit connection entry (used in phase two; phase one only fixes the shape)

After obtaining authorization, the memex client asks cockpit for a session, passing「person + machine + permission」, and cockpit fetches credentials from persona itself:

```
cockpit: POST /api/remote/sessions/memex        (pending cockpit confirmation)
  { grant_id, account, host_id, protocol, mode }   ← no credential field of any kind
→ { session_url, ticket, expires_in }
  ticket is one-time, short-lived (cockpit's existing 5-minute stance unchanged)
```

### 4.2 cockpit → memex validation and callbacks (implemented in phase one; contract first)

```
memex: POST /files/remote/authz/check
  { group_id, account, host_id, protocol, mode }
→ { allowed: bool, grant_id?, level?, expires_at?,
    reason: "allow:<level>" | "deny:<reason>" }     ← tier-order vocabulary same as permission model §5

memex: POST /files/remote/authz/audit              ← cockpit session callback
  { event: "session_started" | "session_ended",
    session_id, grant_id, end_reason? }
```

- **The pull validation runs at the connection point** = revocation takes effect immediately on the "next connection" (§2).
- `mode ∈ {interactive, view, exec}` mapped to levels: interactive→ssh/desktop, view→view, exec→exec; if the level does not include the corresponding action, it is a deny.
- Revocation notification: on revoke, memex **reverse-pushes** a `POST {cockpit}/api/remote/authz/revoked` (body: grant_id; the address and endpoint are **pending cockpit confirmation**), and cockpit terminates online sessions as needed, returning session_ended.

### 4.3 What explicitly does not enter the contract

Credentials (in any form), persona tokens, cockpit's internal RBAC details, recording/egress policy — all closed within their own repositories; memex only recognizes grant_id semantics.

## 5. Phasing and what is not done

- **Phase one (the scope of this design)**: authorization model + storage + audit + management/validation HTTP surface (`/files/remote/` grant/revoke/list/check); no client management UI (curl/CLI can operate it, and the contract can be verified).
- **Phase two**: chat-window embedded session UI (iframe/WebView mounting cockpit's session_url, presentation only); host registration synced from cockpit's agent inventory (pending cockpit confirmation); authorization management dialog; file transfer (via memex's existing direct file channel, unrelated to the remote-control channel).
- **Not scheduled (no foundation)**: audio/video classes (voice calls / video conferencing / real-time screen stream) — they depend on capture/encoding/frame-stream foundations; a separate brief once the foundation is project-started (already registered in todo gap inventory §5).
- **memex never does**: SSH/RDP/VNC protocol chains, guacd integration, credential custody/injection, egress policy/recording (§0 division table).

## 6. Alignment checks (must pass before implementation)

1. The contract draft is confirmed by cockpit (endpoint names / parameters / authentication method / revocation push direction);
2. The persona side confirms the integration form of "cockpit fetches credentials when establishing a session" (persona already has a token/listener surface; this is a cockpit↔persona bilateral matter, and memex does not involve itself in the details);
3. Schema review of the az engine's new `remote-authz-manage` and the `group_capabilities` new `remote` item (including the red-line check item: new tables / new interface fields carry no credentials);
4. Re-review of the audit ledger against the trace iron rule (append-only, never modified or deleted).
