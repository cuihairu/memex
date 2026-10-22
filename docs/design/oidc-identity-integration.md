> **English** | [中文](../zh/design/OIDC身份认证接入设计.md)

# OIDC Identity Authentication Integration Design (Design Spec, Not Implemented)

> Status: this document is design only; no code has been changed. Implementation is scheduled after the user reviews and decides.
> Corresponds to plan §二
> (Identity/IdentityBinding already reserved) and §三 (Authorization) of docs/design/基础平台化改造计划（身份-组织-授权-归档）.md.
> Terminology discipline: use only established terms (OIDC/PKCE/claims/groups/sub/RP/IdP/discovery/JWKS/
> id_token/access_token); no invented words; where unsure, mark 「待定」.

## 一、Ruling

**L2 ruling (user decided 2026-10-12): the deployment ships its own lightweight IdP container (inside compose)
as the default identity source; memex is only an OIDC client (RP) and does not implement its own provider.**

The wording immediately following the ruling: **"default = classic account-password login; OIDC = optional path (enabled only when an
IdP is configured)"** — the existing default experience is unchanged, and a deployment with no IdP configured behaves exactly as today.

Why client only, and not its own provider:

- Implementing a provider means shouldering the entire security responsibility of the issuing side yourself: key generation and custody, JWKS
  publication and rotation, token signing algorithm and validity, revocation, audit — each one is a live security surface,
  and getting any one wrong means the identity side is compromised;
- A standard client's responsibility surface is only a trio (see the §二 division table): discovery, JWKS signature verification
  (iss/aud/exp/nonce), sub+groups mapping — signature verification is "verify only, never sign", so the consequence of an error is
  "login refused" rather than "wrong identity issued"; a refused login is a recoverable failure, an issued wrong identity
  is a security incident;
- Group and account lifecycle management (creating groups, adding people, removing people, changing groups) is a mature admin surface of the IdP,
  and implementing it yourself is rebuilding an organization management backend.

L3 (memex acting as issuer itself to issue tokens to external systems) is **recorded only, not scheduled**: it is picked up only if the
requirement "an external system wants memex to issue tokens for it" actually appears (that time requires filling in the provider
responsibility surface: key management, rotation, JWKS publication, etc.).

## 二、Division Table

Each of the three parties owns one segment; the boundaries are cut along token flow:

| Matter | Client (RP side) | memex server | IdP |
|---|---|---|---|
| Login interaction | System browser runs authorization code + PKCE (public client, redirect_uri=127.0.0.1 loopback port, RFC 8252 convention) | — | Shows the login page, issues the authorization code |
| Token exchange | Exchanges code + verifier with the IdP for id_token (no client_secret, public client) | — | Token endpoint issues id_token |
| Signature verification | — | discovery (fetches issuer configuration at startup) + cached JWKS signature verification: signature/iss/aud/exp/nonce, all must pass before acceptance | Signs and publishes JWKS |
| Identity mapping | — | (iss, sub)→account binding; groups→department/group mapping | Authoritative source of claims content (sub/email/name/groups) |
| Session | Holds a memex session (the same entity as a session after password login) | Self-issues and self-manages sessions (§三) | — |
| Group management | — | Read-only consumer of the groups claim | The admin surface for creating groups, adding people, removing people |

Key point: the memex server **does not** call the IdP per request — signature verification happens only at login acceptance,
and there is no persistent dependency between online sessions and the IdP (§三).

## 三、Server Session and Token Lifecycle

This part is the easiest place to cause live incidents, so it gets its own section. Principle: **the memex session is self-issued by memex,
and its lifetime is decoupled from the IdP token**.

- **Login exchanged for a session**: the client sends an id_token → the server's trio signature verification passes → look up the binding to get
  the account → go through the existing login registration (device fingerprint / new-device approval / login record / same-kind mutual kick, all
  existing behavior unchanged) → issue a memex session. From then on the connection holds only a memex session; the id_token is
  discarded once used.
- **access token**: the access token the client obtains from the IdP has
  **no use** on the memex business surface (the business surface goes entirely through self-issued sessions); it is discarded by default and never persisted to disk.
- **Refresh**: the refresh token is not persisted by default (the session is dropped from memory, and after it expires the
  login interaction is run once again); whether to do silent renewal is 「待定」 — reassess only if there is a real need, not scheduled.
- **Degraded behavior when the IdP goes down** (the wording is fixed):
  - Logged-in sessions are **retained without exception**: signature verification happens only at login acceptance, and an unreachable IdP is never a
    reason to kick anyone — **never silently kick people offline**;
  - New OIDC logins are unavailable: if a new id_token cannot be obtained or JWKS cannot be verified, report
    「IdP 不可达」 explicitly, with no retry storm;
  - **The password path stays available** (this is exactly the fallback value of "keeping password login as the default path");
  - Signature verification **must not be skipped**: when JWKS cannot be fetched, degrade to "refuse new OIDC logins and state the reason",
    never degrade to "skip verification and let them through";
  - After the IdP recovers, no manual intervention is needed: discovery/JWKS is re-fetched automatically at the next login.

## 四、Direct-Connection Mode Boundary

Direct-connection mode (serverless local use) has no IdP and does not need one:

- Keep the existing local authentication entry point; behavior unchanged;
- The two entry points (direct local authentication / server OIDC or password login) converge on **the same internal
  identity object** (plan §二 Identity model: Account/Device/Session/Credential/
  IdentityBinding) — an OIDC login maps to an existing account on the server,
  **no parallel account system is created**;
- The upper az rules, archive, and audit recognize only this internal identity object; there is no separate
  adjudication path for an "oidc user".

## 五、Identity Binding and the sub Convention

- The binding lands in the IdentityBinding already reserved by plan §二: `identity_bindings(idp_iss,
  idp_sub) → account`, with a unique constraint on (idp_iss, idp_sub); on the first OIDC login it
  binds to an existing account (the binding confirmation method is 「待定」: administrator pre-registration vs. first-login page confirmation —
  one of the two during the design phase).
- claims mapping: sub→binding key; email/name→existing account fields (the status of the email column is 「待定」;
  if there is no column, store it only as a binding extension); groups→department/group (§六).
- **sub enters archive and audit as the stable identifier, not the username**: newly written archive rows/audit rows
  carry sub (renaming an account does not affect traceability); existing rows are not backfilled, and retrieval uses the account↔sub
  join convention.

## 六、Authorization Surface: az Reading groups and the Compatibility Period

- Current state: rules such as `cross-department` query and adjudicate on the spot against the own department path
  (server.cpp `member_profile().department_path`), and group rules against the own group table.
- Change: when the IdP groups claim is the group source of truth, sync it into the local mapping at login acceptance
  (groups→`ensure_department_path` department path; which IdP groups count as memex groups
  is 「待定」).
- **Adjudication during the compatibility period (how to adjudicate while the old department table still exists)**: an account-level identity-source switch
  (local=old department table / idp=groups mapping), **a single source of truth, no mixed weighting**
  — the same account has only one source of truth at the same moment, and the switch decides which one it is; cross-source comparison
  (the same account's local and idp departments disagree) is handled on the strict side, and conflict alerts enter the audit.
  Suggestion: existing accounts default to local (zero change to current behavior), new/migrated accounts default to idp;
  the default values await the user's decision.
- Acceptance anchor: an IdP group removal → after the next login sync, the az adjudication changes (e.g. cross-department file
  sending becomes forbidden).

## 七、Boundary with the Password Vault

- **OIDC only answers "who you are"**; external tool credentials (the
  appKey/appSecret of 钉钉/飞书/GitHub provider, etc.) keep going through the password vault (PBKDF2→KEK, AES-256-GCM encrypted on disk,
  the current R24-3/R27-3 convention); the two do not replace each other.
- OIDC's own session credentials (id_token/access_token) do not enter the password vault: the session is dropped from memory
  (§三).
- Machine-to-machine (the client credentials flow): **marked "possibly needed later", not done** —
  reassess only if the scenario of a server component proving itself to the IdP actually appears.

## 八、Deployment and Cost (listed honestly)

- **One more resident container group**: the IdP itself + database (+ optional cache); memory estimated at the GB level
  (subject to measurement once the choice is settled, see §九).
- **One more backup surface**: the IdP's own database must enter the backup plan (paired-shot convention like memex's
  two assets); the backup strategy awaits the user's decision (§十二).
- **Upgrade surface**: the IdP is an independent upstream with a faster upgrade rhythm, so version follow-up and regression are required.
- **Forgotten master password = total lockout**: if the IdP administrator password is lost, the whole identity surface becomes unmanageable.
  **A local recovery entry point is mandatory**: set an emergency administrator at deployment + keep recovery means saved offline; the concrete
  reset flow (the recovery flow / in-container reset command) is verified by measurement after the choice is settled and then written into the deployment
  documentation (「待定」: the concrete commands).
- **Deployment form**: 1 compose becomes 2 (memex compose + IdP compose);
  network requirements: the memex server can reach the IdP (discovery/JWKS), and the user browser can reach the IdP
  (the authorization endpoint).
- The dual-track fallback makes the failure surfaces above controllable: if the IdP is fully down, the password path can still log in (§三).

## 九、IdP Selection Comparison and Recommendation

| Dimension | Authentik | Keycloak | Zitadel |
|---|---|---|---|
| Resource footprint | Medium: server+worker+DB, within the GB range | Heavy: JVM, the 1–2 GB range | Light: a single Go binary+DB, the hundreds-of-MB range |
| OIDC + group mapping | groups come with the default claims, zero custom mapping | Group mapping is the strongest (protocol mapper), needs configuration | Mainly the roles claim; the groups convention is to be verified |
| Backup difficulty | Back up its own DB (+ stateless worker) | Back up the DB (all configuration lives in the DB) | Back up the DB **+ masterkey** (lost = no way to unlock) |
| License | Core is open source (MIT convention, the enterprise edition sold separately) | Apache-2.0 | Apache-2.0 (core) |
| Form | Standard container component, mature UI and documentation | Standard container component, a huge amount of documentation | Single binary, the most resource-frugal |

**Recommendation: Authentik.** The one-line reason: groups come with the default claims —
zero custom sub+groups mapping (the thinnest server-side tier in the §二 division table), and its resource and backup cost is
controllable with "dual-track fallback + backing up its own DB".

Second option: 钉钉/企业微信. Their web login is an OAuth 2.0 form rather than full standard
OIDC (the id_token / standard scope conventions differ per vendor, to be verified), and the identity data lives in their public
cloud — conflicting with the hard constraint "data stays inside by default". The viable form is not memex connecting directly to a 钉钉
endpoint, but **a self-hosted IdP configured with 钉钉 QR-code scan as an upstream** (federation login), so the identity data still lands on the
self-hosted side; the extent of Authentik's upstream support for 钉钉 is to be verified. The 钉钉/飞书
provider already in the repo is "external to-do tool credential access" (appKey/appSecret→accessToken),
belonging to the task-tool surface, not the identity authentication surface, and cannot be used as an IdP.

## 十、Phasing and Testable Acceptance

| Phase | Content | Testable acceptance | Decision point |
|---|---|---|---|
| 0 | IdP container deployment + public client registration (PKCE) | discovery endpoint reachable from the intranet; login page reachable from the browser | IdP backup strategy; re-check the concrete choice |
| 1 | Server trio + identity_bindings table | A test IdP-issued id_token is exchanged for a memex session through LOGIN; tampered/expired/wrong aud/wrong nonce are all refused; password login regression fully green | — |
| 2 | Client authorization code + PKCE + dual-track login page | With an IdP configured, login succeeds with an IdP account; with no IdP configured, the entry does not appear and behavior matches today; password login succeeds; the OIDC source is visible in the login record | Login page default track (current ruling: password track default) |
| 3 | groups mapping + az compatibility-period adjudication (§六) | IdP group removal → the az adjudication changes after the next login; an old account's historical archive/files remain reachable after binding (no data loss) | Identity-source default values; migration batches |
| 4 | sub into archive and audit (§五) | New archive/audit rows carry sub; retrieval by sub works; the no-backfill convention for existing rows is explicit | — |

Not scheduled but recorded: L3 token issuing (§一), client credentials (§七), silent renewal
(§三).

## 十一、Pending List

- First-login binding confirmation method (§五): administrator pre-registration vs. first-login page confirmation;
- The concrete convention of the groups claim and which IdP groups count as memex groups (§六);
- The identity-source switch default value (§六, suggestion: local for existing, idp for new);
- Whether to persist the refresh token (§三, not persisted by default);
- The concrete commands of the IdP administrator recovery entry point (§八, verified by measurement after the choice is settled);
- Where email lands (§五, into a binding extension if the account table has no column);
- The form of the LOGIN frame extension (a new optional field vs. a new frame, decided by protocol compatibility at implementation time).

## 十二、Still Awaiting the User's Decision

1. **Whether password login is kept long term** — the current ruling is "default path, dual tracks coexisting long term";
   if it is to be turned off later (after everyone migrates), that is a separate decision; this document contains no schedule for turning it off;
2. **The IdP backup strategy** — into memex's existing daily backup (paired shots in the same window) or an independent
   backup window and retention count.
